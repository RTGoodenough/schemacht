#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace schemacht {

/**
 * @brief How a column type of your own is stored: specialize this once, next to the type, and the type can be a `Field`'s,
 * a query's argument, and inserted and read like a built-in one.
 *
 * @code
 * template <>
 * struct schemacht::ColumnType<Money> {
 *   static constexpr std::string_view SQL_TYPE = "numeric(12,2)";  // what the database calls it; modifiers are fine
 *
 *   static Money       parse(std::string_view text);               // its text form, as PostgreSQL prints it
 *   static std::string format(const Money& amount);                // its text form, as PostgreSQL reads it
 *
 *   static Money parse_binary(std::span<const std::byte> bytes);   // optional: its binary form, so rows are read in binary
 *   static bool  binary_accepts(std::uint32_t type_oid);           // optional: refuse a column of another type up front
 * };
 * @endcode
 *
 * Without `parse_binary`, rows holding the type are read as text, which needs nothing more. `WrappedColumn` and `EnumColumn`
 * (schemacht/column_type.hpp) write the whole specialization for a type that wraps a built-in one and for an enum.
 *
 * The primary template is left undefined: a type is a column type of its own exactly when this is specialized for it (see
 * `UserColumnType`).
 */
template <typename value_t>
struct ColumnType;

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
