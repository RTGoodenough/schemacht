#pragma once

#include <span>
#include <string>
#include <string_view>

namespace schemacht::postgres {

/**
 * @brief Validates and quotes a SQL identifier.
 *
 * - Requires a safe, unquoted, lowercase snake_case name.
 * - Returns it double-quoted, ready to interpolate into a query string.
 *
 * @param name The identifier to validate.
 * @return The double-quoted identifier.
 * @throws std::invalid_argument if `name` isn't a valid identifier.
 */
[[nodiscard]] std::string quote_identifier(const std::string& name);

/**
 * @brief Quotes and comma-joins a list of SQL identifiers (e.g. for a column list).
 * @param names The identifiers to quote and join, in order.
 * @return The quoted, comma-separated identifiers.
 * @throws std::invalid_argument if any name isn't a valid identifier.
 */
[[nodiscard]] std::string quote_identifier_list(std::span<const std::string_view> names);

}  // namespace schemacht::postgres
