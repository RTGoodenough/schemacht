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

/** @brief "nearest first", as a module would declare it. */
struct Nearest {
  static constexpr std::string_view OPERATOR = "<->";
};

/** @brief A function test that takes a number too, as a module would declare it. */
struct WithinDistance {
  static constexpr std::string_view FUNCTION = "within_distance";
  static constexpr bool             TAKES_NUMBER = true;
};

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
