// What the database fills in, and changes made together: identity keys, RETURNING, unique columns and transactions.
//
// - An identity key: an account's id is a `GENERATED` field, so `create_table` makes it `GENERATED ALWAYS AS IDENTITY`. Rows
//   are written without it (`make_insert_row`), and `insert_returning` gives back the ids the database chose.
// - A unique column: no two accounts have the same owner. A second account for one is refused (SQLSTATE 23505).
// - A transaction: a transfer takes from one account, gives to another and records itself, all of it or none. One that
//   would overdraw is rolled back.
// - RETURNING on an update and a delete: `execute` gives the rows as changed, instead of a count.
//
// Run with a libpq connection string: schemacht_example_transfers "host=localhost dbname=app user=app"
// It creates the tables schemacht_example_accounts and schemacht_example_transfers, and drops them when it is done.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <format>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "schemacht/schemacht.hpp"
#include "schemacht/util/compile_time.hpp"

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

// Sets an account's balance, and gives back its owner and new balance: one row per account changed.
constexpr auto SET_BALANCE = q::update<Accounts>()
                                 .set(AccountColumns::col<"balance">(), q::arg<0>())
                                 .where(AccountColumns::col<"id">() == q::arg<1>())
                                 .returning(AccountColumns::col<"owner">(), AccountColumns::col<"balance">());

// Closes an owner's account, and gives back every column of it as it was.
constexpr auto CLOSE = q::delete_where(AccountColumns::col<"owner">() == q::arg<0>()).returning();

constexpr auto ALL_ACCOUNTS = q::select<Accounts>().order_by(AccountColumns::col<"owner">().asc());

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

  const auto recorded = transaction.insert_returning<Transfers>({Transfers::make_insert_row(from, to, amount)}).get();
  transaction.commit().get();

  std::cout << "  committed as transfer #" << sch::get<"id">(recorded.at(0)) << '\n';
}

void drop_everything(pg::Database& database) {
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

    const auto closed = database.execute(q::Prepared<CLOSE>::bind("bob")).get();
    std::cout << "Closed bob's account #" << sch::get<"id">(closed.at(0)) << ", paying out " << dollars(sch::get<"balance">(closed.at(0))) << ".\n";

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
