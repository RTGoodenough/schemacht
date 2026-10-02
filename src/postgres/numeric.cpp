#include "schemacht/postgres/numeric.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "schemacht/postgres/binary_form.hpp"

namespace schemacht::postgres::detail {

namespace {

constexpr std::int64_t DECIMAL_BASE = 10;
constexpr std::int64_t DIGITS_PER_NUMERIC_DIGIT = 4;  // a `numeric` digit is base 10000
constexpr std::int64_t NUMERIC_DIGIT_LIMIT = 10'000;

// The sign word of a `numeric` in binary form.
constexpr std::uint16_t NUMERIC_POSITIVE = 0x0000;
constexpr std::uint16_t NUMERIC_NEGATIVE = 0x4000;
constexpr std::uint16_t NUMERIC_NAN = 0xC000;
constexpr std::uint16_t NUMERIC_POSITIVE_INFINITY = 0xD000;
constexpr std::uint16_t NUMERIC_NEGATIVE_INFINITY = 0xF000;

constexpr std::array<std::int64_t, MAX_NUMERIC_SCALE + 1> POWERS_OF_TEN = [] {
  std::array<std::int64_t, MAX_NUMERIC_SCALE + 1> powers{};
  powers.at(0) = 1;
  for ( std::size_t i = 1; i < powers.size(); ++i ) {
    powers.at(i) = powers.at(i - 1) * DECIMAL_BASE;
  }
  return powers;
}();

[[noreturn]] void too_large() { throw std::invalid_argument("the numeric is too large for a 64-bit count of its units"); }

std::int64_t checked_add(std::int64_t left, std::int64_t right) {
  if ( right > std::numeric_limits<std::int64_t>::max() - left ) {  // both are never negative here
    too_large();
  }
  return left + right;
}

std::int64_t checked_multiply(std::int64_t left, std::int64_t right) {
  if ( right != 0 && left > std::numeric_limits<std::int64_t>::max() / right ) {
    too_large();
  }
  return left * right;
}

/** @return `units` with one more decimal digit, `digit`, on its right. */
std::int64_t append_digit(std::int64_t units, char digit) { return checked_add(checked_multiply(units, DECIMAL_BASE), digit - '0'); }

bool all_digits(std::string_view text) {
  return std::ranges::all_of(text, [](char chr) { return chr >= '0' && chr <= '9'; });
}

}  // namespace

std::int64_t parse_scaled_numeric(std::string_view text, unsigned scale) {
  const std::string_view whole = text;
  if ( text == "NaN" || text == "Infinity" || text == "-Infinity" ) {
    throw std::invalid_argument("parse_field: the numeric " + std::string(whole) + " is not an amount");
  }
  const bool negative = text.starts_with('-');
  if ( negative ) {
    text.remove_prefix(1);
  }
  const std::size_t      point = text.find('.');
  const std::string_view integer = text.substr(0, point);
  const std::string_view fraction = point == std::string_view::npos ? std::string_view() : text.substr(point + 1);
  if ( integer.empty() || ! all_digits(integer) || ! all_digits(fraction) || (point != std::string_view::npos && fraction.empty()) ) {
    throw std::invalid_argument("parse_field: '" + std::string(whole) + "' is not a numeric");
  }
  std::int64_t units = 0;
  for ( const char digit : integer ) {
    units = append_digit(units, digit);
  }
  for ( std::size_t place = 0; place < scale; ++place ) {
    units = append_digit(units, place < fraction.size() ? fraction.at(place) : '0');
  }
  if ( fraction.size() > scale && ! std::ranges::all_of(fraction.substr(scale), [](char chr) { return chr == '0'; }) ) {
    throw std::invalid_argument("parse_field: '" + std::string(whole) + "' has more than " + std::to_string(scale) + " decimal places");
  }
  return negative ? -units : units;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- reached only through the public template, which takes the scale as a template argument
std::string format_scaled_numeric(std::int64_t units, unsigned scale) {
  const bool          negative = units < 0;
  const std::uint64_t magnitude = negative ? 0U - static_cast<std::uint64_t>(units) : static_cast<std::uint64_t>(units);
  const auto          divisor = static_cast<std::uint64_t>(POWERS_OF_TEN.at(scale));
  std::string         text = negative ? "-" : "";
  text += std::to_string(magnitude / divisor);
  if ( scale > 0 ) {
    const std::string fraction = std::to_string(magnitude % divisor);
    text += '.';
    text.append(scale - fraction.size(), '0');
    text += fraction;
  }
  return text;
}

std::int64_t parse_binary_scaled_numeric(std::span<const std::byte> bytes, unsigned scale) {
  BinaryReader                        reader(bytes);
  const std::int16_t                  count = reader.int16();
  const std::int16_t                  weight = reader.int16();  // the power of 10000 of the first digit
  const std::uint16_t                 sign = reader.uint16();
  [[maybe_unused]] const std::int16_t display_scale = reader.int16();
  if ( sign == NUMERIC_NAN || sign == NUMERIC_POSITIVE_INFINITY || sign == NUMERIC_NEGATIVE_INFINITY ) {
    throw std::invalid_argument("parse_binary: a NaN or infinite numeric is not an amount");
  }
  if ( (sign != NUMERIC_POSITIVE && sign != NUMERIC_NEGATIVE) || count < 0 ) {
    throw std::invalid_argument("parse_binary: not a numeric");
  }
  std::int64_t units = 0;
  for ( std::int16_t index = 0; index < count; ++index ) {
    const std::int64_t digit = reader.int16();
    if ( digit < 0 || digit >= NUMERIC_DIGIT_LIMIT ) {
      throw std::invalid_argument("parse_binary: not a numeric");
    }
    if ( digit == 0 ) {
      continue;
    }
    // The digit is worth 10000^(weight - index), which in units of 10^-scale is 10^exponent.
    const std::int64_t exponent = (DIGITS_PER_NUMERIC_DIGIT * (weight - index)) + static_cast<std::int64_t>(scale);
    if ( std::cmp_greater(exponent, MAX_NUMERIC_SCALE) ) {
      too_large();
    }
    if ( exponent >= 0 ) {
      units = checked_add(units, checked_multiply(digit, POWERS_OF_TEN.at(static_cast<std::size_t>(exponent))));
    } else if ( -exponent < DIGITS_PER_NUMERIC_DIGIT && digit % POWERS_OF_TEN.at(static_cast<std::size_t>(-exponent)) == 0 ) {
      units = checked_add(units, digit / POWERS_OF_TEN.at(static_cast<std::size_t>(-exponent)));  // the digits past `scale` places are zeros
    } else {
      throw std::invalid_argument("parse_binary: the numeric has more than " + std::to_string(scale) + " decimal places");
    }
  }
  reader.expect_end();
  return sign == NUMERIC_NEGATIVE ? -units : units;
}

}  // namespace schemacht::postgres::detail
