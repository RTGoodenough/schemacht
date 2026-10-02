#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "schemacht/postgres/field_codec.hpp"

namespace pg = schemacht::postgres;

namespace {

std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
  std::vector<std::byte> result;
  for ( const unsigned value : values ) {
    result.push_back(static_cast<std::byte>(value));
  }
  return result;
}

}  // namespace

TEST_CASE("which types have a text form", "[codec]") {
  struct Opaque {};
  STATIC_REQUIRE(pg::HasTextForm<bool>);
  STATIC_REQUIRE(pg::HasTextForm<std::int16_t>);
  STATIC_REQUIRE(pg::HasTextForm<double>);
  STATIC_REQUIRE(pg::HasTextForm<std::string>);
  STATIC_REQUIRE(pg::HasTextForm<std::vector<std::byte>>);    // bytea
  STATIC_REQUIRE(pg::HasTextForm<std::vector<std::string>>);  // an array
  STATIC_REQUIRE_FALSE(pg::HasTextForm<Opaque>);
  STATIC_REQUIRE_FALSE(pg::HasTextForm<std::vector<Opaque>>);
}

TEST_CASE("integers parse only when the whole text is a number", "[codec]") {
  CHECK(pg::FieldCodec<std::int64_t>::parse("42") == 42);
  CHECK(pg::FieldCodec<std::int64_t>::parse("-9223372036854775808") == std::numeric_limits<std::int64_t>::min());
  CHECK_THROWS_AS(pg::FieldCodec<std::int64_t>::parse("12abc"), std::invalid_argument);
  CHECK_THROWS_AS(pg::FieldCodec<std::int64_t>::parse(""), std::invalid_argument);
  CHECK_THROWS_AS(pg::FieldCodec<std::int16_t>::parse("40000"), std::invalid_argument);
}

TEST_CASE("booleans parse only from PostgreSQL's spellings", "[codec]") {
  CHECK(pg::FieldCodec<bool>::parse("t"));
  CHECK(pg::FieldCodec<bool>::parse("true"));
  CHECK_FALSE(pg::FieldCodec<bool>::parse("f"));
  CHECK_FALSE(pg::FieldCodec<bool>::parse("false"));
  CHECK_THROWS_AS(pg::FieldCodec<bool>::parse("garbage"), std::invalid_argument);
  CHECK(pg::FieldCodec<bool>::format(true) == "t");
}

TEST_CASE("floating point includes PostgreSQL's special values", "[codec]") {
  CHECK(pg::FieldCodec<double>::parse("1.5") == 1.5);
  CHECK(std::isnan(pg::FieldCodec<double>::parse("NaN")));
  CHECK(pg::FieldCodec<double>::parse("Infinity") == std::numeric_limits<double>::infinity());
  CHECK(pg::FieldCodec<double>::parse("-Infinity") == -std::numeric_limits<double>::infinity());
  CHECK(pg::FieldCodec<float>::parse(pg::FieldCodec<float>::format(0.1F)) == 0.1F);
  CHECK_THROWS_AS(pg::FieldCodec<double>::parse("1.5x"), std::invalid_argument);
}

TEST_CASE("bytea is written in hex with its \\x prefix", "[codec]") {
  const std::vector<std::byte> data = bytes({0x01, 0xff, 0x00});
  CHECK(pg::FieldCodec<std::vector<std::byte>>::format(data) == "\\x01ff00");
  CHECK(pg::FieldCodec<std::vector<std::byte>>::parse("\\x01ff00") == data);
  CHECK(pg::FieldCodec<std::vector<std::byte>>::parse("\\x01FF00") == data);
  CHECK_THROWS_AS(pg::FieldCodec<std::vector<std::byte>>::parse("\\x0g"), std::invalid_argument);
  CHECK_THROWS_AS(pg::FieldCodec<std::vector<std::byte>>::parse("\\x012"), std::invalid_argument);
}

