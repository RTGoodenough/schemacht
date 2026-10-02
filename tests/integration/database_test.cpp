// Against a real PostgreSQL: run with SCHEMACHT_TEST_DB set to a libpq connection string, e.g.
//   SCHEMACHT_TEST_DB="host=localhost dbname=test user=test" ctest --test-dir build -R integration
// Without it, every test here is skipped. The tests create (and drop) tables whose names start with `schemacht_it_`.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <stdio.h>  // NOLINT(modernize-deprecated-headers) -- fileno is POSIX: declared here, not in <cstdio>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <future>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "schemacht/column_type.hpp"
#include "schemacht/schemacht.hpp"
#include "schemacht/util/compile_time.hpp"
#include "support/integration.hpp"

namespace sch = schemacht::schema;
namespace q = schemacht::query;
namespace pg = schemacht::postgres;

using namespace std::chrono_literals;

namespace {

using schemacht::test::exec;
using schemacht::test::test_db;

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
  exec(
      database, "CREATE TABLE " + table +
                    " (id bigint PRIMARY KEY, flag boolean NOT NULL, small smallint NOT NULL, count bigint NOT NULL, "
                    "real real NOT NULL, precise double precision NOT NULL, text text, bytes bytea NOT NULL, doc jsonb)"
  );
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

using Score = sch::Field<double, "score">;
using Note = sch::Field<std::optional<std::string>, "note">;
using Scored = sch::Schema<"schemacht_it_scored", Id, Score, Note>;
using ScoredColumns = q::On<Scored>;

constexpr auto ALL_SCORED = q::select<Scored>().order_by(ScoredColumns::col<"id">().asc());
constexpr auto RAISE_UP_TO = q::update<Scored>().set(ScoredColumns::col<"score">(), q::arg<0>()).where(ScoredColumns::col<"id">() <= q::arg<1>());
constexpr auto CLEAR_NOTES = q::update<Scored>().set_null(ScoredColumns::col<"note">()).all_rows();
constexpr auto DROP_BELOW = q::delete_where(ScoredColumns::col<"score">() < q::arg<0>());
constexpr auto DROP_ALL = q::delete_all<Scored>();

/** @brief An argument type whose text form is always refused. */
struct Unwritable {};

/** @brief Cents in a `numeric`, written with the library's `numeric` helpers. */
struct Cents {
  std::int64_t value = 0;

  auto operator<=>(const Cents&) const = default;
};

}  // namespace

namespace {

enum class Priority : std::uint8_t {
  Low,
  Normal,
  High,
};

/** @brief An exact amount, by the helper rather than by hand. */
struct Price {
  std::int64_t cents = 0;

  auto operator<=>(const Price&) const = default;
};

/** @brief A file's place, stored as text. */
struct Location {
  std::string path;

  auto operator<=>(const Location&) const = default;
};

}  // namespace

template <>
struct schemacht::ColumnType<Priority> : schemacht::EnumIntegerColumn<Priority> {};

template <>
struct schemacht::ColumnType<Price> : schemacht::ScaledNumericColumn<&Price::cents, 2, "numeric(12,2)"> {};

template <>
struct schemacht::ColumnType<Location> : schemacht::ConvertedColumn<
                                             Location, std::string, [](const Location& location) { return "file://" + location.path; },
                                             [](const std::string& text) {
                                               if ( ! text.starts_with("file://") ) {
                                                 throw std::invalid_argument("not a file URL: " + text);
                                               }
                                               return Location{text.substr(7)};
                                             }> {};

