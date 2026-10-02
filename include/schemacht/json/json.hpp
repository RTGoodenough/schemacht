#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "schemacht/postgres/field_codec.hpp"
#include "schemacht/schema/column_type.hpp"

/**
 * JSON as a column type: PostgreSQL's `jsonb`. Also an example of adding a column type (see column_type.hpp): it
 * specializes `schemacht::ColumnType`.
 */
namespace schemacht::json {

/**
 * @brief A JSON document, held as its text and not parsed.
 *
 * The database checks a document when it is stored, and prints it in its own way (`{"a": 1}`, keys reordered, repeated
 * keys resolved), so a document read back can differ in spelling from the one written: `==` compares the text as it is.
 * A program that needs the values inside parses `text()` with a JSON library of its choice.
 */
class Json {
 public:
  /** @brief The document `null`. */
  Json() : _text("null") {}

  explicit Json(std::string text) : _text(std::move(text)) {}

  Json(const Json&) = default;
  Json(Json&&) noexcept = default;
  Json& operator=(const Json&) = default;
  Json& operator=(Json&&) noexcept = default;
  ~Json() = default;

  /** @return The document as it was written or read. */
  [[nodiscard]] const std::string& text() const noexcept { return _text; }

  [[nodiscard]] bool operator==(const Json&) const = default;

 private:
  std::string _text;
};

}  // namespace schemacht::json

/**
 * @brief A document is stored as `jsonb`. Its text form is its JSON text, which is what PostgreSQL prints and reads; its binary
 * form is a version byte (1) and then the text.
 */
template <>
struct schemacht::ColumnType<schemacht::json::Json> {
  static constexpr std::string_view SQL_TYPE = "jsonb";

  [[nodiscard]] static json::Json  parse(std::string_view text) { return json::Json(std::string(text)); }
  [[nodiscard]] static std::string format(const json::Json& document) { return document.text(); }

  /** @throws std::invalid_argument for a version other than 1, or no bytes. */
  [[nodiscard]] static json::Json parse_binary(std::span<const std::byte> bytes) {
    if ( bytes.empty() || bytes.front() != std::byte{JSONB_BINARY_VERSION} ) {
      throw std::invalid_argument("parse_binary: not a jsonb value (its first byte is its version, and only 1 exists)");
    }
    return json::Json(std::string(postgres::as_text(bytes.subspan(1))));
  }

  /** @brief Only `jsonb` (OID 3802, fixed by the server): a `json` column has no version byte and is refused rather than misread. */
  [[nodiscard]] static constexpr bool binary_accepts(std::uint32_t type_oid) { return type_oid == JSONB_OID; }

 private:
  static constexpr std::uint32_t JSONB_OID = 3802;
  static constexpr std::uint8_t  JSONB_BINARY_VERSION = 1;
};
