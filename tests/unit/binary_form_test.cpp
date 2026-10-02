#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <vector>

#include "schemacht/postgres/binary_form.hpp"
#include "schemacht/postgres/numeric.hpp"

namespace pg = schemacht::postgres;

namespace {

/** @return `words` as big-endian 16-bit words, as a `numeric`'s binary form is laid out. */
std::vector<std::byte> words(std::initializer_list<int> values) {
  std::vector<std::byte> bytes;
  for ( const int value : values ) {
    const auto word = static_cast<std::uint16_t>(value);
    bytes.push_back(static_cast<std::byte>(word >> 8U));
    bytes.push_back(static_cast<std::byte>(word & 0xFFU));
  }
  return bytes;
}

// A `numeric` in binary form: digit count, weight, sign, display scale, then the base-10000 digits.
constexpr int POSITIVE = 0x0000;
constexpr int NEGATIVE = 0x4000;
constexpr int NOT_A_NUMBER = 0xC000;

}  // namespace

TEST_CASE("BinaryReader reads big-endian fields in turn", "[binary]") {
  const std::vector<std::byte> bytes = words({1, -2, 0, 7, 0xABCD});
  pg::BinaryReader             reader(bytes);

  CHECK(reader.int16() == 1);
  CHECK(reader.int16() == -2);
  CHECK(reader.int32() == 7);
  CHECK(reader.remaining() == 2);
  CHECK_THROWS_AS(reader.expect_end(), std::invalid_argument);
  CHECK(reader.uint16() == 0xABCD);
  reader.expect_end();
  CHECK_THROWS_AS(reader.int16(), std::invalid_argument);  // nothing left: refused, not read past

  CHECK(pg::binary_integer(words({-3})) == -3);
  CHECK(pg::binary_real(words({0x3FF8, 0, 0, 0})) == 1.5);
}

TEST_CASE("a numeric's text form as a count of units", "[numeric]") {
  CHECK(pg::parse_scaled_numeric<2>("19.99") == 1999);
  CHECK(pg::parse_scaled_numeric<2>("-0.05") == -5);
  CHECK(pg::parse_scaled_numeric<2>("7") == 700);
  CHECK(pg::parse_scaled_numeric<2>("1.5") == 150);
  CHECK(pg::parse_scaled_numeric<2>("1.50000") == 150);  // zeros past the scale lose nothing
  CHECK(pg::parse_scaled_numeric<0>("42") == 42);

  CHECK_THROWS_AS(pg::parse_scaled_numeric<2>("0.001"), std::invalid_argument);
  CHECK_THROWS_AS(pg::parse_scaled_numeric<2>("NaN"), std::invalid_argument);
  CHECK_THROWS_AS(pg::parse_scaled_numeric<2>("Infinity"), std::invalid_argument);
  CHECK_THROWS_AS(pg::parse_scaled_numeric<2>("1."), std::invalid_argument);
  CHECK_THROWS_AS(pg::parse_scaled_numeric<2>("1e3"), std::invalid_argument);
  CHECK_THROWS_AS(pg::parse_scaled_numeric<2>("99999999999999999999"), std::invalid_argument);  // beyond int64 cents

  CHECK(pg::format_scaled_numeric<2>(1999) == "19.99");
  CHECK(pg::format_scaled_numeric<2>(-5) == "-0.05");
  CHECK(pg::format_scaled_numeric<2>(0) == "0.00");
  CHECK(pg::format_scaled_numeric<0>(-42) == "-42");
  CHECK(pg::format_scaled_numeric<2>(std::numeric_limits<std::int64_t>::min()) == "-92233720368547758.08");

  STATIC_REQUIRE(pg::NumericScale<18>);
  STATIC_REQUIRE_FALSE(pg::NumericScale<19>);
}

TEST_CASE("a numeric's binary form as a count of units", "[numeric][binary]") {
  CHECK(pg::parse_binary_scaled_numeric<2>(words({2, 0, POSITIVE, 2, 19, 9900})) == 1999);      // 19.99
  CHECK(pg::parse_binary_scaled_numeric<2>(words({1, 0, NEGATIVE, 2, 5})) == -500);             // -5.00
  CHECK(pg::parse_binary_scaled_numeric<2>(words({1, -1, POSITIVE, 2, 500})) == 5);             // 0.05
  CHECK(pg::parse_binary_scaled_numeric<2>(words({0, 0, POSITIVE, 2})) == 0);                   // 0.00
  CHECK(pg::parse_binary_scaled_numeric<2>(words({2, 0, POSITIVE, 2, 1200, 5000})) == 120050);  // 1200.50
  CHECK(pg::parse_binary_scaled_numeric<2>(words({1, 1, POSITIVE, 0, 1})) == 1000000);          // 10000
  CHECK(pg::parse_binary_scaled_numeric<0>(words({1, 1, POSITIVE, 0, 1})) == 10000);

  CHECK_THROWS_AS(pg::parse_binary_scaled_numeric<2>(words({1, -1, POSITIVE, 3, 10})), std::invalid_argument);  // 0.001
  CHECK_THROWS_AS(pg::parse_binary_scaled_numeric<2>(words({0, 0, NOT_A_NUMBER, 0})), std::invalid_argument);
  CHECK_THROWS_AS(pg::parse_binary_scaled_numeric<2>(words({1, 5, POSITIVE, 0, 1})), std::invalid_argument);      // 10^20: too large
  CHECK_THROWS_AS(pg::parse_binary_scaled_numeric<2>(words({2, 0, POSITIVE, 2, 19})), std::invalid_argument);     // a digit short
  CHECK_THROWS_AS(pg::parse_binary_scaled_numeric<2>(words({1, 0, POSITIVE, 2, 19, 1})), std::invalid_argument);  // a word over
}
