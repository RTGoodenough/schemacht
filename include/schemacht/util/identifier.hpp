#pragma once

#include <algorithm>
#include <cstddef>
#include <string_view>

namespace schemacht::util {

/** @brief The longest SQL identifier PostgreSQL keeps: it silently cuts a longer one, so two long names could collide. */
inline constexpr std::size_t MAX_IDENTIFIER_LENGTH = 63;

/**
 * @return Whether `name` is a table or column name this library will put in a statement: lowercase letters, digits and
 * underscores, not starting with a digit, at most `MAX_IDENTIFIER_LENGTH` bytes. needs no escaping beyond
 * the double quotes it is written in, whatever it is.
 *
 * The one rule for it, in C++ (the compile-time SELECT emitter and the runtime INSERT builder both use this). The Python
 * loaders apply the same rule in `db.ident`; `shared/identifier_cases.txt` holds names both test suites check.
 */
[[nodiscard]] constexpr bool is_identifier(std::string_view name) {
  if ( name.empty() || name.size() > MAX_IDENTIFIER_LENGTH ) {
    return false;
  }
  if ( name.front() >= '0' && name.front() <= '9' ) {
    return false;
  }
  return std::ranges::all_of(name, [](char chr) { return (chr >= 'a' && chr <= 'z') || (chr >= '0' && chr <= '9') || chr == '_'; });
}

}  // namespace schemacht::util
