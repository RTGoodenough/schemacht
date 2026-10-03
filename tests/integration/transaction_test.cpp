// Transactions against a real PostgreSQL; skipped without SCHEMACHT_TEST_DB (see database_test.cpp). Each test has tables of
// its own, whose names start with `schemacht_it_tx_`: ctest runs the tests in parallel processes.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "schemacht/schemacht.hpp"
#include "schemacht/util/compile_time.hpp"
#include "support/integration.hpp"

namespace sch = schemacht::schema;
namespace q = schemacht::query;
namespace pg = schemacht::postgres;

using namespace std::chrono_literals;
using schemacht::test::test_db;

namespace {

using Id = sch::Field<std::int64_t, "id", sch::KeyRole::Primary>;
using Balance = sch::Field<std::int64_t, "balance">;

template <schemacht::util::CTString table>
using AccountsIn = sch::Schema<table, Id, Balance>;

using Series = q::RawStatement<"SELECT generate_series(1, $1) AS n", q::RawArguments<std::int64_t>, sch::Field<std::int64_t, "n">>;
using BackendPid = q::RawStatement<"SELECT pg_backend_pid() AS pid", q::RawArguments<>, sch::Field<std::int32_t, "pid">>;
using Isolation = q::RawStatement<"SELECT current_setting('transaction_isolation') AS level", q::RawArguments<>, sch::Field<std::string, "level">>;

/** @brief Drops and creates `schema_t`'s table. */
template <sch::IsSchema schema_t>
void reset(pg::Database& database) {
  database.drop_table<schema_t>().get();
  database.create_table<schema_t>().get();
}

/** @return How many rows of `schema_t` `runner` (the database, or a transaction) sees. */
template <sch::IsSchema schema_t>
std::size_t count_rows(pg::StatementRunner& runner) {
  static constexpr auto ALL = q::select<schema_t>();
  return runner.execute(q::Prepared<ALL>::bind()).get().size();
}

/** @return The balance of account `id`, as `runner` sees it. */
template <sch::IsSchema schema_t>
std::int64_t balance_of(pg::StatementRunner& runner, std::int64_t id) {
  return sch::get<"balance">(runner.select_by_key<schema_t>(std::vector<std::int64_t>{id}).get().at(0));
}

template <sch::IsSchema schema_t>
constexpr auto SET_BALANCE =
    q::update<schema_t>().set(q::On<schema_t>::template col<"balance">(), q::arg<0>()).where(q::On<schema_t>::template col<"id">() == q::arg<1>());

/** @return The SQLSTATE of the `QueryError` that `future` holds, or "" if it holds none. */
template <typename value_t>
std::string sqlstate_of(std::future<value_t> future) {
  try {
    future.get();
  } catch ( const pg::QueryError& error ) {
    return error.sqlstate();
  }
  return "";
}

}  // namespace

TEST_CASE("integration: a committed transaction's changes are seen, and a rolled back one's are not", "[integration][transaction]") {
  using Accounts = AccountsIn<"schemacht_it_tx_commit">;
  pg::Database database(test_db());
  reset<Accounts>(database);

  pg::Transaction transaction = database.begin().get();
  CHECK(transaction.is_open());

  transaction.insert<Accounts>({Accounts::make_row(1, 100)}).get();
  transaction.insert<Accounts>({Accounts::make_row(2, 200)}).get();

  CHECK(count_rows<Accounts>(transaction) == 2);  // it sees its own changes
  CHECK(count_rows<Accounts>(database) == 0);     // nobody else does, yet

  transaction.commit().get();
  CHECK_FALSE(transaction.is_open());
  CHECK(count_rows<Accounts>(database) == 2);

  pg::Transaction undone = database.begin().get();
  undone.insert<Accounts>({Accounts::make_row(3, 300)}).get();
  CHECK(undone.execute(q::Prepared<SET_BALANCE<Accounts>>::bind(0, 1)).get() == 1);

  undone.rollback().get();
  CHECK(count_rows<Accounts>(database) == 2);
  CHECK(balance_of<Accounts>(database, 1) == 100);

  database.drop_table<Accounts>().get();
}

