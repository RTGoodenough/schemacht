#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "schemacht/postgres/async_client.hpp"
#include "schemacht/postgres/field_codec.hpp"
#include "schemacht/postgres/row_reader.hpp"
#include "schemacht/schema/field.hpp"
#include "support/schemas.hpp"

namespace pg = schemacht::postgres;
namespace sch = schemacht::schema;
namespace t = schemacht::test;

namespace {

/** @brief A chunk held in memory: each cell its text or binary bytes, or NULL. */
class FakeChunk {
 public:
  FakeChunk(bool binary, std::vector<std::uint32_t> types, std::vector<std::vector<std::optional<std::string>>> cells)
      : _binary(binary), _types(std::move(types)), _cells(std::move(cells)) {}

  [[nodiscard]] std::size_t   row_count() const { return _cells.size(); }
  [[nodiscard]] std::size_t   column_count() const { return _types.size(); }
  [[nodiscard]] bool          is_binary() const { return _binary; }
  [[nodiscard]] std::uint32_t column_type(std::size_t column) const { return _types.at(column); }

  [[nodiscard]] std::optional<std::string_view> field(std::size_t row, std::size_t column) const {
    const std::optional<std::string>& cell = _cells.at(row).at(column);
    return cell ? std::optional<std::string_view>(*cell) : std::nullopt;
  }

 private:
  bool                                                 _binary;
  std::vector<std::uint32_t>                           _types;
  std::vector<std::vector<std::optional<std::string>>> _cells;
};

/** @return `value` in PostgreSQL's binary form for an `int8`: 8 big-endian bytes. */
std::string int8_bytes(std::int64_t value) {
  std::string bytes(8, '\0');
  auto        bits = std::bit_cast<std::uint64_t>(value);
  for ( std::size_t i = 8; i-- > 0; ) {
    bytes.at(i) = static_cast<char>(bits & 0xffU);
    bits >>= 8U;
  }
  return bytes;
}

using Row = std::tuple<t::Id, t::Nick>;

/** @brief A row handler that keeps rows, and wants no more once it has `limit`. */
struct Keep {
  std::vector<Row> rows;
  std::size_t      limit = 100;

  bool operator()(Row&& row) {
    rows.push_back(std::move(row));
    return rows.size() < limit;
  }
};

/** @brief A column type with a text form only. */
struct TextOnly {
  std::string text;
};

}  // namespace

template <>
class schemacht::postgres::FieldCodec<TextOnly> {
 public:
  static TextOnly    parse(std::string_view text) { return TextOnly{std::string(text)}; }
  static std::string format(const TextOnly& value) { return value.text; }
};

TEST_CASE("the reader's concepts", "[row_reader]") {
  STATIC_REQUIRE(pg::ResultChunk<pg::QueryResult>);
  STATIC_REQUIRE(pg::ResultChunk<FakeChunk>);
  STATIC_REQUIRE(sch::FieldTuple<Row>);
  STATIC_REQUIRE_FALSE(sch::FieldTuple<std::tuple<int, t::Id>>);
  STATIC_REQUIRE_FALSE(sch::FieldTuple<t::Id>);
  STATIC_REQUIRE(pg::BinaryReadableRow<Row>);
  STATIC_REQUIRE_FALSE(pg::BinaryReadableRow<std::tuple<sch::Field<TextOnly, "only">>>);
  STATIC_REQUIRE(pg::RowCallback<Keep, Row>);
  STATIC_REQUIRE(pg::RowCallback<void (*)(Row&&), Row>);
  STATIC_REQUIRE_FALSE(pg::RowCallback<int (*)(Row&&), Row>);
}

TEST_CASE("rows are read from text, with NULLs", "[row_reader]") {
  const FakeChunk chunk(false, {20, 25}, {{"1", "ann"}, {"2", std::nullopt}});
  const Row       first = pg::read_row<Row, pg::ResultFormat::Text>(chunk, 0);
  CHECK(std::get<t::Id>(first).value == 1);
  CHECK(std::get<t::Nick>(first).value == "ann");
  CHECK_FALSE(std::get<t::Nick>(pg::read_row<Row, pg::ResultFormat::Text>(chunk, 1)).value.has_value());
}

