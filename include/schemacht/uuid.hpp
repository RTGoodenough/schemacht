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
#include "schemacht/util/hex.hpp"

/**
 * @file
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
    std::size_t                 at_text = 0;
    std::size_t                 at_byte = 0;
    for ( const std::size_t group : GROUP_BYTES ) {
      if ( at_byte != 0 ) {
        if ( text.at(at_text) != '-' ) {
          refuse(text);
        }

        ++at_text;
      }

      try {
        util::parse_hex_into(text.substr(at_text, 2 * group), std::span(bytes).subspan(at_byte, group));
      } catch ( const std::invalid_argument& ) {
        refuse(text);  // the message names the whole text, not one group of it
      }

      at_text += 2 * group;
      at_byte += group;
    }

    return Uuid(bytes);
  }

  /** @return The UUID as PostgreSQL prints it: lowercase hex, in groups of 8-4-4-4-12. */
  [[nodiscard]] constexpr std::string to_string() const {
    std::string text = util::format_hex(_bytes);  // all 32 digits, then a hyphen after every group but the last
    text.reserve(TEXT_SIZE);

    std::size_t at_text = 0;
    for ( const std::size_t group : std::span(GROUP_BYTES).first(GROUP_BYTES.size() - 1) ) {
      at_text += 2 * group;
      text.insert(at_text, 1, '-');
      ++at_text;
    }

    return text;
  }

  [[nodiscard]] constexpr const std::array<std::byte, SIZE>& bytes() const noexcept { return _bytes; }

  [[nodiscard]] constexpr auto operator<=>(const Uuid&) const = default;

 private:
  static constexpr std::size_t TEXT_SIZE = 36;

  // How many bytes each group of the 8-4-4-4-12 hex digits holds; a hyphen comes between them.
  static constexpr std::array<std::size_t, 5> GROUP_BYTES{4, 2, 2, 2, 6};

  std::array<std::byte, SIZE> _bytes{};

  [[noreturn]] static void refuse(std::string_view text) { throw std::invalid_argument("'" + std::string(text) + "' is not a UUID"); }

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
