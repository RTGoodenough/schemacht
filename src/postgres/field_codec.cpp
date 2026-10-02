#include "schemacht/postgres/field_codec.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace schemacht::postgres::detail {

namespace {

constexpr unsigned int BITS_PER_HEX_DIGIT = 4;
constexpr unsigned int HEX_NIBBLE_MASK = 0xFU;
constexpr unsigned int DECIMAL_DIGIT_COUNT = 10;
constexpr std::size_t  CHAR_VALUES = 256;
constexpr std::size_t  MESSAGE_TEXT_LIMIT = 80;  // how much of an array's text an error message quotes

/** @brief What a table entry holds for a character that is not a hex digit: no nibble is above `HEX_NIBBLE_MASK`. */
constexpr std::uint8_t NOT_A_DIGIT = 0xFF;

/** @return The value of every hex digit at its character's index, `NOT_A_DIGIT` elsewhere. */
constexpr std::array<std::uint8_t, CHAR_VALUES> make_nibble_table() {
  std::array<std::uint8_t, CHAR_VALUES> table{};
  table.fill(NOT_A_DIGIT);
  for ( unsigned int digit = 0; digit < DECIMAL_DIGIT_COUNT; ++digit ) {
    table.at('0' + digit) = static_cast<std::uint8_t>(digit);
  }
  for ( unsigned int digit = 0; digit <= HEX_NIBBLE_MASK - DECIMAL_DIGIT_COUNT; ++digit ) {
    table.at('a' + digit) = static_cast<std::uint8_t>(DECIMAL_DIGIT_COUNT + digit);
    table.at('A' + digit) = static_cast<std::uint8_t>(DECIMAL_DIGIT_COUNT + digit);
  }
  return table;
}

// A table, not a chain of range checks: hex digits are effectively random, so a branch per digit is often mispredicted,
// and a table lookup decodes large values several times faster.
constexpr std::array<std::uint8_t, CHAR_VALUES> NIBBLES = make_nibble_table();

/** @brief The characters that end an unquoted array element, or cannot be inside one: `,` `"` `{` `}`. */
constexpr std::array<bool, CHAR_VALUES> make_bare_element_ends() {
  std::array<bool, CHAR_VALUES> ends{};
  for ( const char chr : {',', '"', '{', '}'} ) {
    ends.at(static_cast<unsigned char>(chr)) = true;
  }
  return ends;
}

constexpr std::array<bool, CHAR_VALUES> ENDS_BARE_ELEMENT = make_bare_element_ends();

constexpr bool is_space(char chr) { return chr == ' ' || chr == '\t' || chr == '\n' || chr == '\r' || chr == '\v' || chr == '\f'; }

std::string_view trimmed(std::string_view text) {
  while ( ! text.empty() && is_space(text.front()) ) {
    text.remove_prefix(1);
  }
  while ( ! text.empty() && is_space(text.back()) ) {
    text.remove_suffix(1);
  }
  return text;
}

bool is_null_word(std::string_view token) {
  constexpr std::string_view NULL_WORD = "null";
  return token.size() == NULL_WORD.size() &&
         std::ranges::equal(token, NULL_WORD, [](char left, char right) { return left == right || left == static_cast<char>(right - 'a' + 'A'); });
}

}  // namespace

std::vector<std::byte> parse_hex_bytes(std::string_view text) {
  if ( text.size() % 2 != 0 ) {
    throw std::invalid_argument("field_codec: hex byte string must have even length");
  }
  std::vector<std::byte> bytes(text.size() / 2);
  unsigned int           seen = 0;  // every nibble read, or-ed: a non-digit (NOT_A_DIGIT) leaves a bit above the nibble
  for ( std::size_t i = 0; i < bytes.size(); ++i ) {
    const unsigned int high = NIBBLES.at(static_cast<unsigned char>(text.at(2 * i)));
    const unsigned int low = NIBBLES.at(static_cast<unsigned char>(text.at((2 * i) + 1)));
    seen |= high | low;
    bytes.at(i) = static_cast<std::byte>((high << BITS_PER_HEX_DIGIT) | low);
  }
  if ( seen > HEX_NIBBLE_MASK ) {
    throw std::invalid_argument("field_codec: invalid hex digit");
  }
  return bytes;
}

