// Against a real PostgreSQL: run with SCHEMACHT_TEST_DB set to a libpq connection string, e.g.
//   SCHEMACHT_TEST_DB="host=localhost dbname=test user=test" ctest --test-dir build -R integration
// Without it, every test here is skipped. The tests create (and drop) tables whose names start with `schemacht_it_`.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <future>
#include <initializer_list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "schemacht/schemacht.hpp"
#include "schemacht/util/compile_time.hpp"

namespace sch = schemacht::schema;
namespace q = schemacht::query;
namespace pg = schemacht::postgres;

using namespace std::chrono_literals;

namespace {

std::string test_db() {
  const char* conninfo = std::getenv("SCHEMACHT_TEST_DB");
  if ( conninfo == nullptr ) {
    SKIP("SCHEMACHT_TEST_DB is not set");
  }
  return conninfo;
}

/** @brief Runs `sql` (no arguments, no rows wanted) and waits for it. */
void exec(pg::Database& database, std::string sql) {
  std::promise<void> done;
  database.pool().run(std::move(sql), {}, [&done](const pg::QueryResult& /*result*/, const std::exception_ptr& error) {
    if ( error ) {
      done.set_exception(error);
    } else {
      done.set_value();
    }
  });
  done.get_future().get();
}

std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
  std::vector<std::byte> result;
  for ( const unsigned value : values ) {
    result.push_back(static_cast<std::byte>(value));
  }
  return result;
}

using Id = sch::Field<std::int64_t, "id", sch::KeyRole::Primary>;
using Flag = sch::Field<bool, "flag">;
using Small = sch::Field<std::int16_t, "small">;
using Count = sch::Field<std::uint32_t, "count">;
using Real = sch::Field<float, "real">;
using Precise = sch::Field<double, "precise">;
using Text = sch::Field<std::optional<std::string>, "text">;
using Bytes = sch::Field<std::vector<std::byte>, "bytes">;
using Doc = sch::Field<std::optional<schemacht::json::Json>, "doc">;
// Each test has tables of its own: ctest runs the tests in parallel processes.
template <schemacht::util::CTString table>
using TypesIn = sch::Schema<table, Id, Flag, Small, Count, Real, Precise, Text, Bytes, Doc>;
using Types = TypesIn<"schemacht_it_types">;
using Bulk = TypesIn<"schemacht_it_bulk">;

/** @brief Drops and creates `table` with the columns of `TypesIn`. */
void create_types_table(pg::Database& database, const std::string& table) {
  exec(database, "DROP TABLE IF EXISTS " + table);
  exec(database, "CREATE TABLE " + table +
                     " (id bigint PRIMARY KEY, flag boolean NOT NULL, small smallint NOT NULL, count bigint NOT NULL, "
                     "real real NOT NULL, precise double precision NOT NULL, text text, bytes bytea NOT NULL, doc jsonb)");
}

using Tags = sch::Field<std::vector<std::string>, "tags">;
using Tagged = sch::Schema<"schemacht_it_tagged", Id, Tags>;

using CountBulk = q::RawStatement<"SELECT count(*) AS n FROM schemacht_it_bulk", q::RawArguments<>, sch::Field<std::int64_t, "n">>;
using CountTagged = q::RawStatement<"SELECT count(*) AS n FROM schemacht_it_tagged", q::RawArguments<>, sch::Field<std::int64_t, "n">>;
// A set-returning function in the select list streams its rows (in FROM, the server would make them all first).
using Series = q::RawStatement<"SELECT generate_series(1, $1) AS n", q::RawArguments<std::int64_t>, sch::Field<std::int64_t, "n">>;

constexpr auto ALL_TYPES = q::select<Types>().order_by(q::On<Types>::col<"id">().asc());

std::vector<Types::row_type> sample_rows() {
  return {
      {Id{1}, Flag{true}, Small{-32768}, Count{4000000000U}, Real{1.5F}, Precise{0.1}, Text{R"(with "quotes" and \)"}, Bytes{bytes({0, 1, 0xff})},
       Doc{schemacht::json::Json(R"({"a": 1})")}},
      {Id{2}, Flag{false}, Small{7}, Count{0}, Real{-0.25F}, Precise{1e300}, Text{std::nullopt}, Bytes{{}}, Doc{std::nullopt}},
  };
}