TEST_CASE("integration: a transaction dropped without a commit is rolled back", "[integration][transaction]") {
  using Accounts = AccountsIn<"schemacht_it_tx_dropped">;
  // One connection: the count after the drop runs on the transaction's connection, which must be out of the transaction.
  pg::Database database(test_db(), pg::Database::Options{.pool = {.connections = 1, .workers = 2, .session = {}}, .read = {}});
  reset<Accounts>(database);

  {
    pg::Transaction transaction = database.begin().get();
    transaction.insert<Accounts>({Accounts::make_row(1, 100)}).get();
  }

  CHECK(count_rows<Accounts>(database) == 0);

  // Moving one into another that is open rolls back the one moved into.
  pg::Transaction first = database.begin().get();
  first.insert<Accounts>({Accounts::make_row(2, 200)}).get();

  first = pg::Transaction(std::move(first));  // a move into itself, through a temporary: still the same transaction
  CHECK(first.is_open());

  first.commit().get();
  CHECK(count_rows<Accounts>(database) == 1);

  database.drop_table<Accounts>().get();
}

TEST_CASE("integration: after a failed statement, a transaction refuses everything and rolls back", "[integration][transaction]") {
  using Accounts = AccountsIn<"schemacht_it_tx_failed">;
  pg::Database database(test_db());
  reset<Accounts>(database);

  pg::Transaction transaction = database.begin().get();
  transaction.insert<Accounts>({Accounts::make_row(1, 100)}).get();

  CHECK(sqlstate_of(transaction.insert<Accounts>({Accounts::make_row(1, 100)})) == "23505");  // the key again
  CHECK(sqlstate_of(transaction.execute(Series::bind(1))) == "25P02");                        // in_failed_sql_transaction
  CHECK(sqlstate_of(transaction.commit()) == "25P02");                                        // PostgreSQL rolled it back instead

  CHECK_FALSE(transaction.is_open());
  CHECK(count_rows<Accounts>(database) == 0);

  database.drop_table<Accounts>().get();
}

TEST_CASE("integration: a transaction that has ended refuses every statement", "[integration][transaction]") {
  using Accounts = AccountsIn<"schemacht_it_tx_ended">;
  pg::Database database(test_db());
  reset<Accounts>(database);

  pg::Transaction transaction = database.begin().get();
  transaction.commit().get();

  CHECK_THROWS_AS(transaction.execute(Series::bind(1)).get(), std::logic_error);
  CHECK_THROWS_AS(transaction.insert<Accounts>({Accounts::make_row(1, 1)}).get(), std::logic_error);
  CHECK_THROWS_AS(transaction.commit().get(), std::logic_error);
  CHECK_THROWS_AS(transaction.rollback().get(), std::logic_error);

  pg::Transaction source = database.begin().get();
  pg::Transaction target = std::move(source);

  // What a moved-from one does:
  // NOLINTNEXTLINE(bugprone-use-after-move,hicpp-invalid-access-moved)
  CHECK_THROWS_AS(source.execute(Series::bind(1)).get(), std::logic_error);

  target.insert<Accounts>({Accounts::make_row(1, 1)}).get();
  target.commit().get();
  CHECK(count_rows<Accounts>(database) == 1);

  database.drop_table<Accounts>().get();
}

TEST_CASE("integration: a transaction's isolation level and read-only mode", "[integration][transaction]") {
  using Accounts = AccountsIn<"schemacht_it_tx_options">;
  pg::Database database(test_db());
  reset<Accounts>(database);

  pg::Transaction serializable = database.begin({.isolation = pg::Isolation::Serializable}).get();
  CHECK(sch::get<"level">(serializable.execute(Isolation::bind()).get().at(0)) == "serializable");
  serializable.commit().get();

  pg::Transaction repeatable = database.begin({.isolation = pg::Isolation::RepeatableRead}).get();
  CHECK(sch::get<"level">(repeatable.execute(Isolation::bind()).get().at(0)) == "repeatable read");
  repeatable.rollback().get();

  pg::Transaction reader = database.begin({.isolation = pg::Isolation::ReadCommitted, .read_only = true}).get();
  CHECK(count_rows<Accounts>(reader) == 0);
  CHECK(sqlstate_of(reader.insert<Accounts>({Accounts::make_row(1, 1)})) == "25006");  // read_only_sql_transaction

  database.drop_table<Accounts>().get();
}

TEST_CASE("integration: calls on a transaction run in the order they were made", "[integration][transaction]") {
  using Accounts = AccountsIn<"schemacht_it_tx_order">;
  pg::Database database(test_db());
  reset<Accounts>(database);

  pg::Transaction            transaction = database.begin().get();
  std::future<std::uint64_t> inserted = transaction.insert<Accounts>({Accounts::make_row(1, 100)});
  std::future<std::uint64_t> updated = transaction.execute(q::Prepared<SET_BALANCE<Accounts>>::bind(5, 1));  // without waiting for the insert
  std::future<void>          committed = transaction.commit();

  inserted.get();
  CHECK(updated.get() == 1);
  committed.get();
  CHECK(balance_of<Accounts>(database, 1) == 5);

  database.drop_table<Accounts>().get();
}

