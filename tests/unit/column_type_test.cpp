#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "schemacht/column_type.hpp"
#include "schemacht/postgres/statements.hpp"
#include "schemacht/query/predicate.hpp"
#include "schemacht/query/prepared.hpp"
#include "schemacht/schema/schema.hpp"

namespace pg = schemacht::postgres;
namespace q = schemacht::query;
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

enum class Priority : std::uint8_t {
  Low,
  Normal,
  High,
};

/** @brief An exact amount in cents. */
struct Price {
  std::int64_t cents = 0;

  auto operator<=>(const Price&) const = default;
};

/** @brief A count too narrow to be a scaled amount. */
struct Narrow {
  std::int32_t value = 0;
};

/** @return `values` as big-endian 16-bit words. */
std::vector<std::byte> words(std::initializer_list<int> values) {
  std::vector<std::byte> bytes;
  for ( const int value : values ) {
    const auto word = static_cast<std::uint16_t>(value);
    bytes.push_back(static_cast<std::byte>(word >> 8U));
    bytes.push_back(static_cast<std::byte>(word & 0xFFU));
  }
  return bytes;
}

std::vector<std::byte> bytes_of(std::string_view text) {
  std::vector<std::byte> bytes;
  for ( const char chr : text ) {
    bytes.push_back(static_cast<std::byte>(chr));
  }
  return bytes;
}

/** @brief Several labels as one value: an array inside a column type of one's own. */
struct LabelList {
  std::vector<std::string> values;
};

/** @brief A code of at most three characters: `varchar(3)`. */
struct Code {
  std::string text;
};

/** @brief Initials of exactly two characters: `char(2)`. */
struct Initials {
  std::string text;
};

/** @brief A point in WGS 84, as hex EWKB: a `geometry(Point,4326)`, whose modifier is a geometry type and an SRID, not a size. */
struct Spot {
  std::string ewkb;
};

}  // namespace

template <>
struct schemacht::ColumnType<Code> {
  static constexpr std::string_view SQL_TYPE = "varchar(3)";
  static Code                       parse(std::string_view text) { return Code{std::string(text)}; }
  static std::string                format(const Code& code) { return code.text; }
};

template <>
struct schemacht::ColumnType<Initials> {
  static constexpr std::string_view SQL_TYPE = "char(2)";
  static Initials                   parse(std::string_view text) { return Initials{std::string(text)}; }
  static std::string                format(const Initials& initials) { return initials.text; }
};

template <>
struct schemacht::ColumnType<Spot> {
  static constexpr std::string_view SQL_TYPE = "geometry(Point,4326)";
  static Spot                       parse(std::string_view text) { return Spot{std::string(text)}; }
  static std::string                format(const Spot& spot) { return spot.ewkb; }
};

template <>
struct schemacht::ColumnType<OrderId> : schemacht::WrappedColumn<&OrderId::value> {};

template <>
struct schemacht::ColumnType<LabelList> : schemacht::WrappedColumn<&LabelList::values> {};

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
struct schemacht::ColumnType<Priority> : schemacht::EnumIntegerColumn<Priority> {};

template <>
struct schemacht::ColumnType<Price> : schemacht::ScaledNumericColumn<&Price::cents, 2, "numeric(10,2)"> {};

template <>
struct schemacht::ColumnType<std::filesystem::path>
    : schemacht::ConvertedColumn<
          std::filesystem::path, std::string, [](const std::filesystem::path& path) { return path.string(); },
          [](const std::string& text) { return std::filesystem::path(text); }> {};

template <>
struct schemacht::ColumnType<std::chrono::milliseconds>
    : schemacht::ConvertedColumn<
          std::chrono::milliseconds, std::int64_t, [](std::chrono::milliseconds span) { return span.count(); },
          [](std::int64_t count) { return std::chrono::milliseconds(count); }> {};

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
  CHECK(pg::FieldCodec<OrderId>::binary_accepts(pg::OID_INT8));
  CHECK_FALSE(pg::FieldCodec<OrderId>::binary_accepts(pg::OID_TEXT));
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