template <>
struct schemacht::ColumnType<Cents> {
  static constexpr std::string_view SQL_TYPE = "numeric(12,2)";
  static Cents                      parse(std::string_view text) { return {schemacht::postgres::parse_scaled_numeric<2>(text)}; }
  static std::string                format(const Cents& amount) { return schemacht::postgres::format_scaled_numeric<2>(amount.value); }
  static Cents          parse_binary(std::span<const std::byte> bytes) { return {schemacht::postgres::parse_binary_scaled_numeric<2>(bytes)}; }
  static constexpr bool binary_accepts(std::uint32_t type_oid) { return type_oid == schemacht::postgres::OID_NUMERIC; }
};

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

  const auto by_key = database.select_by_key<Types>(std::vector<std::int64_t>{1, 3}).get();
  REQUIRE(by_key.size() == 1);
  CHECK(sch::get<"id">(by_key[0]) == 1);
  CHECK(sch::get<"text">(by_key[0]) == std::get<Text>(sample_rows()[0]).value);
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

  database.insert<Bulk>(rows).get();
  CHECK(std::get<0>(database.execute(CountBulk::bind()).get().at(0)).value == 10000);

  std::vector<Tagged::row_type> tagged;
  tagged.reserve(40000);
  for ( std::int64_t id = 0; id < 40000; ++id ) {  // array column: VALUES statements of at most 65,535 parameters
    tagged.push_back({Id{id}, Tags{{"a", "b,c"}}});
  }

  database.insert<Tagged>(tagged).get();
  CHECK(std::get<0>(database.execute(CountTagged::bind()).get().at(0)).value == 40000);
}

