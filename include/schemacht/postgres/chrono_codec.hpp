#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ratio>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "schemacht/postgres/binary_form.hpp"
#include "schemacht/postgres/concepts/chrono_codec.hpp"  // IWYU pragma: export

// field_codec.hpp includes this at its end, so these codecs are seen wherever `FieldCodec` is.

/**
 * @file
 * Dates and times as column types, written and read as PostgreSQL's own:
 *
 * | C++                            | PostgreSQL    |
 * |--------------------------------|---------------|
 * | `std::chrono::sys_time<D>`     | `timestamptz` |
 * | `std::chrono::local_time<D>`   | `timestamp`   |
 * | `std::chrono::year_month_day`  | `date`        |
 *
 * PostgreSQL keeps a time to the microsecond, so `D` is a microsecond or a whole fraction of one (`MicrosecondExact`):
 * `std::chrono::system_clock::time_point` will do. A time read back is exact; one written is truncated to the microsecond, toward
 * the past. PostgreSQL's `infinity` and `-infinity` are the `time_point`'s `max()` and `min()`, and, for a date,
 * `year::max()/December/31` and `year::min()/January/1`.
 *
 * Text results are read in the ISO form, which `SessionSettings::iso_dates` (on by default) asks the server for.
 */
namespace schemacht::postgres {

template <typename data_t>
class FieldCodec;  // defined in field_codec.hpp; specialized below

namespace detail {

/// A time as these functions pass it: microseconds since 1970-01-01 00:00 (UTC for `timestamptz`), or one of these for infinity.
inline constexpr std::int64_t INFINITE_FUTURE = std::numeric_limits<std::int64_t>::max();
inline constexpr std::int64_t INFINITE_PAST = std::numeric_limits<std::int64_t>::min();

/** @return A time's text form: `2026-10-01 20:55:12.264000+00` with `zoned`, else without the `+00`; `infinity`; ` BC` when it is. */
[[nodiscard]] std::string format_timestamp(std::int64_t micros, bool zoned);

/**
 * @return The time in a text form as PostgreSQL prints it (ISO); with `zoned`, its offset is required and taken off.
 * @throws std::invalid_argument for text that is not a time.
 */
[[nodiscard]] std::int64_t parse_timestamp(std::string_view text, bool zoned);

/**
 * @return The time in PostgreSQL's binary form: microseconds since 2000-01-01.
 * @throws std::invalid_argument for the wrong size.
 */
[[nodiscard]] std::int64_t parse_binary_timestamp(std::span<const std::byte> bytes);

/** @return A date's text form, `2026-10-01`, from days since 1970-01-01 (or an infinite one). */
[[nodiscard]] std::string format_date(std::int64_t days);

/**
 * @return The days since 1970-01-01 of a date in its ISO text form, or an infinite one.
 * @throws std::invalid_argument for text that is not a date.
 */
[[nodiscard]] std::int64_t parse_date(std::string_view text);

/**
 * @return The days since 1970-01-01 of a date in PostgreSQL's binary form (days since 2000-01-01), or an infinite one.
 * @throws std::invalid_argument for the wrong size.
 */
[[nodiscard]] std::int64_t parse_binary_date(std::span<const std::byte> bytes);

}  // namespace detail

namespace detail {

/** @return `time` in microseconds since 1970-01-01, truncated toward the past; `max()` and `min()` are infinite. */
template <typename clock_t, MicrosecondExact duration_t>
[[nodiscard]] std::int64_t micros_of(std::chrono::time_point<clock_t, duration_t> time) {
  using point_t = std::chrono::time_point<clock_t, duration_t>;
  if ( time == point_t::max() ) {
    return INFINITE_FUTURE;
  }

  if ( time == point_t::min() ) {
    return INFINITE_PAST;
  }

  return std::chrono::floor<std::chrono::microseconds>(time.time_since_epoch()).count();
}

/** @return The time `micros` after 1970-01-01. @throws std::invalid_argument if it does not fit `duration_t`. */
template <typename clock_t, MicrosecondExact duration_t>
[[nodiscard]] std::chrono::time_point<clock_t, duration_t> time_of(std::int64_t micros) {
  using point_t = std::chrono::time_point<clock_t, duration_t>;
  using rep_t = duration_t::rep;

  if ( micros == INFINITE_FUTURE ) {
    return point_t::max();
  }

  if ( micros == INFINITE_PAST ) {
    return point_t::min();
  }

  constexpr auto TICKS_PER_MICRO = static_cast<rep_t>(std::ratio_divide<std::micro, typename duration_t::period>::num);
  if ( micros > std::numeric_limits<rep_t>::max() / TICKS_PER_MICRO || micros < std::numeric_limits<rep_t>::min() / TICKS_PER_MICRO ) {
    throw std::invalid_argument("the time is outside what its C++ type holds");
  }

  return point_t(duration_t(static_cast<rep_t>(micros) * TICKS_PER_MICRO));
}

/** @return `date` in days since 1970-01-01; the infinite dates are infinite. @throws std::invalid_argument for a date that does not exist. */
[[nodiscard]] std::int64_t days_of(const std::chrono::year_month_day& date);

/** @return The date `days` after 1970-01-01. @throws std::invalid_argument if it is outside `std::chrono::year`'s range. */
[[nodiscard]] std::chrono::year_month_day date_of(std::int64_t days);

}  // namespace detail

/** @brief A `std::chrono::sys_time` is a `timestamptz`: a moment, in UTC. */
template <MicrosecondExact duration_t>
class FieldCodec<std::chrono::sys_time<duration_t>> {
  using time_type = std::chrono::sys_time<duration_t>;