void check_sample(const std::vector<Types::row_type>& rows) {
  const std::vector<Types::row_type> expected = sample_rows();
  REQUIRE(rows.size() == expected.size());
  for ( std::size_t i = 0; i < rows.size(); ++i ) {
    CHECK(std::get<Id>(rows[i]).value == std::get<Id>(expected[i]).value);
    CHECK(std::get<Flag>(rows[i]).value == std::get<Flag>(expected[i]).value);
    CHECK(std::get<Small>(rows[i]).value == std::get<Small>(expected[i]).value);
    CHECK(std::get<Count>(rows[i]).value == std::get<Count>(expected[i]).value);
    CHECK(std::get<Real>(rows[i]).value == std::get<Real>(expected[i]).value);
    CHECK(std::get<Precise>(rows[i]).value == std::get<Precise>(expected[i]).value);
    CHECK(std::get<Text>(rows[i]).value == std::get<Text>(expected[i]).value);
    CHECK(std::get<Bytes>(rows[i]).value == std::get<Bytes>(expected[i]).value);
    CHECK(std::get<Doc>(rows[i]).value == std::get<Doc>(expected[i]).value);
  }
}

/** @brief An argument type whose text form is always refused. */
struct Unwritable {};

}  // namespace

template <>
class schemacht::postgres::FieldCodec<Unwritable> {
 public:
  static Unwritable  parse(std::string_view /*text*/) { return {}; }
  static std::string format(const Unwritable& /*value*/) { throw std::invalid_argument("this argument has no text form"); }
};

TEST_CASE("integration: every built-in type round-trips, in binary and in text", "[integration]") {
  pg::Database database(test_db());
  create_types_table(database, "schemacht_it_types");

  database.insert<Types>(sample_rows()).get();

  check_sample(database.execute(q::Prepared<ALL_TYPES>::bind()).get());
  check_sample(database.execute(q::Prepared<ALL_TYPES>::bind(), pg::ReadOptions{.format = pg::ResultFormat::Text}).get());

  const std::vector<std::int64_t> ids{2};
  const auto                      found = database.select_by_key_values<Types, "id">(ids).get();
  REQUIRE(found.size() == 1);
  CHECK(std::get<Id>(found[0]).value == 2);
}

TEST_CASE("integration: an insert larger than one statement's parameter limit", "[integration]") {
  pg::Database database(test_db());
  create_types_table(database, "schemacht_it_bulk");
  exec(database, "DROP TABLE IF EXISTS schemacht_it_tagged");
  exec(database, "CREATE TABLE schemacht_it_tagged (id bigint PRIMARY KEY, tags text[] NOT NULL)");

  std::vector<Bulk::row_type> rows;
  rows.reserve(10000);
  for ( std::int64_t id = 0; id < 10000; ++id ) {  // 9 columns: 90,000 values in one unnest statement
    rows.push_back({Id{id}, Flag{true}, Small{1}, Count{2}, Real{3}, Precise{4}, Text{"t"}, Bytes{bytes({1})}, Doc{std::nullopt}});
  }
  database.insert<Bulk>(std::move(rows)).get();
  CHECK(std::get<0>(database.execute(CountBulk::bind()).get().at(0)).value == 10000);

  std::vector<Tagged::row_type> tagged;
  tagged.reserve(40000);
  for ( std::int64_t id = 0; id < 40000; ++id ) {  // array column: VALUES statements of at most 65,535 parameters
    tagged.push_back({Id{id}, Tags{{"a", "b,c"}}});
  }
  database.insert<Tagged>(std::move(tagged)).get();
  CHECK(std::get<0>(database.execute(CountTagged::bind()).get().at(0)).value == 40000);
}

TEST_CASE("integration: stopping a stream cancels the query and frees the connection", "[integration]") {
  // One connection: the query after the stream only runs if the stream's connection came back.
  pg::Database database(test_db(), pg::Database::Options{.pool = {.connections = 1, .workers = 2, .session = {}}, .read = {.chunk_rows = 100}});
  std::size_t  seen = 0;
  database.stream(Series::bind(100000000), [&seen](auto&& /*row*/) { return ++seen < 250; }).get();
  CHECK(seen == 250);
  CHECK(std::get<0>(database.execute(Series::bind(3)).get().at(2)).value == 3);
}

