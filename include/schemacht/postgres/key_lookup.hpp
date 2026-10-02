#pragma once

#include <span>
#include <vector>

#include "schemacht/query/prepared.hpp"
#include "schemacht/schema/schema.hpp"
#include "schemacht/util/compile_time.hpp"

namespace schemacht::postgres {

/** @brief The type of the values of the column named `key_col` in `schema_t` (a lookup is by value, and never for a NULL). */
template <schema::IsSchema schema_t, util::CTString key_col>
using key_type = schema_t::template field_by_name<key_col>::value_type;

/** @brief The type of the values of `schema_t`'s primary key, which is one column. */
template <schema::HasSingleColumnKey schema_t>
using primary_key_type = schema::primary_key_field<schema_t>::value_type;

/**
 * @return The query for every row whose `key_col` value is one of `keys`: what `Database::select_by_key_values` runs. Not necessarily
 * the primary key: any column will do.
 */
template <schema::IsSchema schema_t, util::CTString key_col>
[[nodiscard]] auto by_key_values(std::span<const key_type<schema_t, key_col>> keys) {
  static constexpr auto BY_KEYS = query::On<schema_t>::template col<key_col>().in(query::arg<0>());
  return query::Prepared<BY_KEYS>::bind(std::vector<key_type<schema_t, key_col>>(keys.begin(), keys.end()));
}

}  // namespace schemacht::postgres
