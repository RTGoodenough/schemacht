#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

/**
 * Bytes as hex text and back: what PostgreSQL's `bytea` (after its `\x`), a `uuid`, and many extension types' text forms
 * (PostGIS's hex EWKB, say) are made of. A column type of your own whose text form is hex uses these too.
 */
namespace schemacht::util {

namespace detail {

inline constexpr unsigned int BITS_PER_HEX_DIGIT = 4;
inline constexpr unsigned int HEX_NIBBLE_MASK = 0xFU;
inline constexpr unsigned int DECIMAL_DIGIT_COUNT = 10;
inline constexpr std::size_t  CHAR_VALUES = 256;

/** @brief What a table entry holds for a character that is not a hex digit: no nibble is above `HEX_NIBBLE_MASK`. */
inline constexpr std::uint8_t NOT_A_DIGIT = 0xFF;

/** @return The value of every hex digit at its character's index, `NOT_A_DIGIT` elsewhere. */
consteval std::array<std::uint8_t, CHAR_VALUES> make_nibble_table() {
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
inline constexpr std::array<std::uint8_t, CHAR_VALUES> NIBBLES = make_nibble_table();

}  // namespace detail

/**
 * @brief Decodes hex `text` into `out`, two digits per byte. Either case of `a`-`f` is accepted.
 * @throws std::invalid_argument if `text` is not exactly two digits per byte of `out`, or has a character that is not a hex digit.
 */
constexpr void parse_hex_into(std::string_view text, std::span<std::byte> out) {
  if ( text.size() != 2 * out.size() ) {
    throw std::invalid_argument(
        "hex: " + std::to_string(text.size()) + " digits for " + std::to_string(out.size()) + " bytes, which take " + std::to_string(2 * out.size())
    );
  }

  unsigned int seen = 0;  // every nibble read, or-ed: a non-digit (NOT_A_DIGIT) leaves a bit above the nibble
  std::size_t  digit = 0;
  for ( std::byte& byte : out ) {
    const unsigned int high = detail::NIBBLES.at(static_cast<unsigned char>(text.at(digit)));
    const unsigned int low = detail::NIBBLES.at(static_cast<unsigned char>(text.at(digit + 1)));
    seen |= high | low;
    byte = static_cast<std::byte>((high << detail::BITS_PER_HEX_DIGIT) | low);
    digit += 2;
  }

  if ( seen > detail::HEX_NIBBLE_MASK ) {
    // Looked for only now, so the loop above has no branch per digit.
    const std::size_t bad = text.find_first_not_of("0123456789abcdefABCDEF");
    throw std::invalid_argument("hex: the character at index " + std::to_string(bad) + " is not a hex digit");
  }
}

/**
 * @return The bytes hex `text` writes, two digits per byte. Either case of `a`-`f` is accepted; the empty text is no bytes.
 * @throws std::invalid_argument for an odd number of digits, or a character that is not a hex digit.
 */
[[nodiscard]] constexpr std::vector<std::byte> parse_hex(std::string_view text) {
  if ( text.size() % 2 != 0 ) {
    throw std::invalid_argument("hex: an odd number of digits");
  }

  std::vector<std::byte> bytes(text.size() / 2);
  parse_hex_into(text, bytes);

  return bytes;
}

/** @return `bytes` as hex, two lowercase digits per byte: as PostgreSQL prints a `bytea` (after its `\x`) or a `uuid`. */
[[nodiscard]] constexpr std::string format_hex(std::span<const std::byte> bytes) {
  constexpr std::string_view DIGITS = "0123456789abcdef";
  std::string                text;
  text.reserve(bytes.size() * 2);

  for ( const std::byte byte : bytes ) {
    const auto value = static_cast<unsigned int>(byte);
    text.push_back(DIGITS.at(value >> detail::BITS_PER_HEX_DIGIT));
    text.push_back(DIGITS.at(value & detail::HEX_NIBBLE_MASK));
  }

  return text;
}

}  // namespace schemacht::util