TEST_CASE("rows are read from PostgreSQL's binary form", "[row_reader]") {
  const FakeChunk chunk(true, {20, 25}, {{int8_bytes(-7), "bo"}});
  const Row       row = pg::read_row<Row, pg::ResultFormat::Binary>(chunk, 0);
  CHECK(std::get<t::Id>(row).value == -7);
  CHECK(std::get<t::Nick>(row).value == "bo");
}

TEST_CASE("a projected row reads its fields in the statement's order", "[row_reader]") {
  using Projected = std::tuple<t::Nick, t::Id>;  // what `.project(col<"nick">(), col<"id">())` returns
  const FakeChunk chunk(false, {25, 20}, {{"cy", "3"}});
  const auto      row = pg::read_row<Projected, pg::ResultFormat::Text>(chunk, 0);
  CHECK(std::get<t::Nick>(row).value == "cy");
  CHECK(std::get<t::Id>(row).value == 3);
}

TEST_CASE("errors name the column", "[row_reader]") {
  const FakeChunk null_id(false, {20, 25}, {{std::nullopt, "x"}});
  CHECK_THROWS_WITH((pg::read_row<Row, pg::ResultFormat::Text>(null_id, 0)), "column 'id' is NULL, but its schema field is not a std::optional");

  const FakeChunk bad_id(false, {20, 25}, {{"x", "y"}});
  CHECK_THROWS_WITH((pg::read_row<Row, pg::ResultFormat::Text>(bad_id, 0)),
                    "column 'id': parse_field: could not parse 'x' as a number of the column's type");

  const FakeChunk wrong_type(true, {25, 25}, {{"text", "y"}});
  Keep            keep;
  CHECK_THROWS_WITH(pg::read_rows<Row>(wrong_type, keep),
                    "column 'id': the database has the type with OID 25, which the schema's field cannot read (the field and the table disagree)");
  CHECK(keep.rows.empty());  // refused before any row is read
}

TEST_CASE("read_rows stops when the consumer wants no more", "[row_reader]") {
  const FakeChunk chunk(false, {20, 25}, {{"1", "a"}, {"2", "b"}, {"3", "c"}});
  Keep            keep{.rows = {}, .limit = 2};
  CHECK_FALSE(pg::read_rows<Row>(chunk, keep));
  CHECK(keep.rows.size() == 2);

  Keep all;
  CHECK(pg::read_rows<Row>(chunk, all));
  CHECK(all.rows.size() == 3);

  std::size_t seen = 0;
  auto        count = [&seen](const Row& /*row*/) { ++seen; };  // returns nothing: wants every row
  CHECK(pg::read_rows<Row>(chunk, count));
  CHECK(seen == 3);
}

TEST_CASE("a row with no binary form refuses a binary chunk", "[row_reader]") {
  using TextRow = std::tuple<sch::Field<TextOnly, "only">>;
  struct KeepText {
    std::vector<TextRow> rows;
    bool                 operator()(TextRow&& row) {
      rows.push_back(std::move(row));
      return true;
    }
  };
  KeepText keep;
  CHECK(pg::read_rows<TextRow>(FakeChunk(false, {25}, {{"plain"}}), keep));
  CHECK(std::get<0>(keep.rows.at(0)).value.text == "plain");
  CHECK_THROWS_AS(pg::read_rows<TextRow>(FakeChunk(true, {25}, {{"plain"}}), keep), std::logic_error);
}

TEST_CASE("an empty or moved-from QueryResult has no fields", "[row_reader]") {
  pg::QueryResult empty;
  CHECK(empty.row_count() == 0);
  CHECK_FALSE(empty.is_binary());
  CHECK_THROWS_AS(empty.field(0, 0), std::out_of_range);
  const pg::QueryResult moved(std::move(empty));
  CHECK(moved.column_count() == 0);
}
