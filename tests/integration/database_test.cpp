// Against a real PostgreSQL: run with SCHEMACHT_TEST_DB set to a libpq connection string, e.g.
//   SCHEMACHT_TEST_DB="host=localhost dbname=test user=test" ctest --test-dir build -R integration
// Without it, every test here is skipped. The tests create (and drop) tables whose names start with `schemacht_it_`, and the
// schemas `schemacht_it_schema` and `schemacht_it_upsert`; the extension test also installs pg_trgm (and leaves it), and the tests
// create and drop the functions `similar_above` and `schemacht_it_keep_longer`.

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
#include <tuple>
#include <utility>
#include <vector>

#include "schemacht/column_type.hpp"
#include "schemacht/schemacht.hpp"
#include "schemacht/util/compile_time.hpp"
#include "schemacht/util/futures.hpp"
#include "support/integration.hpp"
#include "support/schemas.hpp"

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
  CHECK(database.table_differences<Types>().get().empty());  // the table written out above is the schema's

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

TEST_CASE("integration: a large insert is one statement, whatever the schema", "[integration]") {
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
  for ( std::int64_t id = 0; id < 40000; ++id ) {  // an array column: in the one statement too, each value an element of a text[]
    tagged.push_back({Id{id}, Tags{{"a", "b,c"}}});
  }

  database.insert<Tagged>(tagged).get();
  CHECK(std::get<0>(database.execute(CountTagged::bind()).get().at(0)).value == 40000);
}

namespace {

/** @brief An array inside a column type of one's own: stored as `text[]`. */
struct Labels {
  std::vector<std::string> values;

  auto operator<=>(const Labels&) const = default;
};

}  // namespace

template <>
struct schemacht::ColumnType<Labels> : schemacht::WrappedColumn<&Labels::values> {};

TEST_CASE("integration: array values round-trip through the insert, whatever they hold", "[integration][insert]") {
  using Ranks = sch::Field<std::optional<std::vector<std::int32_t>>, "ranks">;
  using Blobs = sch::Field<std::vector<std::vector<std::byte>>, "blobs">;
  using Docs = sch::Field<std::vector<schemacht::json::Json>, "docs">;
  using Keys = sch::Field<std::optional<std::vector<schemacht::Uuid>>, "keys">;
  using Levels = sch::Field<std::vector<Priority>, "levels">;
  using Amounts = sch::Field<std::vector<Cents>, "amounts">;
  using Wrapped = sch::Field<Labels, "labels">;
  using Lists = sch::Schema<"schemacht_it_lists", Id, Tags, Ranks, Blobs, Docs, Keys, Levels, Amounts, Wrapped>;
  pg::Database database(test_db());
  database.drop_table<Lists>().get();
  database.create_table<Lists>().get();
  CHECK(database.table_differences<Lists>().get().empty());  // arrays of every kind

  const std::vector<Lists::row_type> rows{
      Lists::make_row(
          1, std::vector<std::string>{R"(a "quoted")", "b,c", "{braced}", R"(back\slash)", "", "NULL"}, std::vector<std::int32_t>{1, -2},
          std::vector{bytes({0x5c, 0x22, 0x00}), bytes({})}, std::vector{schemacht::json::Json(R"({"k": "v \"q\""})")},
          std::vector{schemacht::Uuid::from_string("00000000-0000-0000-0000-000000000001")}, std::vector{Priority::High, Priority::Low},
          std::vector{Cents{1999}, Cents{-5}}, Labels{{"x", "y,z"}}
      ),
      Lists::make_row(
          2, std::vector<std::string>{}, std::nullopt, std::vector<std::vector<std::byte>>{}, std::vector<schemacht::json::Json>{}, std::nullopt,
          std::vector<Priority>{}, std::vector<Cents>{}, Labels{}
      ),
  };
  CHECK(database.insert<Lists>(rows).get() == 2);

  static constexpr auto ALL = q::select<Lists>().order_by(q::On<Lists>::col<"id">().asc());
  for ( const pg::ResultFormat format : {pg::ResultFormat::Binary, pg::ResultFormat::Text} ) {
    const std::vector<Lists::row_type> read = database.execute(q::Prepared<ALL>::bind(), pg::ReadOptions{.format = format}).get();
    REQUIRE(read.size() == rows.size());
    for ( std::size_t row = 0; row < rows.size(); ++row ) {
      CHECK(sch::get<"tags">(read.at(row)) == sch::get<"tags">(rows.at(row)));
      CHECK(sch::get<"ranks">(read.at(row)) == sch::get<"ranks">(rows.at(row)));
      CHECK(sch::get<"blobs">(read.at(row)) == sch::get<"blobs">(rows.at(row)));
      CHECK(sch::get<"docs">(read.at(row)) == sch::get<"docs">(rows.at(row)));
      CHECK(sch::get<"keys">(read.at(row)) == sch::get<"keys">(rows.at(row)));
      CHECK(sch::get<"levels">(read.at(row)) == sch::get<"levels">(rows.at(row)));
      CHECK(sch::get<"amounts">(read.at(row)) == sch::get<"amounts">(rows.at(row)));
      CHECK(sch::get<"labels">(read.at(row)) == sch::get<"labels">(rows.at(row)));
    }
  }

  database.drop_table<Lists>().get();
}

namespace {

/** @brief A `box`, kept in its text form: PostgreSQL's built-in type whose array delimiter is `;`, not `,` (as PostGIS's `geometry`'s is `:`). */
struct Box {
  std::string text;

  auto operator<=>(const Box&) const = default;
};

}  // namespace

template <>
struct schemacht::ColumnType<Box> {
  static constexpr std::string_view SQL_TYPE = "box";
  static Box                        parse(std::string_view text) { return {std::string(text)}; }
  static std::string                format(const Box& box) { return box.text; }
};