// An array literal is split on its element type's own delimiter, which isn't always a comma (PostGIS's geometry uses `:`), so a
// type of one's own goes in an array as text, whose delimiter is, and is cast to its type one value at a time.
TEST_CASE("a column of an own type is inserted as text, cast to its SQL type", "[column_type][insert]") {
  using Id = sch::Field<OrderId, "id", sch::KeyRole::Primary>;
  using Shade = sch::Field<Color, "shade">;
  using Paints = sch::Schema<"paints", Id, Shade>;

  const std::vector<Paints::row_type> rows{Paints::make_row(OrderId{1}, Color::Red)};
  const auto                          statement = pg::insert_statement<Paints>(rows);

  CHECK(
      statement.sql() ==
      R"(INSERT INTO "paints" ("id", "shade") SELECT "id"::bigint, "shade"::color FROM unnest($1::text[], $2::text[]) AS u("id", "shade"))"
  );
  CHECK(std::vector<std::string>(statement.params().begin(), statement.params().end()) == std::vector<std::string>{R"({"1"})", R"({"red"})"});
}

TEST_CASE("a list of an own type is sent as text, cast to its SQL type", "[column_type][sql]") {
  using Paints = sch::Schema<"paints", sch::Field<OrderId, "id", sch::KeyRole::Primary>, sch::Field<Color, "shade">>;
  static constexpr auto SHADE_IN = q::On<Paints>::col<"shade">().in(q::arg<0>());

  STATIC_REQUIRE(q::Prepared<SHADE_IN>::SQL == R"(SELECT "id", "shade" FROM "paints" WHERE "shade" = ANY($1::text[]::color[]))");
}

TEST_CASE("an array inside a column type of one's own is inserted as an array: as text, cast back", "[column_type][insert]") {
  using Listed = sch::Schema<"listed", sch::Field<LabelList, "labels">>;
  STATIC_REQUIRE(sql::type_name<LabelList>() == "text[]");

  const std::vector<Listed::row_type> rows{Listed::make_row(LabelList{{"a", "b"}})};
  CHECK(
      pg::insert_statement<Listed>(rows).sql() == R"(INSERT INTO "listed" ("labels") SELECT "labels"::text[] FROM unnest($1::text[]) AS u("labels"))"
  );
}

TEST_CASE("as_text views bytes as the characters they are", "[column_type]") {
  const std::vector<std::byte> bytes = bytes_of("abc");
  CHECK(pg::as_text(bytes) == "abc");
  CHECK(pg::as_text(std::span<const std::byte>{}).empty());
}

TEST_CASE("an EnumIntegerColumn is stored as its number", "[column_type]") {
  STATIC_REQUIRE(sql::type_name<Priority>() == "smallint");  // a uint8_t's values fit the next wider signed type
  STATIC_REQUIRE(pg::HasBinaryForm<Priority>);

  CHECK(pg::format_field(Priority::High) == "2");
  CHECK(pg::parse_field<Priority>("1") == Priority::Normal);

  CHECK(pg::FieldCodec<Priority>::parse_binary(words({2})) == Priority::High);
  CHECK(pg::FieldCodec<Priority>::binary_accepts(pg::OID_INT2));
  CHECK_FALSE(pg::FieldCodec<Priority>::binary_accepts(pg::OID_TEXT));
}

TEST_CASE("a ScaledNumericColumn is stored as a numeric", "[column_type]") {
  STATIC_REQUIRE(schemacht::ScaledMember<&Price::cents>);
  STATIC_REQUIRE_FALSE(schemacht::ScaledMember<&Narrow::value>);  // an int32 count would overflow long before a numeric(12,2) does
  STATIC_REQUIRE(sql::type_name<Price>() == "numeric(10,2)");

  CHECK(pg::format_field(Price{-5}) == "-0.05");
  CHECK(pg::parse_field<Price>("19.99") == Price{1999});

  CHECK(pg::FieldCodec<Price>::parse_binary(words({2, 0, 0, 2, 19, 9900})) == Price{1999});
  CHECK(pg::FieldCodec<Price>::binary_accepts(pg::OID_NUMERIC));

  CHECK_THROWS_AS(pg::parse_field<Price>("0.001"), std::invalid_argument);
}

