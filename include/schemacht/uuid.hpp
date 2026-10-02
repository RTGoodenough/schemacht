#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "schemacht/postgres/binary_form.hpp"
#include "schemacht/schema/column_type.hpp"

/**
 * A UUID as a column type: PostgreSQL's `uuid`. Also an example of a column type made with `schemacht::ColumnType`.
 */
namespace schemacht {

/** @brief A UUID: 16 bytes, written as 32 hex digits in groups of 8-4-4-4-12. The default one is all zeros (the nil UUID). */
class Uuid {
 public:
  static constexpr std::size_t SIZE = 16;

  constexpr explicit Uuid(const std::array<std::byte, SIZE>& bytes) : _bytes(bytes) {}

  /**
   * @return The UUID `text` writes: `0e9cd3a8-3f43-4f4e-9b5c-6b6f8f6f0a1d`, as PostgreSQL prints one (either case of hex).
   * @throws std::invalid_argument for anything else.
   */
  [[nodiscard]] static constexpr Uuid from_string(std::string_view text) {
    if ( text.size() != TEXT_SIZE ) {
      refuse(text);
    }

    std::array<std::byte, SIZE> bytes{};
    std::size_t                 next = 0;
    for ( std::size_t i = 0; i < TEXT_SIZE; ) {
      if ( std::ranges::contains(TEXT_HYPHENS, i) ) {
        if ( text.at(i) != '-' ) {
          refuse(text);
        }
        ++i;
        continue;
      }

      bytes.at(next++) = static_cast<std::byte>((hex_value(text, i) << 4U) | hex_value(text, i + 1));
      i += 2;
    }

    return Uuid(bytes);
  }

  /** @return The UUID as PostgreSQL prints it: lowercase hex, in groups of 8-4-4-4-12. */
  [[nodiscard]] std::string to_string() const {
    constexpr std::string_view DIGITS = "0123456789abcdef";
    std::string                text;
    text.reserve(TEXT_SIZE);

    for ( std::size_t i = 0; i < SIZE; ++i ) {
      if ( std::ranges::contains(BYTES_BEFORE_HYPHEN, i) ) {
        text += '-';
      }

      const auto value = std::to_integer<unsigned>(_bytes.at(i));
      text += DIGITS.at(value >> 4U);
      text += DIGITS.at(value & LOW_NIBBLE);
    }

    return text;
  }

  [[nodiscard]] constexpr const std::array<std::byte, SIZE>& bytes() const noexcept { return _bytes; }

  [[nodiscard]] constexpr auto operator<=>(const Uuid&) const = default;

 private:
  static constexpr std::size_t TEXT_SIZE = 36;

  // Where the hyphens of the 8-4-4-4-12 groups go: at these positions of the text, and before these bytes.
  static constexpr std::array<std::size_t, 4> TEXT_HYPHENS{8, 13, 18, 23};
  static constexpr std::array<std::size_t, 4> BYTES_BEFORE_HYPHEN{4, 6, 8, 10};

  static constexpr unsigned LOW_NIBBLE = 0xFU;
  static constexpr unsigned FIRST_LETTER_DIGIT = 10U;  // the value of hex digit `a`

  std::array<std::byte, SIZE> _bytes{};

  [[noreturn]] static void refuse(std::string_view text) { throw std::invalid_argument("'" + std::string(text) + "' is not a UUID"); }

  [[nodiscard]] static constexpr unsigned hex_value(std::string_view text, std::size_t index) {
    const char chr = text.at(index);
    if ( chr >= '0' && chr <= '9' ) {
      return static_cast<unsigned>(chr - '0');
    }

    if ( chr >= 'a' && chr <= 'f' ) {
      return static_cast<unsigned>(chr - 'a') + FIRST_LETTER_DIGIT;
    }

    if ( chr >= 'A' && chr <= 'F' ) {
      return static_cast<unsigned>(chr - 'A') + FIRST_LETTER_DIGIT;
    }

    refuse(text);
  }

 public:
  constexpr Uuid() = default;
};

/** @brief A `Uuid` is a `uuid`: written as text, and read in binary form as its 16 bytes. */
template <>
struct ColumnType<Uuid> {
  static constexpr std::string_view SQL_TYPE = "uuid";

  [[nodiscard]] static Uuid        parse(std::string_view text) { return Uuid::from_string(text); }
  [[nodiscard]] static std::string format(const Uuid& uuid) { return uuid.to_string(); }

  /** @throws std::invalid_argument for anything but 16 bytes. */
  [[nodiscard]] static Uuid parse_binary(std::span<const std::byte> bytes) {
    if ( bytes.size() != Uuid::SIZE ) {
      throw std::invalid_argument("parse_binary: a uuid is 16 bytes, not " + std::to_string(bytes.size()));
    }

    std::array<std::byte, Uuid::SIZE> copy{};
    std::ranges::copy(bytes, copy.begin());
    return Uuid(copy);
  }

  /** @brief Only `uuid` (OID 2950, fixed by the server). */
  [[nodiscard]] static constexpr bool binary_accepts(std::uint32_t type_oid) { return type_oid == postgres::OID_UUID; }
};

}  // namespace schemacht
