// What the database fills in, and changes made together: identity keys, RETURNING, unique columns and transactions.
//
// - An identity key: an account's id is a `GENERATED` field, so `create_table` makes it `GENERATED ALWAYS AS IDENTITY`. Rows
//   are written without it (`make_insert_row`), and `insert_returning` gives back the ids the database chose.
// - A unique column: no two accounts have the same owner. A second account for one is refused (SQLSTATE 23505).
// - A transaction: a transfer takes from one account, gives to another and records itself, all of it or none. One that
//   would overdraw is rolled back.
// - RETURNING on an update and a delete: `execute` gives the rows as changed, instead of a count.
// - An upsert: each transfer adds to both accounts' activity, a row per account made by its first transfer and updated by the
//   rest. The counts add up (`MergeWith` an operator), and the first transfer's id is kept (`Keep`).
// - Steps chained by completions: the day's closing balances are read (`execute` with a completion) and, from that completion,
//   written to a table of their own (`insert`, with one too); the caller waits once, at the end. No worker waits on a future
//   in between, which could leave none free to finish it.
//
// Run with a libpq connection string: schemacht_example_transfers "host=localhost dbname=app user=app"
// It creates the tables schemacht_example_accounts, schemacht_example_transfers, schemacht_example_activity and
// schemacht_example_closing_balances, and drops them when it is done.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <format>
#include <future>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "schemacht/schemacht.hpp"
#include "schemacht/util/compile_time.hpp"
#include "schemacht/util/futures.hpp"

namespace sch = schemacht::schema;
namespace q = schemacht::query;
namespace pg = schemacht::postgres;

// The database numbers both tables' rows: a `GENERATED` key is never sent, and comes back from `insert_returning`.
using AccountId = sch::Field<std::int64_t, "id", sch::KeyRole::Primary, true>;
using Owner = sch::Field<std::string, "owner", sch::KeyRole::Unique>;
using Balance = sch::Field<std::int64_t, "balance">;  // in cents
using Accounts = sch::Schema<"schemacht_example_accounts", AccountId, Owner, Balance>;
using AccountColumns = q::On<Accounts>;

using TransferId = sch::Field<std::int64_t, "id", sch::KeyRole::Primary, true>;
using From = sch::Field<std::int64_t, "from_account">;
using To = sch::Field<std::int64_t, "to_account">;
using Amount = sch::Field<std::int64_t, "amount">;
using Transfers = sch::Schema<"schemacht_example_transfers", TransferId, From, To, Amount>;

// Each account's activity: how many transfers it took part in, how much they moved, and the first of them.
using ActiveAccount = sch::Field<std::int64_t, "account", sch::KeyRole::Primary>;
using TransferCount = sch::Field<std::int64_t, "transfers">;
using Moved = sch::Field<std::int64_t, "moved">;  // in cents, in and out
using FirstTransfer = sch::Field<std::int64_t, "first_transfer">;
using Activity = sch::Schema<"schemacht_example_activity", ActiveAccount, TransferCount, Moved, FirstTransfer>;

/** @brief Merges a count by adding the new one to the one there: `"current"."transfers" + excluded."transfers"`. */
struct Add {
  static constexpr std::string_view OPERATOR = "+";
};

// Each account's balance when the day closed.
using ClosedAccount = sch::Field<std::int64_t, "account", sch::KeyRole::Primary>;
using ClosedBalance = sch::Field<std::int64_t, "balance">;  // in cents
using ClosingBalances = sch::Schema<"schemacht_example_closing_balances", ClosedAccount, ClosedBalance>;

// Sets an account's balance, and gives back its owner and new balance: one row per account changed.
constexpr auto SET_BALANCE = q::update<Accounts>()
                                 .set(AccountColumns::col<"balance">(), q::arg<0>())
                                 .where(AccountColumns::col<"id">() == q::arg<1>())
                                 .returning(AccountColumns::col<"owner">(), AccountColumns::col<"balance">());

