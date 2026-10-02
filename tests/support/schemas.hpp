#pragma once

// Schemas and module-style operations shared by the tests.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

/** @brief pg_trgm's similarity of texts, as an extension's operations are declared: the worked example of `query::Operation`. */
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

}  // namespace schemacht::test

template <>
class schemacht::query::sql::SelectExpression<schemacht::test::Tile> {
 public:
  static constexpr std::string_view FUNCTION = "ST_AsBinary";
};
