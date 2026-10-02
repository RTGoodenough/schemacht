#pragma once

#include <concepts>
#include <type_traits>

#include "schemacht/schema/concepts/field.hpp"
#include "schemacht/util/compile_time.hpp"

namespace schemacht::schema {

template <util::CTString table_str, IsField... field_ts>
class Schema;  // schema/schema.hpp

namespace detail {

template <typename t>
struct is_schema : std::false_type {};

template <util::CTString table_str, typename... field_ts>
struct is_schema<Schema<table_str, field_ts...>> : std::true_type {};

}  // namespace detail

/** @brief Detects whether `data_t` is a `Schema<...>` specialization. */
template <typename data_t>
concept IsSchema = detail::is_schema<data_t>::value;

/** @brief Whether `schema_t` has a primary key of exactly one column, so a row can be found by that one value. */
template <typename schema_t>
concept HasSingleColumnKey = IsSchema<schema_t> && schema_t::PRIMARY_KEY_FIELD_COUNT == 1;

/** @brief Whether some field of `schema_t` is `GENERATED`, so that its `insert_row_type` is not its `row_type`. */
template <typename schema_t>
concept HasGeneratedColumn = IsSchema<schema_t> && schema_t::INSERTABLE_FIELD_COUNT < schema_t::FIELD_COUNT;

/**
 * @brief A row `insert` takes for `schema_t`: a whole `row_type` (the values of its generated columns are not sent), or an
 * `insert_row_type`, which has none.
 */
template <typename row_t, typename schema_t>
concept InsertRowOf =
    IsSchema<schema_t> && (std::same_as<row_t, typename schema_t::row_type> || std::same_as<row_t, typename schema_t::insert_row_type>);

}  // namespace schemacht::schema