// Closes an owner's account, and gives back every column of it as it was.
constexpr auto CLOSE = q::delete_where(AccountColumns::col<"owner">() == q::arg<0>()).returning();

constexpr auto ALL_ACCOUNTS = q::select<Accounts>().order_by(AccountColumns::col<"owner">().asc());

constexpr auto ALL_ACTIVITY = q::select<Activity>().order_by(q::On<Activity>::col<"account">().asc());

namespace {

std::string dollars(std::int64_t cents) { return std::format("${}.{:02}", cents / 100, cents % 100); }

/** @return The row of `rows` whose `name` column is `value`. @pre there is one. */
template <schemacht::util::CTString name, typename row_t, typename value_t>
const row_t& find_by(const std::vector<row_t>& rows, const value_t& value) {
  return *std::ranges::find(rows, value, [](const row_t& row) { return sch::get<name>(row); });
}

/** @brief Moves `amount` cents from account `from` to account `to`, and records it; nothing at all if `from` has less than that. */
void transfer(pg::Database& database, std::int64_t from, std::int64_t to, std::int64_t amount) {
  // Serializable: had another transfer changed these accounts meanwhile, the commit would fail (SQLSTATE 40001) and nothing of
  // this one would happen; it would then be tried again.
  pg::Transaction transaction = database.begin({.isolation = pg::Isolation::Serializable}).get();

  const auto             accounts = transaction.select_by_key<Accounts>(std::vector{from, to}).get();
  const std::int64_t     from_balance = sch::get<"balance">(find_by<"id">(accounts, from));
  const std::int64_t     to_balance = sch::get<"balance">(find_by<"id">(accounts, to));
  const std::string_view from_owner = sch::get<"owner">(find_by<"id">(accounts, from));

  if ( from_balance < amount ) {
    std::cout << "  " << from_owner << " has only " << dollars(from_balance) << ": rolled back\n";
    transaction.rollback().get();  // nothing changed yet; a transaction dropped without a commit would be rolled back too

    return;
  }

  for ( const auto& [id, balance] : {std::pair{from, from_balance - amount}, std::pair{to, to_balance + amount}} ) {
    const auto changed = transaction.execute(q::Prepared<SET_BALANCE>::bind(balance, id)).get();
    std::cout << "  " << sch::get<"owner">(changed.at(0)) << ": " << dollars(sch::get<"balance">(changed.at(0))) << '\n';
  }

  const auto         recorded = transaction.insert_returning<Transfers>({Transfers::make_insert_row(from, to, amount)}).get();
  const std::int64_t transfer_id = sch::get<"id">(recorded.at(0));

  // Both accounts' activity: a row made for an account's first transfer, and added to by each one after; its first stays.
  transaction
      .upsert<Activity, pg::MergeWith<"transfers", Add>, pg::MergeWith<"moved", Add>, pg::Keep<"first_transfer">>(
          {Activity::make_row(from, 1, amount, transfer_id), Activity::make_row(to, 1, amount, transfer_id)}
      )
      .get();
  transaction.commit().get();

  std::cout << "  committed as transfer #" << transfer_id << '\n';
}

/**
 * @brief Records every account's balance at the end of the day: reads the accounts, and from that completion inserts the
 * snapshot. @return How many balances were recorded.
 */
std::uint64_t record_closing_balances(pg::Database& database) {
  std::promise<std::uint64_t> recorded;
  std::future<std::uint64_t>  done = recorded.get_future();

  database.execute(
      q::Prepared<ALL_ACCOUNTS>::bind(), [&database, &recorded](const std::vector<Accounts::row_type>& accounts, const std::exception_ptr& error) {
        if ( error ) {
          recorded.set_exception(error);
          return;
        }

        try {
          std::vector<ClosingBalances::row_type> snapshot;
          snapshot.reserve(accounts.size());
          for ( const Accounts::row_type& account : accounts ) {
            snapshot.push_back(ClosingBalances::make_row(sch::get<"id">(account), sch::get<"balance">(account)));
          }

          // From a worker, so no `.get()` here: the insert takes a completion too.
          database.insert<ClosingBalances>(snapshot, [&recorded](std::uint64_t inserted, const std::exception_ptr& insert_error) {
            schemacht::util::settle(recorded, insert_error, inserted);
          });
        } catch ( ... ) {  // no memory to start the insert: a worker drops what a completion throws, and the wait below would never end
          recorded.set_exception(std::current_exception());
        }
      }
  );

  return done.get();  // the one wait, on the caller's thread
}

void drop_everything(pg::Database& database) {
  database.drop_table<ClosingBalances>().get();
  database.drop_table<Activity>().get();
  database.drop_table<Transfers>().get();
  database.drop_table<Accounts>().get();
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const std::span<char*> args(argv, static_cast<std::size_t>(argc));
    const std::string      conninfo = args.size() > 1 ? args.subspan(1).front() : "";

    pg::Database database(conninfo);

    drop_everything(database);
    database.create_table<Accounts>().get();  // "id" bigint NOT NULL GENERATED ALWAYS AS IDENTITY, ..., UNIQUE ("owner")
    database.create_table<Transfers>().get();
    database.create_table<Activity>().get();
    database.create_table<ClosingBalances>().get();

    // Rows without their ids; the database numbers them. The rows may come back in any order: find each by its owner.
    const auto opened = database.insert_returning<Accounts>({Accounts::make_insert_row("ann", 10000), Accounts::make_insert_row("bob", 2500)}).get();
    const std::int64_t ann = sch::get<"id">(find_by<"owner">(opened, "ann"));
    const std::int64_t bob = sch::get<"id">(find_by<"owner">(opened, "bob"));
    std::cout << "Opened accounts #" << ann << " for ann and #" << bob << " for bob.\n";

    try {
      database.insert<Accounts>({Accounts::make_insert_row("ann", 0)}).get();
    } catch ( const pg::QueryError& error ) {
      std::cout << "A second account for ann is refused: SQLSTATE " << error.sqlstate() << ".\n";
    }

    std::cout << "ann sends bob " << dollars(4000) << ":\n";
    transfer(database, ann, bob, 4000);

    std::cout << "bob sends ann " << dollars(100000) << ":\n";
    transfer(database, bob, ann, 100000);

    std::cout << "bob sends ann " << dollars(500) << ":\n";
    transfer(database, bob, ann, 500);

    std::cout << "Activity:\n";
    for ( const Activity::row_type& active : database.execute(q::Prepared<ALL_ACTIVITY>::bind()).get() ) {
      std::cout << "  #" << sch::get<"account">(active) << "  " << sch::get<"transfers">(active) << " transfers, "
                << dollars(sch::get<"moved">(active)) << " moved, the first #" << sch::get<"first_transfer">(active) << '\n';
    }

    const auto closed = database.execute(q::Prepared<CLOSE>::bind("bob")).get();
    std::cout << "Closed bob's account #" << sch::get<"id">(closed.at(0)) << ", paying out " << dollars(sch::get<"balance">(closed.at(0))) << ".\n";

    std::cout << "Recorded the day's closing balances, one per account left: " << record_closing_balances(database) << ".\n";

    std::cout << "Accounts left:\n";
    for ( const Accounts::row_type& account : database.execute(q::Prepared<ALL_ACCOUNTS>::bind()).get() ) {
      std::cout << "  #" << sch::get<"id">(account) << "  " << sch::get<"owner">(account) << "  " << dollars(sch::get<"balance">(account)) << '\n';
    }

    drop_everything(database);

    return EXIT_SUCCESS;
  } catch ( const std::exception& error ) {
    std::cerr << "error: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
