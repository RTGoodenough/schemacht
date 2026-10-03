#pragma once

// Schemas and module-style operations shared by the tests.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "schemacht/column_type.hpp"
#include "schemacht/json.hpp"
#include "schemacht/schema.hpp"

namespace schemacht::test {

namespace sch = schemacht::schema;

using Id = sch::Field<std::int64_t, "id", sch::KeyRole::Primary>;
using Name = sch::Field<std::string, "name">;
using Nick = sch::Field<std::optional<std::string>, "nick">;
using Score = sch::Field<double, "score">;
using Active = sch::Field<bool, "active">;

using People = sch::Schema<"people", Id, Name, Nick, Score, Active>;

using Count = sch::Field<std::uint32_t, "count">;
using Doc = sch::Field<json::Json, "doc">;
using Counted = sch::Schema<"counted", Id, Count, Doc>;

/** @brief A schema with an array-typed column, which `insert` cannot send with `unnest`. */
using Tags = sch::Field<std::vector<std::string>, "tags">;
using Tagged = sch::Schema<"tagged", Id, Tags>;

/** @brief A schema with a generated column, which `insert` leaves out. */
using Doubled = sch::Field<std::int64_t, "doubled", sch::KeyRole::None, true>;
using WithGenerated = sch::Schema<"with_generated", Id, Doubled, Name>;

/** @brief A schema whose key the database numbers: an identity column, which `create_table` writes. */
using NumberedId = sch::Field<std::int32_t, "id", sch::KeyRole::Primary, true>;
using Numbered = sch::Schema<"numbered", NumberedId, Name, Nick>;

/** @brief pg_trgm's similarity of texts, declared as examples/extensions.cpp declares it. */
namespace trgm {

/** @brief A test written as an operator: `"column" % $1::text`, similar enough by the `pg_trgm.similarity_threshold` setting. */
struct Similar {
  static constexpr std::string_view OPERATOR = "%";
};

/** @brief A ranking: `"column" <-> $1::text`, 1 minus the similarity, so the most similar sort first. */
struct Distance {
  static constexpr std::string_view OPERATOR = "<->";
};

/**
 * @brief A test that takes a number too, so a function: `similar_above("column", $1::text, $2::double precision)`. Not
 * pg_trgm's own (its threshold is a setting): the integration test creates it.
 */
struct SimilarAbove {
  static constexpr std::string_view FUNCTION = "similar_above";
  static constexpr bool             TAKES_NUMBER = true;
};

}  // namespace trgm

/** @brief A column type read through a function of it, as PostGIS's raster is. */
struct Tile {
  std::vector<std::byte> bytes;
};

using Raster = sch::Field<Tile, "rast">;
using Tiles = sch::Schema<"tiles", Id, Raster>;

/** @brief PostgreSQL's geometric `box` and `point`, kept as their text: a column of one type tested with an argument of another. */
struct Box {
  std::string text;  // "(1,1),(0,0)"
};
struct Point {
  std::string text;  // "(0.5,0.5)"
};

using Area = sch::Field<Box, "area">;
using Regions = sch::Schema<"regions", Id, Area>;

namespace geo {

/** @brief `"area" @> $1::point`: the box covers the point. Its argument is a `Point`, not the column's `Box`. */
struct Covers {
  static constexpr std::string_view OPERATOR = "@>";
  using argument_type = Point;
};

/** @brief `"area" <-> $1::point`: the distance from the box to the point, so the nearest sort first. */
struct Nearest {
  static constexpr std::string_view OPERATOR = "<->";
  using argument_type = Point;
};

}  // namespace geo

/** @brief A jsonb key, sent as text: the argument of `HasKey`. */
struct JsonKey {
  std::string value;
};

/** @brief jsonb's "has this key" (`"doc" ? $1::text`): an operation built into PostgreSQL whose argument is not the column's type. */
struct HasKey {
  static constexpr std::string_view OPERATOR = "?";
  using argument_type = JsonKey;
};

}  // namespace schemacht::test

template <>
struct schemacht::ColumnType<schemacht::test::Box> {  // text only: a box's binary form is not text's
  static constexpr std::string_view SQL_TYPE = "box";

  static schemacht::test::Box parse(std::string_view text) { return {std::string(text)}; }
  static std::string          format(const schemacht::test::Box& box) { return box.text; }
};

template <>
struct schemacht::ColumnType<schemacht::test::Point> {  // text only, as `Box`
  static constexpr std::string_view SQL_TYPE = "point";

  static schemacht::test::Point parse(std::string_view text) { return {std::string(text)}; }
  static std::string            format(const schemacht::test::Point& point) { return point.text; }
};

template <>
struct schemacht::ColumnType<schemacht::test::JsonKey> : schemacht::WrappedColumn<&schemacht::test::JsonKey::value> {};

template <>
class schemacht::query::sql::SelectExpression<schemacht::test::Tile> {
 public:
  static constexpr std::string_view FUNCTION = "ST_AsBinary";
};