 public:
  [[nodiscard]] static time_type parse(std::string_view text) {
    return detail::time_of<std::chrono::system_clock, duration_t>(detail::parse_timestamp(text, true));
  }
  [[nodiscard]] static std::string format(const time_type& time) { return detail::format_timestamp(detail::micros_of(time), true); }

  [[nodiscard]] static time_type parse_binary(std::span<const std::byte> bytes) {
    return detail::time_of<std::chrono::system_clock, duration_t>(detail::parse_binary_timestamp(bytes));
  }

  /** @brief Only `timestamptz`: a `timestamp` has no zone, and reading it as UTC would be a guess. */
  [[nodiscard]] static constexpr bool binary_accepts(std::uint32_t type_oid) { return type_oid == OID_TIMESTAMPTZ; }
};

/** @brief A `std::chrono::local_time` is a `timestamp`: a date and a time of day, in no particular zone. */
template <MicrosecondExact duration_t>
class FieldCodec<std::chrono::local_time<duration_t>> {
  using time_type = std::chrono::local_time<duration_t>;

 public:
  [[nodiscard]] static time_type parse(std::string_view text) {
    return detail::time_of<std::chrono::local_t, duration_t>(detail::parse_timestamp(text, false));
  }
  [[nodiscard]] static std::string format(const time_type& time) { return detail::format_timestamp(detail::micros_of(time), false); }

  [[nodiscard]] static time_type parse_binary(std::span<const std::byte> bytes) {
    return detail::time_of<std::chrono::local_t, duration_t>(detail::parse_binary_timestamp(bytes));
  }

  /** @brief Only `timestamp`: a `timestamptz` is a moment, which has a date and time of day only in some zone. */
  [[nodiscard]] static constexpr bool binary_accepts(std::uint32_t type_oid) { return type_oid == OID_TIMESTAMP; }
};

/** @brief A `std::chrono::year_month_day` is a `date`. */
template <>
class FieldCodec<std::chrono::year_month_day> {
 public:
  [[nodiscard]] static std::chrono::year_month_day parse(std::string_view text) { return detail::date_of(detail::parse_date(text)); }
  [[nodiscard]] static std::string format(const std::chrono::year_month_day& date) { return detail::format_date(detail::days_of(date)); }

  [[nodiscard]] static std::chrono::year_month_day parse_binary(std::span<const std::byte> bytes) {
    return detail::date_of(detail::parse_binary_date(bytes));
  }

  [[nodiscard]] static constexpr bool binary_accepts(std::uint32_t type_oid) { return type_oid == OID_DATE; }
};

}  // namespace schemacht::postgres
