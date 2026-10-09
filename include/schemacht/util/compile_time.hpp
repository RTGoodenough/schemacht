#pragma once

// The headers use C++20: field names are class-type template parameters, templates are constrained with concepts, and SQL is
// written by consteval functions. Without this, an older standard gives errors that do not say so: clang reads
// `Field<T, "name">` in C++17 as a template parameter of type `int` and reports that a string is not convertible to it.
#if ! defined(_MSC_VER) && __cplusplus < 202002L
#error \
    "schemacht requires C++20: compile with -std=c++20 or later (CMake: link schemacht::schemacht, which asks for it). If your editor shows this but your build works, the editor is not using your build's compile flags (for clangd: a compile_commands.json)."
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <ranges>
#include <string_view>

#include "schemacht/util/concepts/compile_time.hpp"  // IWYU pragma: export

namespace schemacht::util {

/**
 * @brief A compile-time string, usable as a non-type template parameter.
 *
 * @tparam n_sz The string's length, including its trailing null terminator
 *              (deduced from a string literal via CTAD).
 */
template <size_t n_sz>
struct CTString {
  std::array<char, n_sz> value{};

  // Implicit on purpose: a string literal becomes a `CTString` template argument (`Field<int, "id">`), which takes this conversion.
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays, google-explicit-constructor, hicpp-explicit-conversions, cppcoreguidelines-explicit-constructor, misc-explicit-constructor, cppcoreguidelines-pro-bounds-array-to-pointer-decay)
  constexpr CTString(const char (&str)[n_sz]) { std::copy_n(str, n_sz, value.begin()); }

  /** @return The string's contents, without the trailing null terminator. */
  [[nodiscard]] constexpr std::string_view view() const { return {value.data(), n_sz - 1}; }

  constexpr CTString(const CTString&) = default;
  constexpr CTString(CTString&&) noexcept = default;
  constexpr CTString& operator=(const CTString&) = default;
  constexpr CTString& operator=(CTString&&) noexcept = default;
  constexpr ~CTString() = default;
};

/**
 * @brief Joins compile-time strings into one CTString
 *
 *
 * @tparam parts The pieces, in order.
 */
template <CTString... parts>
[[nodiscard]] consteval auto concat_ctstrings() {
  constexpr std::size_t TOTAL = (std::size_t{0} + ... + parts.view().size()) + 1;  // +1 for the trailing null terminator CTString expects
  char                  joined[TOTAL]{};  // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  char*                 cursor = joined;  // NOLINT(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
  ((cursor = std::ranges::copy(parts.view(), cursor).out), ...);

  return CTString<TOTAL>(joined);
}

/**
 * @return Whether no two of `values` are equal: for checking names or indices while compiling, such as a schema's column names
 * (`static_assert(util::all_distinct(names), "...")`). Any forward range of comparable values: an array, a span, a slice.
 * It compares every pair, so it is for the few values of a schema or a statement, not for large data.
 */
template <EqualityComparableRange range_t>
[[nodiscard]] constexpr bool all_distinct(const range_t& values) {
  std::size_t seen = 0;
  for ( const auto& value : values ) {
    ++seen;
    for ( const auto& later : values | std::views::drop(seen) ) {
      if ( value == later ) {
        return false;
      }
    }
  }

  return true;
}

}  // namespace schemacht::util
