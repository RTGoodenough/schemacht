#pragma once

#include <utility>

namespace schemacht::util {

/**
 * @brief Whether a `from_t` converts to `to_t` without narrowing: `to_t{from}` compiles.
 *
 * Accepts an `int` for a `std::int64_t` or a string literal for a `std::string`. Rejects a `double` for a `float`, a negative-capable
 * `int` for an unsigned type, and an `int` for a `bool`.
 */
template <typename from_t, typename to_t>
concept NonNarrowingTo = requires(from_t&& from) { to_t{std::forward<from_t>(from)}; };

}  // namespace schemacht::util
