#pragma once

// Field names are class-type template parameters (C++20), and the headers use C++23 (std::move_only_function, std::byteswap, ...).
// Without this, an older standard gives errors that do not say so: clang reads `Field<T, "name">` in C++17 as a template
// parameter of type `int` and reports that a string is not convertible to it.
#if ! defined(_MSC_VER) && __cplusplus < 202100L
#error \
    "schemacht requires C++23: compile with -std=c++23 (CMake: link schemacht::schemacht, which asks for it). If your editor shows this but your build works, the editor is not using your build's compile flags (for clangd: a compile_commands.json)."
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>

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

  // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays, google-explicit-constructor, modernize-avoid-c-arrays) need compile time
  constexpr CTString(const char (&str)[n_sz]) { std::copy_n(str, n_sz, value.begin()); }

  constexpr CTString(const CTString&) = default;
  constexpr CTString(CTString&&) noexcept = default;
  constexpr CTString& operator=(const CTString&) = default;
  constexpr CTString& operator=(CTString&&) noexcept = default;
  constexpr ~CTString() = default;

  /** @return The string's contents, without the trailing null terminator. */
  [[nodiscard]] constexpr std::string_view view() const { return {value.data(), n_sz - 1}; }
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

}  // namespace schemacht::util