TEST_CASE("arrays round-trip through their text form", "[codec]") {
  const std::vector<std::string> words{"plain", "with \"quote\"", "back\\slash", "", "comma, and space"};
  const std::string              text = pg::FieldCodec<std::vector<std::string>>::format(words);
  CHECK(text == R"({"plain","with \"quote\"","back\\slash","","comma, and space"})");
  CHECK(pg::FieldCodec<std::vector<std::string>>::parse(text) == words);

  CHECK(pg::FieldCodec<std::vector<std::int64_t>>::parse("{1, 2,3}") == std::vector<std::int64_t>{1, 2, 3});
  CHECK(pg::FieldCodec<std::vector<std::int64_t>>::parse("{}").empty());

  // A bytea element keeps its \x through the array's escaping.
  CHECK(pg::format_array(std::vector<std::vector<std::byte>>{bytes({0xab})}) == R"({"\\xab"})");
}

TEST_CASE("arrays a std::vector cannot hold are refused", "[codec]") {
  CHECK_THROWS_AS(pg::FieldCodec<std::vector<std::int64_t>>::parse("{1,NULL}"), std::invalid_argument);
  CHECK_THROWS_AS(pg::FieldCodec<std::vector<std::int64_t>>::parse("{{1},{2}}"), std::invalid_argument);
  CHECK_THROWS_AS(pg::FieldCodec<std::vector<std::int64_t>>::parse("[0:1]={1,2}"), std::invalid_argument);
  CHECK_THROWS_AS(pg::FieldCodec<std::vector<std::int64_t>>::parse("1,2"), std::invalid_argument);
  CHECK_THROWS_AS(pg::FieldCodec<std::vector<std::int64_t>>::parse("{1,}"), std::invalid_argument);
}

TEST_CASE("binary integers are read by their length and range-checked", "[codec][binary]") {
  CHECK(pg::FieldCodec<std::int64_t>::parse_binary(bytes({0xff, 0xfe})) == -2);
  CHECK(pg::FieldCodec<std::int32_t>::parse_binary(bytes({0x00, 0x00, 0x01, 0x00})) == 256);
  CHECK(pg::FieldCodec<std::int64_t>::parse_binary(bytes({0, 0, 0, 0, 0, 0, 0, 7})) == 7);
  CHECK_THROWS_AS(pg::FieldCodec<std::int16_t>::parse_binary(bytes({0, 0, 0x80, 0})), std::invalid_argument);  // 32768
  CHECK_THROWS_AS(pg::FieldCodec<std::uint32_t>::parse_binary(bytes({0xff, 0xff})), std::invalid_argument);    // -1
  CHECK_THROWS_AS(pg::FieldCodec<std::int64_t>::parse_binary(bytes({1, 2, 3})), std::invalid_argument);
  CHECK(pg::FieldCodec<bool>::parse_binary(bytes({1})));
  CHECK(pg::FieldCodec<std::int64_t>::binary_accepts(pg::detail::OID_INT4));
  CHECK_FALSE(pg::FieldCodec<std::int64_t>::binary_accepts(pg::detail::OID_TEXT));
}

TEST_CASE("binary arrays", "[codec][binary]") {
  // One dimension, no NULLs, int4 elements: 3 then -1.
  const std::vector<std::byte> array = bytes({0, 0, 0, 1, 0,    0,    0,    0,   0, 0, 0, 23,  // dimensions, has-NULL, element OID
                                              0, 0, 0, 2, 0,    0,    0,    1,                 // length 2, lower bound 1
                                              0, 0, 0, 4, 0,    0,    0,    3,                 // 4 bytes: 3
                                              0, 0, 0, 4, 0xff, 0xff, 0xff, 0xff});            // 4 bytes: -1
  CHECK(pg::FieldCodec<std::vector<std::int64_t>>::parse_binary(array) == std::vector<std::int64_t>{3, -1});

  std::vector<std::byte> with_null = array;
  with_null.resize(28);
  for ( const unsigned value : {0xffU, 0xffU, 0xffU, 0xffU} ) {
    with_null.push_back(static_cast<std::byte>(value));  // length -1: NULL
  }
  CHECK_THROWS_AS(pg::FieldCodec<std::vector<std::int64_t>>::parse_binary(with_null), std::invalid_argument);

  std::vector<std::byte> truncated = array;
  truncated.resize(truncated.size() - 1);
  CHECK_THROWS_AS(pg::FieldCodec<std::vector<std::int64_t>>::parse_binary(truncated), std::invalid_argument);

  const std::vector<std::byte> empty = bytes({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 23});
  CHECK(pg::FieldCodec<std::vector<std::int64_t>>::parse_binary(empty).empty());
}