TEST_CASE("integration: stopping a stream cancels the query and frees the connection", "[integration]") {
  // One connection: the query after the stream only runs if the stream's connection came back.
  pg::Database database(test_db(), pg::Database::Options{.pool = {.connections = 1, .workers = 2, .session = {}}, .read = {.chunk_rows = 100}});

  std::size_t seen = 0;
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
  CHECK_THROWS_WITH(
      database.execute(Misnamed::bind()).get(),
      "the statement returned the column 'wrong' at position 1, where its fields have 'n' (name the select list's columns with AS)"
  );
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

TEST_CASE("integration: commands report the rows they affected", "[integration]") {
  pg::Database database(test_db());

  using Drop = q::RawStatement<"DROP TABLE IF EXISTS schemacht_it_commands", q::RawArguments<>>;
  using Create = q::RawStatement<"CREATE TABLE schemacht_it_commands (id bigint PRIMARY KEY, score double precision NOT NULL)", q::RawArguments<>>;
  using Fill = q::RawStatement<"INSERT INTO schemacht_it_commands SELECT n, 0 FROM generate_series(1, $1) AS n", q::RawArguments<std::int64_t>>;
  using Raise = q::RawStatement<"UPDATE schemacht_it_commands SET score = $1 WHERE id <= $2", q::RawArguments<double, std::int64_t>>;
  using Remove = q::RawStatement<"DELETE FROM schemacht_it_commands WHERE score > $1", q::RawArguments<double>>;

  CHECK(database.execute(Drop::bind()).get() == 0);
  CHECK(database.execute(Create::bind()).get() == 0);
  CHECK(database.execute(Fill::bind(10)).get() == 10);
  CHECK(database.execute(Raise::bind(2.5, 4)).get() == 4);
  CHECK(database.execute(Remove::bind(1.0)).get() == 4);

  using Broken = q::RawStatement<"UPDATE schemacht_it_no_such_table SET x = 1", q::RawArguments<>>;
  CHECK_THROWS_AS(database.execute(Broken::bind()).get(), pg::QueryError);

  using Refused = q::RawStatement<"UPDATE schemacht_it_commands SET score = 0 WHERE $1::text = ''", q::RawArguments<Unwritable>>;
  CHECK_THROWS_WITH(database.execute(Refused::bind(Unwritable{})).get(), "this argument has no text form");

  CHECK(database.execute(Drop::bind()).get() == 0);
}

TEST_CASE("integration: typed updates and deletes", "[integration]") {
  pg::Database database(test_db());
  database.drop_table<Scored>().get();
  database.create_table<Scored>().get();
  CHECK_THROWS_AS(database.create_table<Scored>().get(), pg::QueryError);  // it exists

  database.insert<Scored>({Scored::make_row(1, 0.0, "a"), Scored::make_row(2, 0.0, "b"), Scored::make_row(3, 5.0, std::nullopt)}).get();

  CHECK(database.execute(q::Prepared<RAISE_UP_TO>::bind(2.5, 2)).get() == 2);
  CHECK(database.execute(q::Prepared<CLEAR_NOTES>::bind()).get() == 3);

  const auto rows = database.execute(q::Prepared<ALL_SCORED>::bind()).get();
  REQUIRE(rows.size() == 3);
  CHECK(sch::get<"score">(rows[0]) == 2.5);
  CHECK(sch::get<"score">(rows[2]) == 5.0);
  CHECK_FALSE(sch::get<"note">(rows[0]).has_value());

  CHECK(database.execute(q::Prepared<DROP_BELOW>::bind(3.0)).get() == 2);
  CHECK(database.execute(q::Prepared<DROP_ALL>::bind()).get() == 1);

  database.drop_table<Scored>().get();
  database.drop_table<Scored>().get();  // already gone: nothing to do
}

namespace {

using At = sch::Field<std::chrono::system_clock::time_point, "at">;
using Wall = sch::Field<std::chrono::local_time<std::chrono::microseconds>, "wall">;
using Day = sch::Field<std::optional<std::chrono::year_month_day>, "day">;
using Key = sch::Field<schemacht::Uuid, "key", sch::KeyRole::Primary>;
using Times = sch::Schema<"schemacht_it_times", Key, At, Wall, Day>;

constexpr auto ALL_TIMES = q::select<Times>().order_by(q::On<Times>::col<"key">().asc());

// What the server makes of literals, in its own text and binary forms.
using ServerTimes = q::RawStatement<
    "SELECT '00000000-0000-0000-0000-00000000000a'::uuid AS key, '2026-10-01 20:55:12.264-04'::timestamptz AS at, "
    "'2026-10-01 12:00:00.5'::timestamp AS wall, '0044-03-15 BC'::date AS day",
    q::RawArguments<>, Key, At, Wall, Day>;

}  // namespace

TEST_CASE("integration: dates, times and UUIDs round-trip, in binary and in text", "[integration]") {
  using namespace std::chrono;
  pg::Database database(test_db(), pg::Database::Options{.pool = {.connections = 1}});  // one connection: the SET below applies to all
  database.drop_table<Times>().get();
  database.create_table<Times>().get();

  const schemacht::Uuid          first = schemacht::Uuid::from_string("00000000-0000-0000-0000-000000000001");
  const schemacht::Uuid          second = schemacht::Uuid::from_string("ffffffff-0000-0000-0000-000000000002");
  const system_clock::time_point moment = sys_days{2026y / 10 / 1} + 20h + 55min + 12s + 264ms;
  const local_time<microseconds> wall = local_days{1999y / 12 / 31} + 23h + 59min + 59s + 999999us;
  database
      .insert<Times>(
          {Times::make_row(first, moment, wall, year_month_day{2026y / 2 / 28}),
           Times::make_row(second, system_clock::time_point::max(), wall, std::nullopt)}
      )
      .get();

  for ( const pg::ResultFormat format : {pg::ResultFormat::Binary, pg::ResultFormat::Text} ) {
    const auto rows = database.execute(q::Prepared<ALL_TIMES>::bind(), pg::ReadOptions{.format = format}).get();
    REQUIRE(rows.size() == 2);
    CHECK(sch::get<"key">(rows[0]) == first);
    CHECK(sch::get<"at">(rows[0]) == moment);
    CHECK(sch::get<"wall">(rows[0]) == wall);
    CHECK(sch::get<"day">(rows[0]) == year_month_day{2026y / 2 / 28});
    CHECK(sch::get<"at">(rows[1]) == system_clock::time_point::max());  // infinity
    CHECK_FALSE(sch::get<"day">(rows[1]).has_value());
  }

  // The server's own output, with a session time zone that is not UTC: its text has an offset of +05:30.
  CHECK(database.execute(q::RawStatement<"SET TIME ZONE 'Asia/Kolkata'", q::RawArguments<>>::bind()).get() == 0);

  for ( const pg::ResultFormat format : {pg::ResultFormat::Binary, pg::ResultFormat::Text} ) {
    const auto rows = database.execute(ServerTimes::bind(), pg::ReadOptions{.format = format}).get();
    REQUIRE(rows.size() == 1);
    CHECK(sch::get<"key">(rows[0]) == schemacht::Uuid::from_string("00000000-0000-0000-0000-00000000000a"));
    CHECK(sch::get<"at">(rows[0]) == sys_days{2026y / 10 / 2} + 55min + 12s + 264ms);
    CHECK(sch::get<"wall">(rows[0]) == local_days{2026y / 10 / 1} + 12h + 500ms);
    CHECK(sch::get<"day">(rows[0]) == year_month_day{year{-43}, March, day{15}});
  }

  database.drop_table<Times>().get();
}

TEST_CASE("integration: notices go to the handler, or nowhere", "[integration]") {
  using Raise = q::RawStatement<"DO 'BEGIN RAISE NOTICE ''hello from the server''; END'", q::RawArguments<>>;
  using Skip = q::RawStatement<"DROP TABLE IF EXISTS schemacht_it_never_there", q::RawArguments<>>;

  std::mutex               mutex;
  std::vector<std::string> received;  // severity and message of each
  pg::Database             heard(test_db(), pg::Database::Options{.pool = {.on_notice = [&](const pg::Notice& notice) {
                                                        const std::scoped_lock lock(mutex);
                                                        received.push_back(std::string(notice.severity) + ": " + std::string(notice.message));
                                                                  }}});

  heard.execute(Raise::bind()).get();

  {
    const std::scoped_lock lock(mutex);
    REQUIRE(received.size() == 1);
    CHECK(received.at(0) == "NOTICE: hello from the server");
  }

  // Without a handler, nothing reaches stderr (where libpq would print it).
  pg::Database                                          quiet(test_db());
  const std::unique_ptr<std::FILE, int (*)(std::FILE*)> capture(std::tmpfile(), &std::fclose);
  REQUIRE(capture != nullptr);
  const int saved = dup(STDERR_FILENO);
  REQUIRE(dup2(fileno(capture.get()), STDERR_FILENO) >= 0);

  quiet.execute(Raise::bind()).get();
  quiet.execute(Skip::bind()).get();

  REQUIRE(std::fflush(stderr) == 0);
  REQUIRE(dup2(saved, STDERR_FILENO) >= 0);
  close(saved);

  CHECK(std::ftell(capture.get()) == 0);
}

TEST_CASE("integration: a numeric is read exactly, in binary and in text", "[integration]") {
  pg::Database database(test_db());

  using Amount = sch::Field<Cents, "amount">;
  using Amounts = q::RawStatement<
      "SELECT v::numeric(12,2) AS amount FROM unnest(ARRAY['19.99', '-0.05', '0.05', '1200.5', '9999999999.99', '0']) "
      "WITH ORDINALITY AS t(v, n) ORDER BY n",
      q::RawArguments<>, Amount>;
  using Third = q::RawStatement<"SELECT 1::numeric / 3 AS amount", q::RawArguments<>, Amount>;
  using Echo = q::RawStatement<"SELECT $1::numeric(12,2) AS amount", q::RawArguments<Cents>, Amount>;

  for ( const pg::ResultFormat format : {pg::ResultFormat::Binary, pg::ResultFormat::Text} ) {
    const auto                rows = database.execute(Amounts::bind(), pg::ReadOptions{.format = format}).get();
    std::vector<std::int64_t> cents;
    cents.reserve(rows.size());
    for ( const auto& row : rows ) {
      cents.push_back(sch::get<"amount">(row).value);
    }

    CHECK(cents == std::vector<std::int64_t>{1999, -5, 5, 120050, 999999999999, 0});
    CHECK_THROWS_AS(
        database.execute(Third::bind(), pg::ReadOptions{.format = format}).get(),
        std::invalid_argument
    );  // 0.333...: refused, not rounded
    CHECK(sch::get<"amount">(database.execute(Echo::bind(Cents{-123456}), pg::ReadOptions{.format = format}).get().at(0)) == Cents{-123456});
  }
}

TEST_CASE("integration: the column type helpers round-trip, in binary and in text", "[integration]") {
  using Level = sch::Field<Priority, "level">;
  using Cost = sch::Field<Price, "cost">;
  using Where = sch::Field<std::optional<Location>, "place">;
  using Tasks = sch::Schema<"schemacht_it_tasks", Id, Level, Cost, Where>;
  constexpr auto ALL_TASKS = q::select<Tasks>().order_by(q::On<Tasks>::col<"id">().asc());
  constexpr auto URGENT = q::select(q::On<Tasks>::col<"level">() == q::arg<0>()).project(q::On<Tasks>::col<"id">());

  pg::Database database(test_db());
  database.drop_table<Tasks>().get();
  database.create_table<Tasks>().get();
  database
      .insert<Tasks>(
          {Tasks::make_row(1, Priority::High, Price{1999}, Location{"/tmp/a"}), Tasks::make_row(2, Priority::Low, Price{-5}, std::nullopt)}
      )
      .get();

  for ( const pg::ResultFormat format : {pg::ResultFormat::Binary, pg::ResultFormat::Text} ) {
    const auto rows = database.execute(q::Prepared<ALL_TASKS>::bind(), pg::ReadOptions{.format = format}).get();
    REQUIRE(rows.size() == 2);
    CHECK(sch::get<"level">(rows[0]) == Priority::High);
    CHECK(sch::get<"cost">(rows[0]) == Price{1999});
    CHECK(sch::get<"place">(rows[0]) == Location{"/tmp/a"});
    CHECK(sch::get<"cost">(rows[1]) == Price{-5});
    CHECK_FALSE(sch::get<"place">(rows[1]).has_value());
  }

  const auto urgent = database.execute(q::Prepared<URGENT>::bind(Priority::High)).get();
  REQUIRE(urgent.size() == 1);
  CHECK(sch::get<"id">(urgent[0]) == 1);

  using Stored = q::RawStatement<
      "SELECT level::text || ' ' || cost::text || ' ' || place AS stored FROM schemacht_it_tasks WHERE id = 1", q::RawArguments<>,
      sch::Field<std::string, "stored">>;
  CHECK(sch::get<"stored">(database.execute(Stored::bind()).get().at(0)) == "2 19.99 file:///tmp/a");  // as the database holds them

  database.drop_table<Tasks>().get();
}

TEST_CASE("integration: a table created from a schema keeps each unique column unique", "[integration]") {
  using Email = sch::Field<std::string, "email", sch::KeyRole::Unique>;
  using Phone = sch::Field<std::optional<std::string>, "phone", sch::KeyRole::Unique>;
  using Accounts = sch::Schema<"schemacht_it_accounts", Id, Email, Phone>;

  pg::Database database(test_db());
  database.drop_table<Accounts>().get();
  database.create_table<Accounts>().get();

  database.insert<Accounts>({Accounts::make_row(1, "a@example.com", "555-0100")}).get();
  database.insert<Accounts>({Accounts::make_row(2, "b@example.com", std::nullopt)}).get();  // each column on its own: a new email will do

  const auto refused = [&database](Accounts::row_type row) {
    try {
      database.insert<Accounts>({std::move(row)}).get();
      FAIL("the insert should have been refused");
    } catch ( const pg::QueryError& error ) {
      CHECK(error.sqlstate() == "23505");  // unique_violation
    }
  };

  refused(Accounts::make_row(3, "a@example.com", "555-0199"));  // the email again
  refused(Accounts::make_row(4, "c@example.com", "555-0100"));  // the phone again

  database.drop_table<Accounts>().get();
}

namespace {

using NumberedId = sch::Field<std::int64_t, "id", sch::KeyRole::Primary, true>;
using Label = sch::Field<std::string, "label">;
using Numbered = sch::Schema<"schemacht_it_numbered", NumberedId, Label>;
using Relabelled = sch::Schema<"schemacht_it_relabelled", NumberedId, Label>;
using RelabelledColumns = q::On<Relabelled>;

constexpr auto ALL_NUMBERED = q::select<Numbered>().order_by(q::On<Numbered>::col<"id">().asc());
constexpr auto RELABEL = q::update<Relabelled>()
                             .set(RelabelledColumns::col<"label">(), q::arg<0>())
                             .where(RelabelledColumns::col<"label">() == q::arg<1>())
                             .returning(RelabelledColumns::col<"label">(), RelabelledColumns::col<"id">());
constexpr auto REMOVE_LABELLED = q::delete_where(RelabelledColumns::col<"label">() == q::arg<0>()).returning();

}  // namespace

TEST_CASE("integration: a table whose key the database numbers gives the keys back", "[integration][returning]") {
  pg::Database database(test_db());
  database.drop_table<Numbered>().get();
  database.create_table<Numbered>().get();

  const auto written = database.insert_returning<Numbered>({Numbered::make_insert_row("a"), Numbered::make_insert_row("b")}).get();
  REQUIRE(written.size() == 2);
  CHECK(sch::get<"id">(written[0]) != sch::get<"id">(written[1]));

  // A whole row's generated value is not sent: the database numbers it all the same.
  const auto whole = database.insert_returning<Numbered>({Numbered::make_row(0, "c")}).get();
  REQUIRE(whole.size() == 1);
  CHECK(sch::get<"id">(whole[0]) > 0);
  CHECK(sch::get<"label">(whole[0]) == "c");

  // Read as text, the same.
  const auto as_text = database.insert_returning<Numbered>({Numbered::make_insert_row("d")}, pg::ReadOptions{.format = pg::ResultFormat::Text}).get();
  REQUIRE(as_text.size() == 1);
  CHECK(sch::get<"id">(as_text[0]) > sch::get<"id">(whole[0]));
  CHECK(sch::get<"label">(as_text[0]) == "d");
  CHECK(database.insert_returning<Numbered>(std::vector<Numbered::insert_row_type>{}).get().empty());

  // What came back is what was written.
  const auto rows = database.execute(q::Prepared<ALL_NUMBERED>::bind()).get();
  REQUIRE(rows.size() == 4);
  for ( const Numbered::row_type& row : written ) {
    const auto found = std::ranges::find(rows, sch::get<"id">(row), [](const Numbered::row_type& stored) { return sch::get<"id">(stored); });
    REQUIRE(found != rows.end());
    CHECK(sch::get<"label">(*found) == sch::get<"label">(row));
  }

  database.drop_table<Numbered>().get();
}

TEST_CASE("integration: updates and deletes give back the rows they change, in binary and in text", "[integration][returning]") {
  pg::Database database(test_db());
  database.drop_table<Relabelled>().get();
  database.create_table<Relabelled>().get();

  std::int64_t a_id = sch::get<"id">(database.insert_returning<Relabelled>({Relabelled::make_insert_row("a")}).get().at(0));
  database.insert<Relabelled>({Relabelled::make_insert_row("b")}).get();

  for ( const pg::ResultFormat format : {pg::ResultFormat::Binary, pg::ResultFormat::Text} ) {
    const pg::ReadOptions read{.format = format};
    const std::string     renamed = format == pg::ResultFormat::Binary ? "binary" : "text";

    const auto changed = database.execute(q::Prepared<RELABEL>::bind(renamed, "a"), read).get();
    REQUIRE(changed.size() == 1);
    CHECK(sch::get<"label">(changed[0]) == renamed);
    CHECK(sch::get<"id">(changed[0]) == a_id);

    const auto removed = database.execute(q::Prepared<REMOVE_LABELLED>::bind(renamed), read).get();
    REQUIRE(removed.size() == 1);
    CHECK(sch::get<"id">(removed[0]) == a_id);

    CHECK(database.execute(q::Prepared<REMOVE_LABELLED>::bind("nothing has this label"), read).get().empty());

    a_id = sch::get<"id">(database.insert_returning<Relabelled>({Relabelled::make_insert_row("a")}).get().at(0));  // for the next round
  }

  database.drop_table<Relabelled>().get();
}
