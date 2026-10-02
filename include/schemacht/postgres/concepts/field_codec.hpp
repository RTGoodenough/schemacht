#pragma once

#include <concepts>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace schemacht::postgres {

template <typename data_t>
class FieldCodec;  // postgres/field_codec.hpp

/**
 * @brief Whether `data_t` has a text form: its `FieldCodec` has `parse` and `format`. The built-in types do, as does a
 * `std::vector` of a type that does (an array); any other type needs a `ColumnType` specialization (or a `FieldCodec` one)
 * to be a column's type or a statement's argument.
 */
template <typename data_t>
concept HasTextForm = requires(const data_t& value, std::string_view text) {
  { FieldCodec<data_t>::format(value) } -> std::same_as<std::string>;
  { FieldCodec<data_t>::parse(text) } -> std::same_as<data_t>;
};

/** @brief Whether `data_t` can be read from PostgreSQL's binary form: its `FieldCodec` has `parse_binary`. */
template <typename data_t>
concept HasBinaryForm = requires(std::span<const std::byte> bytes) {
  { FieldCodec<data_t>::parse_binary(bytes) } -> std::same_as<data_t>;
};

}  // namespace schemacht::postgres