TEST_CASE("a ConvertedColumn is stored as another type", "[column_type]") {
  STATIC_REQUIRE(sql::type_name<std::filesystem::path>() == "text");
  STATIC_REQUIRE(sql::type_name<std::chrono::milliseconds>() == "bigint");
  STATIC_REQUIRE(pg::HasBinaryForm<std::chrono::milliseconds>);  // a bigint has one

  CHECK(pg::format_field(std::filesystem::path("/var/log/app.log")) == "/var/log/app.log");
  CHECK(pg::parse_field<std::filesystem::path>("a/b") == std::filesystem::path("a/b"));

  CHECK(pg::format_field(std::chrono::milliseconds(1500)) == "1500");
  CHECK(pg::FieldCodec<std::chrono::milliseconds>::parse_binary(words({0, 0, 0, 7})) == std::chrono::milliseconds(7));
  CHECK(pg::FieldCodec<std::chrono::milliseconds>::binary_accepts(pg::OID_INT8));

  using Wrong = decltype([](int number) { return number; });
  STATIC_REQUIRE_FALSE(schemacht::ConvertsThrough<std::chrono::milliseconds, std::int64_t, Wrong{}, Wrong{}>);
}

// A cast cuts a character or bit string to the type's length (`'abcdef'::varchar(3)` is `abc`), so an argument is cast to the
// type's unlimited form, and a value too long fails where it is stored. `char` and `bit` alone mean a length of one: their
// unlimited forms are `bpchar` and `varbit`. Any other type's modifier stays (a geometry's is its type and SRID).
TEST_CASE("an argument is cast to a string column's type without its length, and to any other type as written", "[column_type][sql]") {
  using Codes = sch::Schema<
      "codes", sch::Field<OrderId, "id", sch::KeyRole::Primary>, sch::Field<Code, "code">, sch::Field<Initials, "initials">,
      sch::Field<std::vector<Initials>, "aliases">>;
  static constexpr auto CODE_IN = q::On<Codes>::col<"code">().in(q::arg<0>());

  const std::vector<Codes::row_type> rows{Codes::make_row(OrderId{1}, Code{"abc"}, Initials{"rg"}, std::vector<Initials>{})};
  CHECK(
      pg::insert_statement<Codes>(rows).sql() ==
      R"(INSERT INTO "codes" ("id", "code", "initials", "aliases") SELECT "id"::bigint, "code"::varchar, "initials"::bpchar, "aliases"::bpchar[] )"
      R"(FROM unnest($1::text[], $2::text[], $3::text[], $4::text[]) AS u("id", "code", "initials", "aliases"))"
  );
  STATIC_REQUIRE(q::Prepared<CODE_IN>::SQL.ends_with(R"(WHERE "code" = ANY($1::text[]::varchar[]))"));

  // A modifier other than a string's length stays: a number's precision, a geometry's type and SRID.
  using Places = sch::Schema<"places", sch::Field<OrderId, "id", sch::KeyRole::Primary>, sch::Field<Cents, "price">, sch::Field<Spot, "spot">>;
  static constexpr auto               SPOT_IN = q::On<Places>::col<"spot">().in(q::arg<0>());
  const std::vector<Places::row_type> places{Places::make_row(OrderId{1}, Cents{100}, Spot{"0101000020E6100000"})};
  CHECK(pg::insert_statement<Places>(places).sql().contains(R"(SELECT "id"::bigint, "price"::numeric(12,2), "spot"::geometry(Point,4326) FROM)"));
  STATIC_REQUIRE(q::Prepared<SPOT_IN>::SQL.ends_with(R"(WHERE "spot" = ANY($1::text[]::geometry(Point,4326)[]))"));

  // The table itself keeps the lengths.
  CHECK(
      pg::create_table_statement<Codes>().sql().contains(R"("code" varchar(3) NOT NULL, "initials" char(2) NOT NULL, "aliases" char(2)[] NOT NULL)")
  );
}