TEST_CASE("integration: a stream stopped early in a transaction leaves the transaction usable", "[integration][transaction]") {
  pg::Database    database(test_db(), pg::Database::Options{.pool = {}, .read = {.chunk_rows = 100}});
  pg::Transaction transaction = database.begin().get();
  std::size_t     seen = 0;

  transaction.stream(Series::bind(100000), [&seen](auto&& /*row*/) { return ++seen < 250; }).get();
  CHECK(seen == 250);
  CHECK(transaction.execute(Series::bind(3)).get().size() == 3);  // not cancelled: the transaction did not fail

  transaction.commit().get();
}

TEST_CASE("integration: a transaction holds its connection until it ends", "[integration][transaction]") {
  pg::Database    database(test_db(), pg::Database::Options{.pool = {.connections = 1, .workers = 2, .session = {}}, .read = {}});
  pg::Transaction transaction = database.begin().get();

  auto waiting = database.execute(Series::bind(2));  // the pool's only connection is the transaction's
  auto next = database.begin();
  CHECK(waiting.wait_for(200ms) == std::future_status::timeout);
  CHECK(next.wait_for(0ms) == std::future_status::timeout);

  transaction.commit().get();

  REQUIRE(waiting.wait_for(10s) == std::future_status::ready);
  CHECK(waiting.get().size() == 2);

  pg::Transaction second = next.get();
  second.rollback().get();
}

TEST_CASE("integration: an insert of many rows is atomic", "[integration][transaction]") {
  using Tags = sch::Field<std::vector<std::string>, "tags">;
  using Tagged = sch::Schema<"schemacht_it_tx_tagged", Id, Tags>;
  pg::Database database(test_db());
  reset<Tagged>(database);

  // 40,000 rows of a schema with an array column, in one statement: the last has the first's key again.
  std::vector<Tagged::row_type> rows;
  rows.reserve(40000);
  for ( std::int64_t id = 0; id < 40000; ++id ) {
    rows.push_back(Tagged::make_row(id == 39999 ? 0 : id, std::vector<std::string>{"a"}));
  }

  CHECK(sqlstate_of(database.insert<Tagged>(rows)) == "23505");
  CHECK(count_rows<Tagged>(database) == 0);  // none of them is written

  // In a transaction, the insert is part of it.
  pg::Transaction transaction = database.begin().get();
  rows.back() = Tagged::make_row(39999, std::vector<std::string>{"a"});
  transaction.insert<Tagged>(rows).get();

  CHECK(count_rows<Tagged>(transaction) == 40000);
  transaction.rollback().get();
  CHECK(count_rows<Tagged>(database) == 0);

  database.drop_table<Tagged>().get();
}

TEST_CASE("integration: a transaction whose connection is lost fails, and the connection comes back", "[integration][transaction]") {
  using Terminate = q::RawStatement<"SELECT pg_terminate_backend($1) AS ok", q::RawArguments<std::int32_t>, sch::Field<bool, "ok">>;
  pg::Database database(test_db(), pg::Database::Options{.pool = {.connections = 1, .workers = 2, .session = {}}, .read = {}});
  pg::Database other(test_db());

  pg::Transaction    transaction = database.begin().get();
  const std::int32_t pid = sch::get<"pid">(transaction.execute(BackendPid::bind()).get().at(0));
  REQUIRE(sch::get<"ok">(other.execute(Terminate::bind(pid)).get().at(0)));

  CHECK_THROWS(transaction.execute(BackendPid::bind()).get());  // the statement that finds the link gone
  CHECK_THROWS_WITH(transaction.execute(BackendPid::bind()).get(), "the leased connection's link to Postgres was lost (a transaction on it is over)");
  CHECK_THROWS(transaction.commit().get());

  // Released, the connection is re-established and goes back to the pool.
  CHECK(sch::get<"pid">(database.execute(BackendPid::bind()).get().at(0)) != pid);
}

TEST_CASE("integration: a large insert in a transaction runs before the calls after it", "[integration][transaction]") {
  using Tags = sch::Field<std::vector<std::string>, "tags">;
  using Tagged = sch::Schema<"schemacht_it_tx_pipelined", Id, Tags>;
  pg::Database database(test_db());
  reset<Tagged>(database);

  std::vector<Tagged::row_type> rows;
  rows.reserve(40000);
  for ( std::int64_t id = 0; id < 40000; ++id ) {
    rows.push_back(Tagged::make_row(id, std::vector<std::string>{"a"}));
  }

  static constexpr auto ALL = q::select<Tagged>();

  // Nothing waits: the count and the COMMIT are handed in while the insert is still running.
  pg::Transaction            transaction = database.begin().get();
  std::future<std::uint64_t> inserted = transaction.insert<Tagged>(rows);
  auto                       counted = transaction.execute(q::Prepared<ALL>::bind());
  std::future<void>          committed = transaction.commit();

  inserted.get();
  CHECK(counted.get().size() == 40000);  // after the insert
  committed.get();
  CHECK(count_rows<Tagged>(database) == 40000);

  database.drop_table<Tagged>().get();
}