TEST_CASE("integration: a type whose array delimiter isn't a comma is inserted and listed", "[integration][insert]") {
  using Shape = sch::Field<Box, "shape">;
  using Boxes = sch::Schema<"schemacht_it_boxes", Id, Shape>;
  pg::Database database(test_db());
  database.drop_table<Boxes>().get();
  database.create_table<Boxes>().get();

  // A `box`'s `=` compares areas: 4, 1 and 9 here.
  const std::vector<Boxes::row_type> rows{
      Boxes::make_row(1, Box{"(2,2),(0,0)"}), Boxes::make_row(2, Box{"(1,1),(0,0)"}), Boxes::make_row(3, Box{"(3,3),(0,0)"})
  };
  CHECK(database.insert<Boxes>(rows).get() == 3);

  static constexpr auto ALL = q::select<Boxes>().order_by(q::On<Boxes>::col<"id">().asc());
  const auto            read = database.execute(q::Prepared<ALL>::bind(), pg::ReadOptions{.format = pg::ResultFormat::Text}).get();
  REQUIRE(read.size() == rows.size());
  for ( std::size_t row = 0; row < rows.size(); ++row ) {
    CHECK(sch::get<"shape">(read.at(row)) == sch::get<"shape">(rows.at(row)));
  }

  static constexpr auto SHAPE_IN = q::select(q::On<Boxes>::col<"shape">().in(q::arg<0>())).order_by(q::On<Boxes>::col<"id">().asc());
  const auto            listed =
      database
          .execute(
              q::Prepared<SHAPE_IN>::bind(std::vector{Box{"(2,2),(0,0)"}, Box{"(3,3),(0,0)"}}), pg::ReadOptions{.format = pg::ResultFormat::Text}
          )
          .get();
  REQUIRE(listed.size() == 2);
  CHECK(sch::get<"id">(listed.at(0)) == 1);
  CHECK(sch::get<"id">(listed.at(1)) == 3);

  database.drop_table<Boxes>().get();
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
  database.execute(Refused::bind(Unwritable{}), [&called](const std::vector<Refused::row_type>& /*rows*/, const std::exception_ptr& error) {
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
  CHECK(database.table_differences<Times>().get().empty());  // dates, times and a uuid

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
  CHECK(database.table_differences<Tasks>().get().empty());  // the column type helpers' types
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
  const std::vector<Numbered::insert_row_type> text_rows{Numbered::make_insert_row("d")};
  const auto as_text = database.execute(pg::insert_returning_statement<Numbered>(text_rows), pg::ReadOptions{.format = pg::ResultFormat::Text}).get();
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

namespace {

using Word = sch::Field<std::string, "word">;
using Words = sch::Schema<"schemacht_it_words", Id, Word>;
using WordColumns = q::On<Words>;
namespace trgm = schemacht::test::trgm;

// The words similar to an argument, the most similar first.
constexpr auto LIKE_THIS = q::select(WordColumns::col<"word">().apply<trgm::Similar>(q::arg<0>()))
                               .order_by(WordColumns::col<"word">().ranked_by<trgm::Distance>(q::arg<0>()))
                               .project(WordColumns::col<"word">());

// The words more similar to an argument than a number.
constexpr auto CLOSE_TO = q::select(WordColumns::col<"word">().apply<trgm::SimilarAbove>(q::arg<0>(), q::arg<1>()))
                              .order_by(WordColumns::col<"id">().asc())
                              .project(WordColumns::col<"word">());

}  // namespace

TEST_CASE("integration: an extension's operations: pg_trgm's", "[integration][extension]") {
  pg::Database database(test_db());
  try {
    exec(database, "CREATE EXTENSION IF NOT EXISTS pg_trgm");
  } catch ( const pg::QueryError& error ) {
    SKIP("pg_trgm is not available: " << error.what());
  }

  exec(
      database,
      "CREATE OR REPLACE FUNCTION similar_above(text, text, double precision) RETURNS boolean AS 'SELECT similarity($1, $2) > $3' LANGUAGE sql"
  );
  database.drop_table<Words>().get();
  database.create_table<Words>().get();
  database
      .insert<Words>(
          {Words::make_row(1, "word"), Words::make_row(2, "words"), Words::make_row(3, "sword"), Words::make_row(4, "world"),
           Words::make_row(5, "banana")}
      )
      .get();

  // Similar by the pg_trgm.similarity_threshold setting: "word" itself first and "words" next, never "banana".
  const auto similar = database.execute(q::Prepared<LIKE_THIS>::bind("word")).get();
  REQUIRE(similar.size() >= 2);
  CHECK(sch::get<"word">(similar[0]) == "word");
  CHECK(sch::get<"word">(similar[1]) == "words");
  CHECK(std::ranges::none_of(similar, [](const auto& row) { return sch::get<"word">(row) == "banana"; }));

  // More similar than 0.5: "word" (1) and "words" (4 trigrams of 7 shared); "sword" and "world" share 3 of 8.
  const auto close = database.execute(q::Prepared<CLOSE_TO>::bind("word", 0.5)).get();
  REQUIRE(close.size() == 2);
  CHECK(sch::get<"word">(close[0]) == "word");
  CHECK(sch::get<"word">(close[1]) == "words");

  database.drop_table<Words>().get();
  exec(database, "DROP FUNCTION IF EXISTS similar_above(text, text, double precision)");
}

namespace {

using Document = sch::Field<schemacht::json::Json, "doc">;
using Documents = sch::Schema<"schemacht_it_documents", Id, Document>;
using DocumentColumns = q::On<Documents>;

// The documents with a key: jsonb's `?`, whose argument is text, not the column's jsonb.
constexpr auto WITH_KEY = q::select(DocumentColumns::col<"doc">().apply<schemacht::test::HasKey>(q::arg<0>()))
                              .order_by(DocumentColumns::col<"id">().asc())
                              .project(DocumentColumns::col<"id">());

}  // namespace

TEST_CASE("integration: an operation whose argument has a type of its own: jsonb's ?", "[integration][extension]") {
  using schemacht::json::Json;
  using schemacht::test::JsonKey;

  pg::Database database(test_db());
  database.drop_table<Documents>().get();
  database.create_table<Documents>().get();
  database
      .insert<Documents>(
          {Documents::make_row(1, Json(R"({"tags": ["a"]})")), Documents::make_row(2, Json(R"({"name": "b"})")),
           Documents::make_row(3, Json(R"({"tags": [], "name": "c"})"))}
      )
      .get();

  const auto tagged = database.execute(q::Prepared<WITH_KEY>::bind(JsonKey{"tags"})).get();
  REQUIRE(tagged.size() == 2);
  CHECK(sch::get<"id">(tagged[0]) == 1);
  CHECK(sch::get<"id">(tagged[1]) == 3);

  CHECK(database.execute(q::Prepared<WITH_KEY>::bind(JsonKey{"missing"})).get().empty());

  database.drop_table<Documents>().get();
}

namespace {

using Named = schemacht::test::Numbered;  // an identity key, a name and a nickname
using NamedColumns = q::On<Named>;

constexpr auto BY_NAME = q::select<Named>().order_by(NamedColumns::col<"name">().asc());
constexpr auto RENAME = q::update<Named>()
                            .set(NamedColumns::col<"name">(), q::arg<0>())
                            .where(NamedColumns::col<"name">() == q::arg<1>())
                            .returning(NamedColumns::col<"name">());
constexpr auto REMOVE_NAMED = q::delete_where(NamedColumns::col<"name">() == q::arg<0>());
constexpr auto ALL_TAGGED = q::select<schemacht::test::Tagged>();

std::vector<std::string> names(const std::vector<Named::row_type>& rows) {
  std::vector<std::string> result;
  result.reserve(rows.size());
  for ( const Named::row_type& row : rows ) {
    result.push_back(sch::get<"name">(row));
  }
  return result;
}

}  // namespace

namespace {

/** @brief Makes `table` afresh: dropped if it was there, then created. */
template <sch::IsSchema schema_t>
void recreate(pg::StatementRunner& runner, const sch::Table<schema_t>& table) {
  runner.execute(pg::drop_table_statement<schema_t>().on(table)).get();
  runner.execute(pg::create_table_statement<schema_t>().on(table)).get();
}

/** @brief Drops `table`. */
template <sch::IsSchema schema_t>
void drop(pg::StatementRunner& runner, const sch::Table<schema_t>& table) {
  runner.execute(pg::drop_table_statement<schema_t>().on(table)).get();
}

}  // namespace

TEST_CASE("integration: one schema on tables named while the program runs", "[integration][table_name]") {
  pg::Database            database(test_db());
  const sch::Table<Named> first("schemacht_it_named_a");
  const sch::Table<Named> second("schemacht_it_named_b");
  recreate(database, first);
  recreate(database, second);

  database
      .execute(pg::insert_statement<Named>(std::vector{Named::make_insert_row("cat", std::nullopt), Named::make_insert_row("ann", "a")}).on(first))
      .get();
  const auto written = database.execute(pg::insert_returning_statement<Named>(std::vector{Named::make_insert_row("bob", "b")}).on(second)).get();
  REQUIRE(written.size() == 1);

  // Each table holds its own rows.
  CHECK(names(database.execute(q::Prepared<BY_NAME>::bind().on(first)).get()) == std::vector<std::string>{"ann", "cat"});
  CHECK(names(database.execute(q::Prepared<BY_NAME>::bind().on(second)).get()) == std::vector<std::string>{"bob"});
  CHECK(names(database.execute(pg::by_key<Named>(std::vector{sch::get<"id">(written[0])}).on(second)).get()) == std::vector<std::string>{"bob"});

  // Updates and deletes, with and without RETURNING, change only their own table.
  const auto renamed = database.execute(q::Prepared<RENAME>::bind("anne", "ann").on(first)).get();
  REQUIRE(renamed.size() == 1);
  CHECK(sch::get<"name">(renamed[0]) == "anne");
  CHECK(database.execute(q::Prepared<REMOVE_NAMED>::bind("cat").on(first)).get() == 1);
  CHECK(database.execute(q::Prepared<REMOVE_NAMED>::bind("cat").on(second)).get() == 0);

  std::size_t streamed = 0;
  database.stream(q::Prepared<BY_NAME>::bind().on(first), [&streamed](const Named::row_type& /*row*/) { ++streamed; }).get();
  CHECK(streamed == 1);

  // The same table on a transaction's connection: rolled back, as anything else in it would be.
  {
    pg::Transaction transaction = database.begin().get();
    transaction.execute(pg::insert_statement<Named>(std::vector{Named::make_insert_row("dan", std::nullopt)}).on(first)).get();
    CHECK(transaction.execute(q::Prepared<BY_NAME>::bind().on(first)).get().size() == 2);
    transaction.rollback().get();
  }
  CHECK(names(database.execute(q::Prepared<BY_NAME>::bind().on(first)).get()) == std::vector<std::string>{"anne"});

  drop(database, first);
  drop(database, second);
}

TEST_CASE("integration: a table named while the program runs takes an array column's rows", "[integration][table_name]") {
  using schemacht::test::Tagged;

  pg::Database             database(test_db());
  const sch::Table<Tagged> tagged("schemacht_it_named_tagged");
  recreate(database, tagged);

  const std::vector rows{Tagged::make_row(1, std::vector<std::string>{"a", "b"}), Tagged::make_row(2, std::vector<std::string>{})};
  CHECK(database.execute(pg::insert_statement<Tagged>(rows).on(tagged)).get() == 2);
  CHECK(database.execute(q::Prepared<ALL_TAGGED>::bind().on(tagged)).get().size() == 2);

  drop(database, tagged);
}

TEST_CASE("integration: a table named while the program runs, in a schema of its own", "[integration][table_name]") {
  pg::Database database(test_db());
  exec(database, "DROP SCHEMA IF EXISTS schemacht_it_schema CASCADE");
  exec(database, "CREATE SCHEMA schemacht_it_schema");

  const sch::Table<Named> table("schemacht_it_schema", "numbered");
  database.execute(pg::create_table_statement<Named>().on(table)).get();
  database.execute(pg::insert_statement<Named>(std::vector{Named::make_insert_row("eve", std::nullopt)}).on(table)).get();
  CHECK(names(database.execute(q::Prepared<BY_NAME>::bind().on(table)).get()) == std::vector<std::string>{"eve"});

  exec(database, "DROP SCHEMA schemacht_it_schema CASCADE");
}

namespace {

using schemacht::test::Keyless;
using schemacht::test::Pairs;

constexpr auto ALL_PAIRS = q::select<Pairs>().order_by(q::On<Pairs>::col<"a">().asc(), q::On<Pairs>::col<"b">().asc());
constexpr auto ALL_KEYLESS = q::select<Keyless>().order_by(q::On<Keyless>::col<"name">().asc());

/**
 * @brief Waits for a call's completion: `done()` is the completion, `wait()` rethrows its error, and `thread()` is the thread it
 * ran on, once it has.
 */
class Completion {
 public:
  /** @return The completion: it takes whatever the call gives it (a count, say), of which the last is the error. */
  [[nodiscard]] auto done() {
    return [this](const auto&... results) {
      _thread = std::this_thread::get_id();
      schemacht::util::settle(_promise, std::get<sizeof...(results) - 1>(std::tie(results...)));
    };
  }

  void wait() { _promise.get_future().get(); }

  [[nodiscard]] std::thread::id thread() const { return _thread; }

 private:
  std::promise<void> _promise;
  std::thread::id    _thread;
};

/** @brief A value whose stored form is always refused: an insert of it fails before anything is sent. */
struct Unstorable {
  auto operator<=>(const Unstorable&) const = default;
};

}  // namespace

template <>
struct schemacht::ColumnType<Unstorable>
    : schemacht::ConvertedColumn<
          Unstorable, std::string, [](const Unstorable& /*value*/) -> std::string { throw std::invalid_argument("this value has no stored form"); },
          [](const std::string& /*text*/) { return Unstorable{}; }> {};

namespace {

/** @brief A completion that can be called only as an lvalue. */
struct LvalueOnly {
  Completion* completion;

  // NOLINTNEXTLINE(readability-make-member-function-const) -- `&` alone, so that it can't be called as an rvalue
  void operator()(std::uint64_t inserted, const std::exception_ptr& error) & { completion->done()(inserted, error); }
};

using Refusals = sch::Schema<"refusals", schemacht::test::Name, sch::Field<Unstorable, "value">>;
using Listed = sch::Schema<"schemacht_it_listed", schemacht::test::Name, schemacht::test::Score>;

constexpr auto RESCORE = q::update<Keyless>().set(q::On<Keyless>::col<"score">(), q::arg<0>()).all_rows();

}  // namespace

TEST_CASE("integration: tables named while the program runs, with a two-column key or none", "[integration][table_name]") {
  pg::Database            database(test_db());
  const sch::Table<Pairs> pairs("schemacht_it_named_pairs");
  recreate(database, pairs);
  database.execute(pg::insert_statement<Pairs>(std::vector{Pairs::make_row(1, 1, 10), Pairs::make_row(1, 2, 20)}).on(pairs)).get();

  Completion inserted;
  database.execute(pg::insert_statement<Pairs>(std::vector{Pairs::make_row(2, 1, 30)}).on(pairs), inserted.done());
  inserted.wait();
  CHECK(database.execute(q::Prepared<ALL_PAIRS>::bind().on(pairs)).get().size() == 3);

  // The key is both columns: the same pair again is refused, through the completion.
  Completion refused;
  database.execute(pg::insert_statement<Pairs>(std::vector{Pairs::make_row(1, 2, 99)}).on(pairs), refused.done());
  try {
    refused.wait();
    FAIL("the insert should have been refused");
  } catch ( const pg::QueryError& error ) {
    CHECK(error.sqlstate() == "23505");  // unique_violation
  }

  const sch::Table<Keyless> keyless("schemacht_it_named_keyless");
  recreate(database, keyless);
  const std::vector rows{Keyless::make_row("b", 2.0), Keyless::make_row("a", 1.0), Keyless::make_row("a", 1.0)};  // repeats allowed
  database.execute(pg::insert_statement<Keyless>(rows).on(keyless)).get();
  CHECK(database.execute(q::Prepared<ALL_KEYLESS>::bind().on(keyless)).get().size() == 3);

  drop(database, pairs);
  drop(database, keyless);
}

TEST_CASE("integration: an insert chained onto a query's completion, without a future in between", "[integration][table_name]") {
  pg::Database            database(test_db());
  const sch::Table<Named> source("schemacht_it_chain_source");
  const sch::Table<Named> copy("schemacht_it_chain_copy");
  recreate(database, source);
  recreate(database, copy);
  database
      .execute(pg::insert_statement<Named>(std::vector{Named::make_insert_row("ann", std::nullopt), Named::make_insert_row("bob", "b")}).on(source))
      .get();

  // Read on a worker, then insert from that completion, then finish: nothing blocks a worker on a future.
  Completion copied;
  database.execute(
      q::Prepared<BY_NAME>::bind().on(source),
      [&database, &copy, &copied](const std::vector<Named::row_type>& rows, const std::exception_ptr& error) {
        if ( error ) {
          copied.done()(error);
          return;
        }

        try {  // whole rows: their generated ids are not sent, so `copy` numbers its own
          database.execute(pg::insert_statement<Named>(rows).on(copy), copied.done());
        } catch ( ... ) {  // no memory to start it: say so, or the wait below would never end
          copied.done()(std::current_exception());
        }
      }
  );
  copied.wait();
  CHECK(names(database.execute(q::Prepared<BY_NAME>::bind().on(copy)).get()) == std::vector<std::string>{"ann", "bob"});

  // Nothing to insert still completes, on a worker.
  Completion nothing;
  database.execute(pg::insert_statement<Named>(std::vector<Named::row_type>{}).on(copy), nothing.done());
  nothing.wait();

  // In a transaction, the rows go when it rolls back.
  {
    pg::Transaction transaction = database.begin().get();
    Completion      added;
    transaction.execute(pg::insert_statement<Named>(std::vector{Named::make_insert_row("cat", std::nullopt)}).on(copy), added.done());
    added.wait();
    transaction.rollback().get();
  }
  CHECK(database.execute(q::Prepared<BY_NAME>::bind().on(copy)).get().size() == 2);

  // A stream with a completion.
  std::size_t streamed = 0;
  Completion  streamed_all;
  database.stream(q::Prepared<BY_NAME>::bind().on(source), [&streamed](const Named::row_type& /*row*/) { ++streamed; }, streamed_all.done());
  streamed_all.wait();
  CHECK(streamed == 2);

  // A transaction begun, and committed, from completions.
  Completion committed;
  database.begin({}, [&committed](pg::Transaction transaction, const std::exception_ptr& error) {
    if ( error ) {
      committed.done()(error);
      return;
    }

    const auto held = std::make_shared<pg::Transaction>(std::move(transaction));
    held->commit([held, &committed](const std::exception_ptr& commit_error) { committed.done()(commit_error); });
  });
  committed.wait();

  drop(database, source);
  drop(database, copy);
}

TEST_CASE("integration: a call with a completion calls it once, on a worker, whatever fails", "[integration]") {
  pg::Database              database(test_db());
  const sch::Table<Keyless> keyless("schemacht_it_completions");
  recreate(database, keyless);
  const std::vector<Keyless::row_type> rows{Keyless::make_row("a", 1.0), Keyless::make_row("b", 2.0)};

  // On the database, into the schema's own table.
  database.drop_table<Listed>().get();
  database.create_table<Listed>().get();
  const std::vector<Listed::row_type> listed{Listed::make_row("a", 1.0), Listed::make_row("b", 2.0)};

  Completion from_vector;
  database.insert<Listed>(listed, from_vector.done());
  from_vector.wait();
  CHECK(from_vector.thread() != std::this_thread::get_id());

  Completion from_span;  // any contiguous range, through the statement
  database.execute(pg::insert_statement<Listed>(std::span<const Listed::row_type>(listed).first(1)), from_span.done());
  from_span.wait();
  CHECK(database.execute(q::Prepared<q::select<Listed>()>::bind()).get().size() == 3);
  database.drop_table<Listed>().get();

  database.execute(pg::insert_statement<Keyless>(rows).on(keyless)).get();

  // No rows: an insert of nothing, sent all the same.
  CHECK(database.execute(pg::insert_statement<Keyless>(std::vector<Keyless::row_type>{}).on(keyless)).get() == 0);

  // An update with a completion, on the table: the count of rows it changed.
  std::promise<std::uint64_t> rescored;
  database.execute(q::Prepared<RESCORE>::bind(5.0).on(keyless), [&rescored](std::uint64_t count, const std::exception_ptr& error) {
    schemacht::util::settle(rescored, error, count);
  });
  CHECK(rescored.get_future().get() == 2);

  // A value its codec refuses: no throw from the call, the error through the completion, on a worker, and nothing written.
  const sch::Table<Refusals> refusals("schemacht_it_refusals");
  recreate(database, refusals);
  const std::vector refused_rows{Refusals::make_row("a", Unstorable{})};

  Completion refused;
  database.execute(pg::insert_statement<Refusals>(refused_rows).on(refusals), refused.done());
  CHECK_THROWS_WITH(refused.wait(), "this value has no stored form");
  CHECK(refused.thread() != std::this_thread::get_id());

  CHECK_THROWS_WITH(database.execute(pg::insert_statement<Refusals>(refused_rows).on(refusals)).get(), "this value has no stored form");

  // An argument its codec refuses, in a stream and in a command: the same.
  using RefusedQuery = q::RawStatement<"SELECT $1::text AS n", q::RawArguments<Unwritable>, sch::Field<std::string, "n">>;
  Completion streamed;
  database.stream(RefusedQuery::bind(Unwritable{}), [](const RefusedQuery::row_type& /*row*/) {}, streamed.done());
  CHECK_THROWS_WITH(streamed.wait(), "this argument has no text form");
  CHECK(streamed.thread() != std::this_thread::get_id());

  using RefusedCommand = q::RawStatement<"UPDATE schemacht_it_completions SET score = 0 WHERE $1::text = ''", q::RawArguments<Unwritable>>;
  std::promise<std::uint64_t> commanded;
  database.execute(RefusedCommand::bind(Unwritable{}), [&commanded](std::uint64_t count, const std::exception_ptr& error) {
    schemacht::util::settle(commanded, error, count);
  });
  CHECK_THROWS_WITH(commanded.get_future().get(), "this argument has no text form");

  // A completion callable only as an lvalue.
  Completion lvalue_only;
  database.execute(pg::insert_statement<Keyless>(rows).on(keyless), LvalueOnly{&lvalue_only});
  lvalue_only.wait();

  drop(database, keyless);
  drop(database, refusals);
}

namespace {

using Body = sch::Field<std::string, "body">;
using Hits = sch::Field<std::int64_t, "hits">;
using Notes = sch::Schema<"schemacht_it_notes", Id, Body, Hits>;

/** @brief Merges by a SQL function of the test's own, which `unchecked_sql` creates: the longer text wins. */
struct KeepLonger {
  static constexpr std::string_view FUNCTION = "schemacht_it_keep_longer";
};

/** @brief Merges by a SQL operator: the counts add up. */
struct Add {
  static constexpr std::string_view OPERATOR = "+";
};

constexpr auto ALL_NOTES = q::select<Notes>().order_by(q::On<Notes>::col<"id">().asc());

/** @return Each note as (id, body, hits), in key order. */
std::vector<std::tuple<std::int64_t, std::string, std::int64_t>> notes_in(pg::StatementRunner& runner, const sch::Table<Notes>& table) {
  std::vector<std::tuple<std::int64_t, std::string, std::int64_t>> notes;
  for ( const auto& row : runner.execute(q::Prepared<ALL_NOTES>::bind().on(table)).get() ) {
    notes.emplace_back(sch::get<"id">(row), sch::get<"body">(row), sch::get<"hits">(row));
  }
  return notes;
}

}  // namespace

TEST_CASE("integration: an upsert inserts new keys and merges into the rows already there", "[integration][upsert]") {
  using Note = std::tuple<std::int64_t, std::string, std::int64_t>;
  pg::Database            database(test_db());
  const sch::Table<Notes> notes("schemacht_it_notes_merged");
  recreate(database, notes);
  database
      .execute(
          pg::unchecked_sql(
              "CREATE OR REPLACE FUNCTION schemacht_it_keep_longer(current_body text, incoming text) RETURNS text LANGUAGE sql IMMUTABLE "
              "AS $$ SELECT CASE WHEN length(incoming) > length(current_body) THEN incoming ELSE current_body END $$"
          )
      )
      .get();

  CHECK(database.execute(pg::insert_statement<Notes>(std::vector{Notes::make_row(1, "short", 1), Notes::make_row(2, "bb", 5)}).on(notes)).get() == 2);

  // Key 1 is there (merged: the longer body, the hits added), key 3 is not (inserted); key 2 is left alone.
  const std::vector incoming{Notes::make_row(1, "much longer", 2), Notes::make_row(3, "new", 1)};
  CHECK(database.execute(pg::upsert_statement<Notes, pg::MergeWith<"body", KeepLonger>, pg::MergeWith<"hits", Add>>(incoming).on(notes)).get() == 2);
  CHECK(notes_in(database, notes) == std::vector<Note>{{1, "much longer", 3}, {2, "bb", 5}, {3, "new", 1}});

  // Without a MergeWith, the row already there takes the incoming values: a shorter body too.
  CHECK(database.execute(pg::upsert_statement<Notes>(std::vector{Notes::make_row(1, "x", 10)}).on(notes)).get() == 1);
  CHECK(notes_in(database, notes).front() == Note{1, "x", 10});

  // Two rows with one key in one statement: the database refuses the whole of it.
  CHECK_THROWS_AS(
      database.execute(pg::upsert_statement<Notes>(std::vector{Notes::make_row(4, "a", 1), Notes::make_row(4, "b", 1)}).on(notes)).get(),
      pg::QueryError
  );
  CHECK(notes_in(database, notes).size() == 3);

  // In a transaction rolled back, the merge is undone.
  pg::Transaction transaction = database.begin().get();
  CHECK(
      transaction.execute(pg::upsert_statement<Notes, pg::MergeWith<"hits", Add>>(std::vector{Notes::make_row(2, "bb", 100)}).on(notes)).get() == 1
  );
  CHECK(std::get<2>(notes_in(transaction, notes).at(1)) == 105);
  transaction.rollback().get();
  CHECK(std::get<2>(notes_in(database, notes).at(1)) == 5);

  drop(database, notes);
  database.execute(pg::unchecked_sql("DROP FUNCTION schemacht_it_keep_longer(text, text)")).get();
}

TEST_CASE("integration: the upsert convenience runs on the schema's own table", "[integration][upsert]") {
  pg::Database database(test_db());
  database.drop_table<Notes>().get();
  database.create_table<Notes>().get();

  CHECK(database.upsert<Notes>({Notes::make_row(1, "a", 1)}).get() == 1);
  CHECK(database.upsert<Notes, pg::MergeWith<"hits", Add>>({Notes::make_row(1, "b", 2), Notes::make_row(2, "c", 3)}).get() == 2);
  const auto rows = database.execute(q::Prepared<ALL_NOTES>::bind()).get();
  REQUIRE(rows.size() == 2);
  CHECK(sch::get<"body">(rows.at(0)) == "b");
  CHECK(sch::get<"hits">(rows.at(0)) == 3);

  // A kept column stays as it is in a row already there, and is written for a row inserted.
  CHECK(database.upsert<Notes, pg::Keep<"body">>({Notes::make_row(1, "ignored", 9), Notes::make_row(3, "d", 4)}).get() == 2);
  const auto kept = database.execute(q::Prepared<ALL_NOTES>::bind()).get();
  REQUIRE(kept.size() == 3);
  CHECK(sch::get<"body">(kept.at(0)) == "b");
  CHECK(sch::get<"hits">(kept.at(0)) == 9);
  CHECK(sch::get<"body">(kept.at(2)) == "d");

  database.drop_table<Notes>().get();
}

TEST_CASE("integration: a statement given as text runs with its arguments, on the pool or in a transaction", "[integration][unchecked_sql]") {
  pg::Database            database(test_db());
  const sch::Table<Notes> notes("schemacht_it_notes_text");
  recreate(database, notes);
  database
      .execute(pg::insert_statement<Notes>(std::vector{Notes::make_row(1, "a", 1), Notes::make_row(2, "b", 2), Notes::make_row(3, "c", 3)}).on(notes))
      .get();

  const std::string table = notes.name().quoted();
  CHECK(database.execute(pg::unchecked_sql("CREATE INDEX ON " + table + " (body)")).get() == 0);
  CHECK(
      database.execute(pg::unchecked_sql("UPDATE " + table + " SET hits = $1 WHERE id = ANY($2)", std::int64_t{7}, std::vector<std::int64_t>{1, 3}))
          .get() == 2
  );
  CHECK(std::get<2>(notes_in(database, notes).at(2)) == 7);

  pg::Transaction transaction = database.begin().get();
  CHECK(transaction.execute(pg::unchecked_sql("DELETE FROM " + table + " WHERE body = $1", std::string("b"))).get() == 1);
  CHECK(notes_in(transaction, notes).size() == 2);
  transaction.rollback().get();
  CHECK(notes_in(database, notes).size() == 3);

  // Its errors arrive as any statement's: the server's, and an argument its codec refuses, never from the call.
  CHECK_THROWS_AS(database.execute(pg::unchecked_sql("NOT SQL")).get(), pg::QueryError);
  CHECK_THROWS_AS(database.execute(pg::unchecked_sql("SELECT 1; SELECT 2")).get(), pg::QueryError);  // one statement only
  std::future<std::uint64_t> refused;
  CHECK_NOTHROW(refused = database.execute(pg::unchecked_sql("SELECT $1::text", Unwritable{})));
  CHECK_THROWS_WITH(refused.get(), "this argument has no text form");

  drop(database, notes);
}

namespace {

using Region = sch::Field<std::int32_t, "region", sch::KeyRole::Primary>;
using TallyDay = sch::Field<std::int32_t, "day", sch::KeyRole::Primary>;
using Total = sch::Field<Cents, "total">;  // a type of the test's own: sent as text, cast back
using Seen = sch::Field<std::vector<std::string>, "seen">;
using Tallies = sch::Schema<"schemacht_it_tallies", Region, TallyDay, Total, Seen>;

/** @brief Merges totals by a function named with its schema, called there whatever the search path is. */
struct AddTotals {
  static constexpr std::string_view FUNCTION = "schemacht_it_upsert.add_totals";
};

/** @brief Merges arrays by appending: `"current"."seen" || excluded."seen"`. */
struct Append {
  static constexpr std::string_view OPERATOR = "||";
};

constexpr auto ALL_TALLIES = q::select<Tallies>().order_by(q::On<Tallies>::col<"region">().asc()).order_by(q::On<Tallies>::col<"day">().asc());

}  // namespace

TEST_CASE("integration: an upsert merges a type of one's own and an array, on a two-column key, in a schema of its own", "[integration][upsert]") {
  pg::Database database(test_db());
  database.execute(pg::unchecked_sql("DROP SCHEMA IF EXISTS schemacht_it_upsert CASCADE")).get();
  database.execute(pg::unchecked_sql("CREATE SCHEMA schemacht_it_upsert")).get();
  const sch::Table<Tallies> tallies("schemacht_it_upsert", "tallies");
  database.execute(pg::create_table_statement<Tallies>().on(tallies)).get();
  database
      .execute(
          pg::unchecked_sql(
              "CREATE FUNCTION schemacht_it_upsert.add_totals(current_total numeric, incoming numeric) RETURNS numeric LANGUAGE sql IMMUTABLE "
              "AS $$ SELECT current_total + incoming $$"
          )
      )
      .get();

  database.execute(pg::insert_statement<Tallies>(std::vector{Tallies::make_row(1, 1, Cents{100}, std::vector<std::string>{"a"})}).on(tallies)).get();

  // (1, 1) is there: its total adds up (by the function in the table's schema, not on the search path) and its array grows. (1, 2) shares only half the key, so it is a row of its own.
  const std::vector incoming{
      Tallies::make_row(1, 1, Cents{250}, std::vector<std::string>{"b", "c,d"}), Tallies::make_row(1, 2, Cents{5}, std::vector<std::string>{})
  };
  CHECK(
      database.execute(pg::upsert_statement<Tallies, pg::MergeWith<"total", AddTotals>, pg::MergeWith<"seen", Append>>(incoming).on(tallies)).get() ==
      2
  );

  const auto rows = database.execute(q::Prepared<ALL_TALLIES>::bind().on(tallies)).get();
  REQUIRE(rows.size() == 2);
  CHECK(sch::get<"total">(rows.at(0)) == Cents{350});
  CHECK(sch::get<"seen">(rows.at(0)) == std::vector<std::string>{"a", "b", "c,d"});
  CHECK(sch::get<"total">(rows.at(1)) == Cents{5});
  CHECK(sch::get<"seen">(rows.at(1)).empty());

  database.execute(pg::unchecked_sql("DROP SCHEMA schemacht_it_upsert CASCADE")).get();
}

namespace {

/** @brief A code of at most three characters, as a type of the test's own: `varchar(3)`. */
struct ShortCode {
  std::string text;
};

}  // namespace

template <>
struct schemacht::ColumnType<ShortCode> {
  static constexpr std::string_view SQL_TYPE = "varchar(3)";
  static ShortCode                  parse(std::string_view text) { return ShortCode{std::string(text)}; }
  static std::string                format(const ShortCode& code) { return code.text; }
};

TEST_CASE("integration: a value too long for its column's size is refused, not cut to fit", "[integration][insert]") {
  using Codes = sch::Schema<"schemacht_it_codes", Id, sch::Field<ShortCode, "code">>;
  static constexpr auto CODE_IN = q::select(q::On<Codes>::col<"code">().in(q::arg<0>()));

  pg::Database database(test_db());
  database.drop_table<Codes>().get();
  database.create_table<Codes>().get();

  CHECK(database.insert<Codes>({Codes::make_row(1, ShortCode{"abc"})}).get() == 1);
  CHECK_THROWS_AS(database.insert<Codes>({Codes::make_row(2, ShortCode{"abcdef"})}).get(), pg::QueryError);  // not stored as "abc"

  CHECK(database.execute(q::Prepared<CODE_IN>::bind(std::vector{ShortCode{"abc"}})).get().size() == 1);
  CHECK(database.execute(q::Prepared<CODE_IN>::bind(std::vector{ShortCode{"abcdef"}})).get().empty());  // not compared as "abc"

  database.drop_table<Codes>().get();
}

TEST_CASE("integration: a stream hands its rows over a chunk at a time, in order", "[integration][stream]") {
  pg::Database database(test_db(), pg::Database::Options{.pool = {}, .read = {.chunk_rows = 100}});
  CHECK(database.read_options().chunk_rows == 100);

  // 250 rows as the database reads by default: chunks of 100, and the last has what is left.
  std::vector<std::size_t> sizes;
  std::int64_t             next = 1;
  bool                     in_order = true;
  database
      .stream_chunks(
          Series::bind(250),
          [&](const std::vector<Series::row_type>& rows) {
            sizes.push_back(rows.size());
            for ( const Series::row_type& row : rows ) {
              in_order = in_order && sch::get<"n">(row) == next++;
            }
          }
      )
      .get();
  CHECK(sizes == std::vector<std::size_t>{100, 100, 50});
  CHECK(in_order);

  // One setting changed for the call, the rest as the database's: chunks of 40, with a completion.
  pg::ReadOptions read = database.read_options();
  read.chunk_rows = 40;
  std::vector<std::vector<Series::row_type>> kept;  // each chunk is the handler's to keep
  std::promise<void>                         ended;
  database.stream_chunks(
      Series::bind(100), [&kept](std::vector<Series::row_type>&& rows) { kept.push_back(std::move(rows)); }, read,
      [&ended](const std::exception_ptr& error) { schemacht::util::settle(ended, error); }
  );
  ended.get_future().get();
  REQUIRE(kept.size() == 3);
  CHECK(kept.at(0).size() == 40);
  CHECK(kept.at(1).size() == 40);
  CHECK(kept.at(2).size() == 20);
  CHECK(sch::get<"n">(kept.at(2).back()) == 100);

  // A statement that returns no rows never calls the handler.
  std::size_t calls = 0;
  database.stream_chunks(Series::bind(0), [&calls](const std::vector<Series::row_type>& /*rows*/) { ++calls; }).get();
  CHECK(calls == 0);
}

TEST_CASE("integration: a chunk handler stops its stream, or fails it; what it had stays", "[integration][stream]") {
  // One connection: the query after each stream only runs if the stream's connection came back.
  pg::Database database(test_db(), pg::Database::Options{.pool = {.connections = 1, .workers = 2, .session = {}}, .read = {.chunk_rows = 100}});

  std::size_t seen = 0;
  database.stream_chunks(Series::bind(100000000), [&seen](const std::vector<Series::row_type>& rows) { return (seen += rows.size()) < 250; }).get();
  CHECK(seen == 300);  // stopped by the third chunk
  CHECK(database.execute(Series::bind(3)).get().size() == 3);

  seen = 0;
  std::future<void> failed = database.stream_chunks(Series::bind(100000000), [&seen](const std::vector<Series::row_type>& rows) {
    seen += rows.size();
    if ( seen >= 200 ) {
      throw std::runtime_error("no more");
    }
  });
  CHECK_THROWS_WITH(failed.get(), "no more");
  CHECK(seen == 200);
  CHECK(database.execute(Series::bind(3)).get().size() == 3);

  // In a transaction, a stream stopped early leaves the transaction usable.
  pg::Transaction transaction = database.begin().get();
  seen = 0;
  transaction.stream_chunks(Series::bind(100000), [&seen](const std::vector<Series::row_type>& rows) { return (seen += rows.size()) < 250; }).get();
  CHECK(seen == 300);
  CHECK(transaction.read_options().chunk_rows == 100);  // as its database reads
  CHECK(transaction.execute(Series::bind(3)).get().size() == 3);
  transaction.commit().get();
}

TEST_CASE("integration: a chunk stream's last chunk, and a chunk with a row that can't be read", "[integration][stream]") {
  pg::Database database(test_db(), pg::Database::Options{.pool = {}, .read = {.chunk_rows = 100}});

  // A result that is a whole number of chunks ends without an empty one.
  std::vector<std::size_t> sizes;
  database.stream_chunks(Series::bind(300), [&sizes](const std::vector<Series::row_type>& rows) { sizes.push_back(rows.size()); }).get();
  CHECK(sizes == std::vector<std::size_t>{100, 100, 100});

  // Row 150 is NULL, which the field can't hold: the first chunk is handed over, and none of the second, where `stream` hands
  // over the 149 rows before it.
  using Holed = q::RawStatement<
      "SELECT CASE WHEN n = 150 THEN NULL ELSE n END AS n FROM generate_series(1::bigint, 300) AS s(n)", q::RawArguments<>,
      sch::Field<std::int64_t, "n">>;

  std::size_t in_chunks = 0;
  CHECK_THROWS(database.stream_chunks(Holed::bind(), [&in_chunks](const std::vector<Holed::row_type>& rows) { in_chunks += rows.size(); }).get());
  CHECK(in_chunks == 100);

  std::size_t in_rows = 0;
  CHECK_THROWS(database.stream(Holed::bind(), [&in_rows](const Holed::row_type& /*row*/) { ++in_rows; }).get());
  CHECK(in_rows == 149);
}

TEST_CASE("integration: a statement that is streamed is left whole, unless it was given as a temporary", "[integration][stream]") {
  pg::Database              database(test_db());
  const sch::Table<Keyless> keyless("schemacht_it_stream_twice");
  recreate(database, keyless);

  // An insert that gives back its rows, kept and streamed twice: its arguments are still there the second time.
  const std::vector<Keyless::row_type> rows{Keyless::make_row("a", 1.0), Keyless::make_row("b", 2.0)};
  auto                                 statement = pg::insert_returning_statement<Keyless>(rows).on(keyless);

  std::size_t by_row = 0;
  database.stream(statement, [&by_row](const Keyless::row_type& /*row*/) { ++by_row; }).get();
  CHECK(by_row == 2);

  std::size_t by_chunk = 0;
  database.stream_chunks(statement, [&by_chunk](const std::vector<Keyless::row_type>& chunk) { by_chunk += chunk.size(); }).get();
  CHECK(by_chunk == 2);
  CHECK(statement.params().size() == 2);

  drop(database, keyless);
}
