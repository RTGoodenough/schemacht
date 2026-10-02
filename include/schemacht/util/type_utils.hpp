#pragma once

#include <concepts>
#include <cstddef>
#include <utility>
#include <vector>

namespace schemacht::util {

/**
 * @brief Whether a `from_t` converts to `to_t` without narrowing: `to_t{from}` compiles.
 *
 * Accepts an `int` for a `std::int64_t` or a string literal for a `std::string`. Rejects a `double` for a `float`, a negative-capable
 * `int` for an unsigned type, and an `int` for a `bool`.
 */
template <typename from_t, typename to_t>
concept NonNarrowingTo = requires(from_t&& from) { to_t{std::forward<from_t>(from)}; };

namespace detail {

template <typename data_t>
inline constexpr bool IS_SQL_ARRAY = false;

template <typename element_t>
inline constexpr bool IS_SQL_ARRAY<std::vector<element_t>> = ! std::same_as<element_t, std::byte>;

}  // namespace detail

/** @brief Whether `data_t` is a PostgreSQL array: a `std::vector` of anything but `std::byte` (a `std::vector<std::byte>` is a `bytea`). */
template <typename data_t>
concept SqlArray = detail::IS_SQL_ARRAY<data_t>;

}  // namespace schemacht::util
