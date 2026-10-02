#pragma once

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

}  // namespace schemacht::schema
