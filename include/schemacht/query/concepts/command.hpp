#pragma once

#include <concepts>
#include <cstddef>
#include <type_traits>

#include "schemacht/query/predicate.hpp"
#include "schemacht/schema/schema.hpp"

namespace schemacht::query {

template <schema::IsSchema schema_t, std::size_t set_capacity, std::size_t predicate_capacity>
struct Update;  // query/command.hpp

template <schema::IsSchema schema_t, std::size_t predicate_capacity>
struct Delete;  // query/command.hpp

/** @brief Whether `column_t` is a column of `schema_t` (a `ColumnRef`, as `On<schema_t>::col` gives). */
template <typename column_t, typename schema_t>
concept ColumnOf = schema::IsSchema<schema_t> && requires { column_t::INDEX; } && std::same_as<column_t, ColumnRef<schema_t, column_t::INDEX>>;

/** @brief Whether an update can set `column_t`: a column of `schema_t` that the database does not compute (`GENERATED`). */
template <typename column_t, typename schema_t>
concept SettableColumn = ColumnOf<column_t, schema_t> && ! schema_t::template field_at<column_t::INDEX>::GENERATED;

/** @brief Whether an update can set `column_t` to NULL: a settable column whose field is a `std::optional`. */
template <typename column_t, typename schema_t>
concept NullableColumn = SettableColumn<column_t, schema_t> && column_t::NULLABLE;

namespace detail {

template <typename data_t>
struct is_update : std::false_type {};

template <schema::IsSchema schema_t, std::size_t set_capacity, std::size_t predicate_capacity>
struct is_update<Update<schema_t, set_capacity, predicate_capacity>> : std::true_type {};

template <typename data_t>
struct is_delete : std::false_type {};

template <schema::IsSchema schema_t, std::size_t predicate_capacity>
struct is_delete<Delete<schema_t, predicate_capacity>> : std::true_type {};

}  // namespace detail

/** @brief Detects whether `data_t` is an `Update<...>`. */
template <typename data_t>
concept IsUpdate = detail::is_update<data_t>::value;

/** @brief Detects whether `data_t` is a `Delete<...>`. */
template <typename data_t>
concept IsDelete = detail::is_delete<data_t>::value;

/** @brief What `Prepared` accepts besides a query: a statement that changes rows and returns none. */
template <typename data_t>
concept IsCommandSource = IsUpdate<data_t> || IsDelete<data_t>;

}  // namespace schemacht::query
