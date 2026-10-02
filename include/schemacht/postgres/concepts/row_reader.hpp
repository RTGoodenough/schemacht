#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "schemacht/postgres/concepts/field_codec.hpp"
#include "schemacht/schema/concepts/field.hpp"

namespace schemacht::postgres {

/**
 * @brief A chunk of a query's result, as `read_rows` reads it (`QueryResult` is one).
 *
 * `field` gives a value as its text, or as its bytes in binary form when `is_binary()`, and `std::nullopt` for NULL.
 */
template <typename chunk_t>
concept ResultChunk = requires(const chunk_t& chunk, std::size_t index) {
  { chunk.row_count() } -> std::convertible_to<std::size_t>;
  { chunk.column_count() } -> std::convertible_to<std::size_t>;
  { chunk.is_binary() } -> std::convertible_to<bool>;
  { chunk.column_type(index) } -> std::convertible_to<std::uint32_t>;
  { chunk.field(index, index) } -> std::same_as<std::optional<std::string_view>>;
};

namespace detail {

template <schema::FieldTuple result_t, std::size_t... pos>
consteval bool every_field_has_text_form(std::index_sequence<pos...> /*positions*/) {
  return (HasTextForm<typename std::tuple_element_t<pos, result_t>::value_type> && ...);
}

template <schema::FieldTuple result_t, std::size_t... pos>
consteval bool every_field_has_binary_form(std::index_sequence<pos...> /*positions*/) {
  return (HasBinaryForm<typename std::tuple_element_t<pos, result_t>::value_type> && ...);
}

}  // namespace detail

/**
 * @brief A row whose every column's type has a text form (`HasTextForm`): it can be read from a text result, and written as a
 * statement's values. Every row a `Database` reads or writes is one.
 */
template <typename result_t>
concept TextRow =
    schema::FieldTuple<result_t> && detail::every_field_has_text_form<result_t>(std::make_index_sequence<std::tuple_size_v<result_t>>{});

/**
 * @brief A row whose every column's type can be read from PostgreSQL's binary form (`HasBinaryForm`). A row that is not is
 * always read as text.
 */
template <typename result_t>
concept BinaryReadableRow =
    schema::FieldTuple<result_t> && detail::every_field_has_binary_form<result_t>(std::make_index_sequence<std::tuple_size_v<result_t>>{});

/**
 * @brief A function rows are handed to (by `read_rows`, and so by `Database::stream`): invocable with a `result_t&&`,
 * returning `void`, or a `bool` where `false` means it wants no more.
 */
template <typename handler_t, typename result_t>
concept RowCallback =
    schema::FieldTuple<result_t> && std::invocable<handler_t&, result_t&&> &&
    (std::same_as<std::invoke_result_t<handler_t&, result_t&&>, void> || std::same_as<std::invoke_result_t<handler_t&, result_t&&>, bool>);

}  // namespace schemacht::postgres
