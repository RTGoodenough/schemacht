#pragma once

#include <algorithm>
#include <cstddef>
#include <string_view>

namespace schemacht::util {

/** @brief The longest SQL identifier PostgreSQL keeps: it silently truncates a longer one, so two long names could collide. */
inline constexpr std::size_t MAX_IDENTIFIER_LENGTH = 63;

namespace detail {

[[nodiscard]] constexpr bool is_digit(char chr) { return chr >= '0' && chr <= '9'; }
[[nodiscard]] constexpr bool is_lower(char chr) { return chr >= 'a' && chr <= 'z'; }
[[nodiscard]] constexpr bool is_upper(char chr) { return chr >= 'A' && chr <= 'Z'; }

}  // namespace detail

/**
 * @return Whether `name` is a valid table or column name: lowercase letters, digits and underscores, not starting with a
 * digit, at most `MAX_IDENTIFIER_LENGTH` bytes. Such a name is safe to write between double quotes with no escaping.
 */
[[nodiscard]] constexpr bool is_identifier(std::string_view name) {
  if ( name.empty() || name.size() > MAX_IDENTIFIER_LENGTH || detail::is_digit(name.front()) ) {
    return false;
  }

  return std::ranges::all_of(name, [](char chr) { return detail::is_lower(chr) || detail::is_digit(chr) || chr == '_'; });
}

namespace detail {

/** @return Whether `word` is one part of a SQL function name: letters, digits and underscores, not starting with a digit. */
[[nodiscard]] constexpr bool is_function_word(std::string_view word) {
  if ( word.empty() || is_digit(word.front()) ) {
    return false;
  }

  return std::ranges::all_of(word, [](char chr) { return is_lower(chr) || is_upper(chr) || is_digit(chr) || chr == '_'; });
}

}  // namespace detail

/**
 * @return Whether `name` is a valid SQL function name: letters, digits and underscores, not starting with a digit; optionally
 * after the schema it is in and a dot (`stats.add_counts`), to call it whatever the search path is.
 */
[[nodiscard]] constexpr bool is_sql_function_name(std::string_view name) {
  const std::size_t dot = name.find('.');
  if ( dot == std::string_view::npos ) {
    return detail::is_function_word(name);
  }

  return detail::is_function_word(name.substr(0, dot)) && detail::is_function_word(name.substr(dot + 1));
}

/**
 * @return Whether `name` is a SQL operator PostgreSQL reads as a single operator, such as `<->` or `@>`.
 *
 * It must be only operator characters and must not contain `--` or slash-star (which start a comment). An operator of several
 * characters may end in `+` or `-` only if it also contains one of `~ ! @ # % ^ & | ?`; otherwise PostgreSQL reads the
 * trailing sign as a separate operator.
 */
[[nodiscard]] constexpr bool is_sql_operator(std::string_view name) {
  constexpr std::string_view OPERATOR_CHARS = "<>-|#&~=@+*/%^!?";
  constexpr std::string_view SIGN_ALLOWING_CHARS = "~!@#%^&|?";

  if ( name.empty() || ! std::ranges::all_of(name, [&](char chr) { return OPERATOR_CHARS.contains(chr); }) ) {
    return false;
  }

  if ( name.contains("--") || name.contains("/*") ) {
    return false;
  }

  const bool ends_in_sign = name.back() == '+' || name.back() == '-';
  return name.size() == 1 || ! ends_in_sign || name.find_first_of(SIGN_ALLOWING_CHARS) != std::string_view::npos;
}

}  // namespace schemacht::util