std::string format_hex_bytes(const std::vector<std::byte>& bytes) {
  static constexpr std::string_view DIGITS = "0123456789abcdef";
  std::string                       text;
  text.reserve(bytes.size() * 2);
  for ( const std::byte byte : bytes ) {
    const auto value = static_cast<unsigned int>(byte);
    text.push_back(DIGITS.at(value >> BITS_PER_HEX_DIGIT));
    text.push_back(DIGITS.at(value & HEX_NIBBLE_MASK));
  }
  return text;
}

void ArrayReader::fail(std::string_view reason) const {
  std::string text(_text.substr(0, MESSAGE_TEXT_LIMIT));
  if ( _text.size() > MESSAGE_TEXT_LIMIT ) {
    text += "...";
  }
  throw std::invalid_argument("parse_field: the array " + std::string(reason) + ": '" + text + "'");
}

ArrayReader::ArrayReader(std::string_view text) : _text(text) {
  const std::string_view whole = trimmed(text);
  if ( ! whole.empty() && whole.front() == '[' ) {
    fail("has a dimension prefix ([lower:upper]=), which is not supported");
  }
  if ( whole.empty() || whole.front() != '{' ) {
    fail("does not start with '{'");
  }
  if ( whole.back() != '}' ) {
    fail("does not end with '}'");
  }
  _body = trimmed(whole.substr(1, whole.size() - 2));
  if ( ! _body.empty() ) {
    _estimate = static_cast<std::size_t>(std::ranges::count(_body, ',')) + 1;
  }
}

void ArrayReader::skip_spaces() {
  while ( _at < _body.size() && is_space(_body.at(_at)) ) {
    ++_at;
  }
}

std::string_view ArrayReader::bare_element() {
  // One pass to the comma that ends the element; a quote or a brace on the way is not allowed in an unquoted element.
  const std::string_view rest = _body.substr(_at);
  const auto* const      stop = std::ranges::find_if(rest, [](char chr) { return ENDS_BARE_ELEMENT.at(static_cast<unsigned char>(chr)); });
  const auto             length = static_cast<std::size_t>(stop - rest.begin());
  std::string_view       token = rest.substr(0, length);
  _at += length;  // at the comma, if there is one: `next` looks at it
  if ( stop != rest.end() && *stop != ',' ) {
    fail(*stop == '"' ? "has a quote inside an unquoted element" : "is nested or has a brace inside an unquoted element");
  }
  if ( ! token.empty() && (is_space(token.front()) || is_space(token.back())) ) {
    token = trimmed(token);
  }
  if ( token.empty() ) {
    fail("has an empty element (an empty string is written \"\")");
  }
  if ( is_null_word(token) ) {
    fail("has a NULL element, which a std::vector cannot hold");
  }
  return token;
}

std::string_view ArrayReader::quoted_element() {
  const std::size_t start = _at + 1;  // past the opening quote
  std::size_t       end = start;
  bool              escaped = false;
  while ( end < _body.size() && _body.at(end) != '"' ) {
    if ( _body.at(end) == '\\' ) {
      escaped = true;
      ++end;  // the character after a backslash is data, even a quote
    }
    ++end;
  }
  if ( end >= _body.size() ) {
    fail("has a string that is never closed");
  }
  const std::string_view raw = _body.substr(start, end - start);
  _at = end + 1;
  if ( ! escaped ) {
    return raw;
  }
  _unescaped.clear();
  for ( std::size_t i = 0; i < raw.size(); ++i ) {
    if ( raw.at(i) == '\\' && i + 1 < raw.size() ) {
      ++i;
    }
    _unescaped += raw.at(i);
  }
  return _unescaped;
}

std::optional<std::string_view> ArrayReader::next() {
  skip_spaces();
  if ( _at >= _body.size() ) {
    if ( _after_comma ) {
      fail("has a comma with no element after it");
    }
    return std::nullopt;
  }
  _after_comma = false;
  const std::string_view element = _body.at(_at) == '"' ? quoted_element() : bare_element();
  skip_spaces();
  if ( _at < _body.size() ) {
    if ( _body.at(_at) != ',' ) {
      fail("has text after a string that is not a comma");
    }
    ++_at;
    _after_comma = true;
  }
  return element;
}

// ---- PostgreSQL's binary form ---------------------------------------------------------------------------------------------

