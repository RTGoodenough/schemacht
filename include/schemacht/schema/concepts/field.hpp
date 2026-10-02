#pragma once

#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>

#include "schemacht/util/compile_time.hpp"
#include "schemacht/util/concepts/type_utils.hpp"

namespace schemacht::schema {

enum class KeyRole : std::uint8_t;  // schema/field.hpp

template <typename data_t, util::CTString column_str, KeyRole key_role, bool generated>
struct Field;  // schema/field.hpp

namespace detail {

template <typename t>
struct is_field : std::false_type {};

template <typename data_t, util::CTString column_str, KeyRole key_role, bool generated>
struct is_field<Field<data_t, column_str, key_role, generated>> : std::true_type {};

}  // namespace detail

/** @brief Detects whether `data_t` is a `Field<...>` specialization. */
template <typename data_t>
concept IsField = detail::is_field<data_t>::value;

namespace detail {

template <typename t>
struct is_field_tuple : std::false_type {};

template <IsField... field_ts>
struct is_field_tuple<std::tuple<field_ts...>> : std::true_type {};

}  // namespace detail

/** @brief A row as queries return it: a `std::tuple` of `Field`s, one per returned column, in result order. */
template <typename row_t>
concept FieldTuple = detail::is_field_tuple<row_t>::value;

namespace detail {

template <util::CTString name, IsField... field_ts>
[[nodiscard]] consteval std::size_t count_named(const std::tuple<field_ts...>* /*row*/) {
  return ((field_ts::COLUMN_NAME.view() == name.view() ? 1U : 0U) + ... + 0U);
}

template <typename row_t, typename... arg_ts>
inline constexpr bool ROW_VALUES = false;

template <IsField... field_ts, typename... arg_ts>
  requires(sizeof...(field_ts) == sizeof...(arg_ts))
inline constexpr bool ROW_VALUES<std::tuple<field_ts...>, arg_ts...> = (util::NonNarrowingTo<arg_ts, typename field_ts::data_type> && ...);

}  // namespace detail

/** @brief Whether exactly one field of the row `row_t` (a `FieldTuple`) is the column `name`, so `get<name>` can find it. */
template <typename row_t, util::CTString name>
concept HasColumnNamed = FieldTuple<row_t> && (detail::count_named<name>(static_cast<const row_t*>(nullptr)) == 1);

/**
 * @brief Whether `arg_ts` are a value for each field of the row `row_t`, in order: each converts to its field's `data_type`
 * without narrowing (`std::nullopt` for a column that can be NULL).
 */
template <typename row_t, typename... arg_ts>
concept RowValues = FieldTuple<row_t> && detail::ROW_VALUES<row_t, arg_ts...>;

}  // namespace schemacht::schema
