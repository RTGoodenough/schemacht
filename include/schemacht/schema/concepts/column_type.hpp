#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace schemacht {

template <typename value_t>
struct ColumnType;  // schema/column_type.hpp

/** @brief Whether `ColumnType<value_t>` is specialized with an SQL type's name and a text form (`parse` and `format`). */
template <typename value_t>
concept UserColumnType = requires(const value_t& value, std::string_view text) {
  { ColumnType<value_t>::SQL_TYPE } -> std::convertible_to<std::string_view>;
  { ColumnType<value_t>::parse(text) } -> std::same_as<value_t>;
  { ColumnType<value_t>::format(value) } -> std::same_as<std::string>;
};

/** @brief Whether a `UserColumnType` also reads PostgreSQL's binary form (`parse_binary`). */
template <typename value_t>
concept UserColumnTypeWithBinaryForm = UserColumnType<value_t> && requires(std::span<const std::byte> bytes) {
  { ColumnType<value_t>::parse_binary(bytes) } -> std::same_as<value_t>;
};

/** @brief Whether a `UserColumnType` checks a result column's PostgreSQL type before reading it in binary (`binary_accepts`). */
template <typename value_t>
concept UserColumnTypeWithTypeCheck = UserColumnType<value_t> && requires(std::uint32_t type_oid) {
  { ColumnType<value_t>::binary_accepts(type_oid) } -> std::convertible_to<bool>;
};

}  // namespace schemacht
