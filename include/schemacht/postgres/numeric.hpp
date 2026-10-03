#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "schemacht/postgres/concepts/numeric.hpp"  // IWYU pragma: export

/**
 * Exact decimal amounts as PostgreSQL's `numeric`, for a column type of your own that holds one as a whole number of its
 * smallest unit: cents are a scale of 2, so `19.99` is 1999. The `Money` of examples/custom_types.cpp is written with these.
 *
 * A value is refused, with `std::invalid_argument`, rather than rounded: one with a nonzero digit past `scale` decimal places
 * (a `total / 3`, say), `NaN` or an infinity, or one too large for `std::int64_t` units.
 */
namespace schemacht::postgres {

namespace detail {

[[nodiscard]] std::int64_t parse_scaled_numeric(std::string_view text, unsigned scale);
[[nodiscard]] std::string  format_scaled_numeric(std::int64_t units, unsigned scale);
[[nodiscard]] std::int64_t parse_binary_scaled_numeric(std::span<const std::byte> bytes, unsigned scale);

}  // namespace detail

/**
 * @return A `numeric` in its text form (`19.99`, `-0.05`, `7`, `1.50000`) as a count of 10^-`scale` units:
 * `parse_scaled_numeric<2>("19.99")` is 1999.
 * @throws std::invalid_argument for anything else, `NaN` and infinities included, or a value it cannot hold exactly.
 */
template <unsigned scale>
  requires NumericScale<scale>
[[nodiscard]] std::int64_t parse_scaled_numeric(std::string_view text) {
  return detail::parse_scaled_numeric(text, scale);
}

/** @return `units` 10^-`scale` units in a `numeric`'s text form, with exactly `scale` decimal places: `format_scaled_numeric<2>(-5)` is `-0.05`. */
template <unsigned scale>
  requires NumericScale<scale>
[[nodiscard]] std::string format_scaled_numeric(std::int64_t units) {
  return detail::format_scaled_numeric(units, scale);
}

/**
 * @return A `numeric` in its binary form (base-10000 digits after a header of count, weight, sign and display scale) as a count
 * of 10^-`scale` units.
 * @throws std::invalid_argument as `parse_scaled_numeric`, or for bytes that are not a `numeric`.
 */
template <unsigned scale>
  requires NumericScale<scale>
[[nodiscard]] std::int64_t parse_binary_scaled_numeric(std::span<const std::byte> bytes) {
  return detail::parse_binary_scaled_numeric(bytes, scale);
}

}  // namespace schemacht::postgres