TEST_CASE("integration: a lease let go of inside a transaction comes back out of it", "[integration][transaction]") {
  using Accounts = AccountsIn<"schemacht_it_tx_lease">;
  // One connection: the pool's statement after the lease runs on the lease's connection.
  pg::Database database(test_db(), pg::Database::Options{.pool = {.connections = 1, .workers = 2, .session = {}}, .read = {}});
  reset<Accounts>(database);
  const std::int32_t pid = sch::get<"pid">(database.execute(BackendPid::bind()).get().at(0));

  {
    std::promise<pg::AsyncClient::Lease> granted;
    database.pool().lease([&granted](pg::AsyncClient::Lease lease, const std::exception_ptr& /*error*/) { granted.set_value(std::move(lease)); });
    const pg::AsyncClient::Lease lease = granted.get_future().get();

    std::promise<void> inserted;
    database.pool().run(lease, "BEGIN", {}, [](const pg::QueryResult& /*result*/, const std::exception_ptr& /*error*/) {});
    database.pool().run(
        lease, "INSERT INTO schemacht_it_tx_lease VALUES (1, 1)", {},
        [&inserted](const pg::QueryResult& /*result*/, const std::exception_ptr& error) {
          error ? inserted.set_exception(error) : inserted.set_value();
        }
    );

    inserted.get_future().get();
  }  // let go of without COMMIT or ROLLBACK

  CHECK(count_rows<Accounts>(database) == 0);                                       // not inside the leftover transaction
  CHECK(sch::get<"pid">(database.execute(BackendPid::bind()).get().at(0)) != pid);  // the connection was re-established

  std::promise<void> refused;
  database.pool().run(pg::AsyncClient::Lease(), "SELECT 1", {}, [&refused](const pg::QueryResult& /*result*/, const std::exception_ptr& error) {
    error ? refused.set_exception(error) : refused.set_value();
  });

  CHECK_THROWS_AS(refused.get_future().get(), std::logic_error);  // an empty lease runs nothing

  database.drop_table<Accounts>().get();
}

TEST_CASE("integration: a large insert gives back every row, alone or in a transaction", "[integration][transaction][returning]") {
  using NumberedId = sch::Field<std::int64_t, "id", sch::KeyRole::Primary, true>;
  using Tags = sch::Field<std::vector<std::string>, "tags">;
  using Label = sch::Field<std::string, "label">;
  using NumberedTags = sch::Schema<"schemacht_it_tx_numbered_tags", NumberedId, Tags, Label>;
  pg::Database database(test_db());
  reset<NumberedTags>(database);

  // 40,000 rows of a schema with an array column: one statement, as any insert is.
  std::vector<NumberedTags::insert_row_type> rows;
  rows.reserve(40000);
  for ( std::int64_t row = 0; row < 40000; ++row ) {
    rows.push_back(NumberedTags::make_insert_row(std::vector<std::string>{"a"}, std::to_string(row)));
  }

  // PostgreSQL does not promise the order of the rows it returns: compare which came back, not where.
  const auto labels_of = [](const std::vector<NumberedTags::row_type>& returned) {
    std::set<std::string> labels;
    for ( const NumberedTags::row_type& row : returned ) {
      labels.insert(sch::get<"label">(row));
    }
    return labels;
  };
  const auto ids_of = [](const std::vector<NumberedTags::row_type>& returned) {
    return std::ranges::minmax(returned | std::views::transform([](const NumberedTags::row_type& row) { return sch::get<"id">(row); }));
  };

  const auto written = database.insert_returning<NumberedTags>(rows).get();
  CHECK(written.size() == 40000);
  CHECK(labels_of(written).size() == 40000);

  pg::Transaction transaction = database.begin().get();
  const auto      again = transaction.insert_returning<NumberedTags>(rows).get();
  CHECK(labels_of(again) == labels_of(written));
  CHECK(ids_of(again).min > ids_of(written).max);  // numbered after the first ones
  transaction.commit().get();

  CHECK(count_rows<NumberedTags>(database) == 80000);

  database.drop_table<NumberedTags>().get();
}