TEST_CASE("integration: destroying the database mid-stream ends the stream", "[integration]") {
  auto                     database = std::make_unique<pg::Database>(test_db(), pg::Database::Options{.pool = {}, .read = {.chunk_rows = 10}});
  std::atomic<std::size_t> seen{0};
  std::future<void>        streamed = database->stream(Series::bind(1000000), [&seen](auto&& /*row*/) {
    ++seen;
    std::this_thread::sleep_for(100us);  // a slow consumer: chunks are still being handled when the database goes
  });
  while ( seen < 50 ) {
    std::this_thread::sleep_for(1ms);
  }
  database.reset();
  REQUIRE(streamed.wait_for(10s) == std::future_status::ready);
  CHECK_THROWS_WITH(streamed.get(), "the Postgres client is shutting down");
}

TEST_CASE("integration: a lost connection is re-established, with its session settings", "[integration]") {
  using BackendPid = q::RawStatement<"SELECT pg_backend_pid() AS pid", q::RawArguments<>, sch::Field<std::int32_t, "pid">>;
  using Terminate = q::RawStatement<"SELECT pg_terminate_backend($1) AS ok", q::RawArguments<std::int32_t>, sch::Field<bool, "ok">>;
  using Jit = q::RawStatement<"SELECT current_setting('jit') AS jit", q::RawArguments<>, sch::Field<std::string, "jit">>;

  pg::Database database(test_db(), pg::Database::Options{.pool = {.connections = 1, .workers = 2, .session = {}}, .read = {}});
  pg::Database other(test_db());

  const std::int32_t pid = std::get<0>(database.execute(BackendPid::bind()).get().at(0)).value;
  REQUIRE(std::get<0>(other.execute(Terminate::bind(pid)).get().at(0)).value);

  CHECK_THROWS(database.execute(BackendPid::bind()).get());  // the statement that finds the link gone fails

  // The reactor reconnects without blocking a worker; the next statement waits for it, and runs on a new session.
  const std::int32_t new_pid = std::get<0>(database.execute(BackendPid::bind()).get().at(0)).value;
  CHECK(new_pid != pid);
  CHECK(std::get<0>(database.execute(Jit::bind()).get().at(0)).value == "off");
}

TEST_CASE("integration: errors carry what went wrong", "[integration]") {
  pg::Database database(test_db());

  using Missing = q::RawStatement<"SELECT 1 AS n FROM schemacht_it_no_such_table", q::RawArguments<>, sch::Field<int, "n">>;
  try {
    database.execute(Missing::bind()).get();
    FAIL("the statement should have failed");
  } catch ( const pg::QueryError& error ) {
    CHECK(error.sqlstate() == "42P01");
  }

  using Misnamed = q::RawStatement<"SELECT 1 AS wrong", q::RawArguments<>, sch::Field<int, "n">>;
  CHECK_THROWS_WITH(database.execute(Misnamed::bind()).get(),
                    "the statement returned the column 'wrong' at position 1, where its fields have 'n' (name the select list's columns with AS)");
}

TEST_CASE("integration: a statement that cannot start reports why, through its completion", "[integration]") {
  pg::Database database(test_db());

  using Refused = q::RawStatement<"SELECT $1::text AS n", q::RawArguments<Unwritable>, sch::Field<std::string, "n">>;
  CHECK_THROWS_WITH(database.execute(Refused::bind(Unwritable{})).get(), "this argument has no text form");

  std::promise<std::pair<std::thread::id, bool>> called;  // on which thread, and with an error
  database.execute_into(Refused::bind(Unwritable{}), [&called](const std::vector<Refused::row_type>& /*rows*/, const std::exception_ptr& error) {
    called.set_value({std::this_thread::get_id(), error != nullptr});
  });
  const auto [thread, failed] = called.get_future().get();
  CHECK(thread != std::this_thread::get_id());  // a worker thread, as documented
  CHECK(failed);

  CHECK_THROWS_AS(database.execute(Series::bind(1), pg::ReadOptions{.chunk_rows = 0}).get(), std::invalid_argument);
}