namespace {

constexpr std::size_t BINARY_LENGTH_BYTES = 4;     // each array element starts with its length
constexpr std::size_t BINARY_ARRAY_HEADER = 12;    // dimension count, has-NULL flag, element type
constexpr std::size_t BINARY_DIMENSION_BYTES = 8;  // a dimension's length and lower bound
constexpr std::size_t INT16_BYTES = 2;
constexpr std::size_t INT32_BYTES = 4;
constexpr std::size_t INT64_BYTES = 8;

/** @return The `value_t` at `offset`, read from its big-endian bytes (which the caller has checked are there). */
template <std::unsigned_integral value_t>
value_t load_big_endian(std::span<const std::byte> bytes, std::size_t offset) {
  value_t value = 0;
  std::memcpy(&value, bytes.data() + offset, sizeof(value_t));  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  if constexpr ( std::endian::native == std::endian::little ) {
    value = std::byteswap(value);
  }
  return value;
}

}  // namespace

std::int64_t binary_integer(std::span<const std::byte> bytes) {
  switch ( bytes.size() ) {
    case INT16_BYTES:
      return static_cast<std::int16_t>(load_big_endian<std::uint16_t>(bytes, 0));
    case INT32_BYTES:
      return static_cast<std::int32_t>(load_big_endian<std::uint32_t>(bytes, 0));
    case INT64_BYTES:
      return static_cast<std::int64_t>(load_big_endian<std::uint64_t>(bytes, 0));
    default:
      throw std::invalid_argument("parse_binary: a whole number is 2, 4 or 8 bytes, not " + std::to_string(bytes.size()));
  }
}

double binary_real(std::span<const std::byte> bytes) {
  switch ( bytes.size() ) {
    case INT32_BYTES:
      return static_cast<double>(std::bit_cast<float>(load_big_endian<std::uint32_t>(bytes, 0)));
    case INT64_BYTES:
      return std::bit_cast<double>(load_big_endian<std::uint64_t>(bytes, 0));
    default:
      throw std::invalid_argument("parse_binary: a floating point number is 4 or 8 bytes, not " + std::to_string(bytes.size()));
  }
}

BinaryArrayReader::BinaryArrayReader(std::span<const std::byte> bytes) {
  if ( bytes.size() < BINARY_ARRAY_HEADER ) {
    throw std::invalid_argument("parse_binary: too short to be an array");
  }
  const auto dimensions = static_cast<std::int32_t>(load_big_endian<std::uint32_t>(bytes, 0));
  _element_type = load_big_endian<std::uint32_t>(bytes, 2 * INT32_BYTES);
  if ( dimensions == 0 ) {
    return;  // an empty array has no dimension entry
  }
  if ( dimensions != 1 ) {
    throw std::invalid_argument("parse_binary: a multi-dimensional array does not fit a list");
  }
  if ( bytes.size() < BINARY_ARRAY_HEADER + BINARY_DIMENSION_BYTES ) {
    throw std::invalid_argument("parse_binary: too short to hold its length");
  }
  const auto length = static_cast<std::int32_t>(load_big_endian<std::uint32_t>(bytes, BINARY_ARRAY_HEADER));
  _rest = bytes.subspan(BINARY_ARRAY_HEADER + BINARY_DIMENSION_BYTES);
  // Every element takes at least its length field, so a count above that is a lie (and would be a huge reserve).
  if ( length < 0 || static_cast<std::size_t>(length) > _rest.size() / BINARY_LENGTH_BYTES ) {
    throw std::invalid_argument("parse_binary: the array says it has " + std::to_string(length) + " elements, more than its bytes can hold");
  }
  _size = static_cast<std::size_t>(length);
}

std::span<const std::byte> BinaryArrayReader::next() {
  if ( _rest.size() < BINARY_LENGTH_BYTES ) {
    throw std::invalid_argument("parse_binary: the array ends before its last element");
  }
  const auto length = static_cast<std::int32_t>(load_big_endian<std::uint32_t>(_rest, 0));
  if ( length < 0 ) {
    throw std::invalid_argument("parse_binary: a NULL element does not fit a list");
  }
  if ( static_cast<std::size_t>(length) > _rest.size() - BINARY_LENGTH_BYTES ) {
    throw std::invalid_argument("parse_binary: an element ends after the array's bytes");
  }
  const std::span<const std::byte> element = _rest.subspan(BINARY_LENGTH_BYTES, static_cast<std::size_t>(length));
  _rest = _rest.subspan(BINARY_LENGTH_BYTES + static_cast<std::size_t>(length));
  return element;
}

}  // namespace schemacht::postgres::detail
