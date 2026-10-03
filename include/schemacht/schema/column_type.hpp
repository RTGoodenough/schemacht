#pragma once

#include "schemacht/schema/concepts/column_type.hpp"  // IWYU pragma: export

namespace schemacht {

/**
 * @brief How a column type of your own is stored: specialize this once, next to the type, and the type can be a `Field`'s,
 * a query's argument, and inserted and read like a built-in one.
 *
 * The specialization has, as static members:
 * - `SQL_TYPE`, what the database calls the type (modifiers are fine: `numeric(12,2)`);
 * - `T parse(std::string_view text)`, from its text form, as PostgreSQL prints it;
 * - `std::string format(const T& value)`, to its text form, as PostgreSQL reads it;
 * - optionally `T parse_binary(std::span<const std::byte> bytes)`, from its binary form, so rows are read in binary;
 * - optionally `bool binary_accepts(std::uint32_t type_oid)`, to refuse a column of another type before any row is read.
 *
 * Without `parse_binary`, rows holding the type are read as text, which needs nothing more. `WrappedColumn` and `EnumColumn`
 * (schemacht/column_type.hpp) write the whole specialization for a type that wraps a built-in one and for an enum.
 *
 * The primary template is left undefined: a type is a column type of its own exactly when this is specialized for it (see
 * `UserColumnType`).
 */
template <typename value_t>
struct ColumnType;

}  // namespace schemacht
