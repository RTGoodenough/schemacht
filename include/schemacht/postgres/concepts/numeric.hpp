#pragma once

namespace schemacht::postgres {

/** @brief The most decimal places a scaled value can have: one unit of 10^-18 still leaves an `std::int64_t` room for 9. */
inline constexpr unsigned MAX_NUMERIC_SCALE = 18;

/** @brief Whether `scale` decimal places fit an `std::int64_t` count of units. */
template <unsigned scale>
concept NumericScale = scale <= MAX_NUMERIC_SCALE;

}  // namespace schemacht::postgres
