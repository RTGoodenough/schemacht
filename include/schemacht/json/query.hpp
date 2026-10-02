#pragma once

#include <concepts>
#include <cstddef>
#include <string_view>

#include "schemacht/json/json.hpp"
#include "schemacht/query/predicate.hpp"
#include "schemacht/schema/schema.hpp"

/**
 * Tests on JSON columns for queries: the operator that PostgreSQL answers from a GIN index.
 */
namespace schemacht::json {

/** @brief The document contains another: `"column" @> argument`. */
class Contains {
 public:
  static constexpr std::string_view OPERATOR = "@>";
};

/**
 * @brief Rows whose JSON column contains the document bound to `arg<N>()`:
 *
 * @code
 * using Streets = query::On<StreetsSchema>;   // with a Field<json::Json, "properties">
 * constexpr auto FOOTPATHS = query::select(json::contains(Streets::col<"properties">(), query::arg<0>()));
 * database.execute(query::Prepared<FOOTPATHS>::bind(json::Json(R"({"highway":"footway","surface":"paved"})")));
 * @endcode
 *
 * Written as the operator, because that is what a GIN index (`CREATE INDEX ... USING GIN (properties)`) answers; the
 * function behind it, `jsonb_contains`, is a sequential scan. "Contains" is `jsonb`'s: an object contains another with
 * the same keys and values, possibly with more; an array contains another whose elements it has, in any order and
 * however often; and an array contains a scalar it has as an element. Negate with `!`.
 * For a key that many queries filter on, a column of its own is cheaper than any index on a document.
 */
template <schema::IsSchema schema_t, std::size_t column_idx, std::size_t slot_idx>
  requires std::same_as<typename query::ColumnRef<schema_t, column_idx>::data_type, Json>
[[nodiscard]] constexpr auto contains(const query::ColumnRef<schema_t, column_idx>& column, query::ArgRef<slot_idx> arg) {
  return column.template apply<Contains>(arg);
}

}  // namespace schemacht::json
