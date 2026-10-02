#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "schemacht/column_type.hpp"
#include "schemacht/postgres/statements.hpp"
#include "schemacht/schema/schema.hpp"

namespace pg = schemacht::postgres;
namespace sch = schemacht::schema;
namespace sql = schemacht::query::sql;

namespace {

/** @brief A strongly typed id: a bigint in the database. */
struct OrderId {
  std::int64_t value = 0;

  auto operator<=>(const OrderId&) const = default;
};

enum class Color : std::uint8_t {
  Red,
  Green,
};

/** @brief A type with a text form only, written by hand. */
struct Cents {
  std::int64_t value = 0;
};

/** @brief A type that has both a `ColumnType` and an `SqlType` specialization: the `SqlType` wins. */
struct Labelled {
  std::string text;
};

/** @brief A type whose `ColumnType` misses `format`: not a column type. */
struct Half {};

std::vector<std::byte> bytes_of(std::string_view text) {
  std::vector<std::byte> bytes;
  for ( const char chr : text ) {
    bytes.push_back(static_cast<std::byte>(chr));
  }
  return bytes;
}

}  // namespace

template <>
struct schemacht::ColumnType<OrderId> : schemacht::WrappedColumn<&OrderId::value> {};

template <>
struct schemacht::ColumnType<Color> : schemacht::EnumColumn<Color, "color", "red", "green"> {};

template <>
struct schemacht::ColumnType<Cents> {
  static constexpr std::string_view SQL_TYPE = "numeric(12,2)";
  static Cents                      parse(std::string_view text) { return Cents{pg::FieldCodec<std::int64_t>::parse(text)}; }
  static std::string                format(const Cents& amount) { return pg::FieldCodec<std::int64_t>::format(amount.value); }
};

template <>
struct schemacht::ColumnType<Labelled> {
  static constexpr std::string_view SQL_TYPE = "text";
  static Labelled                   parse(std::string_view text) { return Labelled{std::string(text)}; }
  static std::string                format(const Labelled& value) { return value.text; }
};

template <>
class schemacht::query::sql::SqlType<Labelled> {
 public:
  static constexpr std::string_view NAME = "varchar";
};

template <>
struct schemacht::ColumnType<Half> {
  static constexpr std::string_view SQL_TYPE = "half";
  static Half                       parse(std::string_view /*text*/) { return {}; }
};

TEST_CASE("a ColumnType makes a type a column type", "[column_type]") {
  STATIC_REQUIRE(schemacht::UserColumnType<Cents>);
  STATIC_REQUIRE(pg::HasTextForm<Cents>);
  STATIC_REQUIRE_FALSE(pg::HasBinaryForm<Cents>);       // rows holding it are read as text
  STATIC_REQUIRE(pg::HasTextForm<std::vector<Cents>>);  // and so is an array of it
  STATIC_REQUIRE(sql::type_name<Cents>() == "numeric(12,2)");

  STATIC_REQUIRE_FALSE(schemacht::UserColumnType<Half>);
  STATIC_REQUIRE_FALSE(pg::HasTextForm<Half>);

  STATIC_REQUIRE(sql::type_name<Labelled>() == "varchar");  // an SqlType specialization comes first
  STATIC_REQUIRE(sql::type_name<std::int16_t>() == "smallint");

  CHECK(pg::format_field(Cents{1999}) == "1999");
  CHECK(pg::parse_field<Cents>("7").value == 7);
}

TEST_CASE("a WrappedColumn is stored as the type it wraps", "[column_type]") {
  STATIC_REQUIRE(schemacht::WrappableMember<&OrderId::value>);
  STATIC_REQUIRE(pg::HasBinaryForm<OrderId>);
  STATIC_REQUIRE(sql::type_name<OrderId>() == "bigint");

  CHECK(pg::format_field(OrderId{42}) == "42");
  CHECK(pg::parse_field<OrderId>("-3") == OrderId{-3});
  const std::vector<std::byte> eight{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}, std::byte{0}};
  CHECK(pg::FieldCodec<OrderId>::parse_binary(eight) == OrderId{256});
  CHECK(pg::FieldCodec<OrderId>::binary_accepts(pg::detail::OID_INT8));
  CHECK_FALSE(pg::FieldCodec<OrderId>::binary_accepts(pg::detail::OID_TEXT));
}

TEST_CASE("an EnumColumn is stored as its labels", "[column_type]") {
  STATIC_REQUIRE(pg::HasBinaryForm<Color>);
  STATIC_REQUIRE(sql::type_name<Color>() == "color");

  CHECK(pg::format_field(Color::Green) == "green");
  CHECK(pg::parse_field<Color>("red") == Color::Red);
  CHECK(pg::FieldCodec<Color>::parse_binary(bytes_of("green")) == Color::Green);
  CHECK_THROWS_AS(pg::parse_field<Color>("blue"), std::invalid_argument);
  CHECK_THROWS_AS(pg::format_field(static_cast<Color>(7)), std::invalid_argument);
}

TEST_CASE("a column of an own type is inserted with its SQL type", "[column_type][insert]") {
  using Id = sch::Field<OrderId, "id", sch::KeyRole::Primary>;
  using Shade = sch::Field<Color, "shade">;
  using Paints = sch::Schema<"paints", Id, Shade>;
  const std::vector<Paints::row_type> rows{Paints::make_row(OrderId{1}, Color::Red)};
  const auto                          statements = pg::detail::build_insert<Paints>(rows);
  CHECK(statements.at(0).sql == R"(INSERT INTO "paints" ("id", "shade") SELECT * FROM unnest($1::bigint[], $2::color[]))");
  CHECK(statements.at(0).values == std::vector<std::string>{R"({"1"})", R"({"red"})"});
}

TEST_CASE("as_text views bytes as the characters they are", "[column_type]") {
  const std::vector<std::byte> bytes = bytes_of("abc");
  CHECK(pg::as_text(bytes) == "abc");
  CHECK(pg::as_text(std::span<const std::byte>{}).empty());
}
