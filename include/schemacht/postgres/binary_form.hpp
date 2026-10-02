#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

/**
 * Reading PostgreSQL's binary form: what a column type's `parse_binary` and `binary_accepts` are written with (see
 * `schemacht::ColumnType`). PostgreSQL sends every number big endian.
 */
namespace schemacht::postgres {

// The type OIDs (`pg_type.oid`) of PostgreSQL's built-in types, fixed by the server: what `binary_accepts` compares a result
// column's type with. A type an extension or `CREATE TYPE` adds has an OID that differs between databases.
inline constexpr std::uint32_t OID_BOOL = 16;
inline constexpr std::uint32_t OID_BYTEA = 17;
inline constexpr std::uint32_t OID_NAME = 19;
inline constexpr std::uint32_t OID_INT8 = 20;
inline constexpr std::uint32_t OID_INT2 = 21;
inline constexpr std::uint32_t OID_INT4 = 23;
inline constexpr std::uint32_t OID_TEXT = 25;
inline constexpr std::uint32_t OID_FLOAT4 = 700;
inline constexpr std::uint32_t OID_FLOAT8 = 701;
inline constexpr std::uint32_t OID_BPCHAR = 1042;
inline constexpr std::uint32_t OID_VARCHAR = 1043;
inline constexpr std::uint32_t OID_DATE = 1082;
inline constexpr std::uint32_t OID_TIMESTAMP = 1114;
inline constexpr std::uint32_t OID_TIMESTAMPTZ = 1184;
inline constexpr std::uint32_t OID_NUMERIC = 1700;
inline constexpr std::uint32_t OID_UUID = 2950;
inline constexpr std::uint32_t OID_JSONB = 3802;

/**
 * @return `bytes` as the text they hold, valid while they are: for reading a type whose binary form is its text (a string, an
 * enum's label, `jsonb` after its version byte).
 */
[[nodiscard]] inline std::string_view as_text(std::span<const std::byte> bytes) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) -- the one place bytes are viewed as the characters they are
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

/** @return A whole number in binary form: big endian, signed, 2, 4 or 8 bytes (`int2`, `int4`, `int8`). @throws std::invalid_argument for any other length. */
[[nodiscard]] std::int64_t binary_integer(std::span<const std::byte> bytes);

/** @return A floating point number in binary form: big endian IEEE, 4 or 8 bytes (`float4`, `float8`). @throws std::invalid_argument for any other length. */
[[nodiscard]] double binary_real(std::span<const std::byte> bytes);

/**
 * @brief Reads a value's binary form from the front: each call takes the next big-endian field, as a composite type's binary
 * form is laid out (a `numeric`: four 16-bit words, then its digits).
 *
 * @code
 * postgres::BinaryReader reader(bytes);
 * const std::int16_t count = reader.int16();
 * ...
 * reader.expect_end();
 * @endcode
 *
 * Every read throws `std::invalid_argument` if the bytes end before it, so a short value is refused, never read past.
 */
class BinaryReader {
 public:
  explicit BinaryReader(std::span<const std::byte> bytes) noexcept : _rest(bytes) {}

  [[nodiscard]] std::int16_t  int16();
  [[nodiscard]] std::int32_t  int32();
  [[nodiscard]] std::int64_t  int64();
  [[nodiscard]] std::uint16_t uint16();
  [[nodiscard]] std::uint32_t uint32();
  [[nodiscard]] double        float64();

  /** @return The next `count` bytes as they are, valid as long as the value's bytes are. */
  [[nodiscard]] std::span<const std::byte> bytes(std::size_t count);

  /** @return How many bytes are left. */
  [[nodiscard]] std::size_t remaining() const noexcept { return _rest.size(); }

  /** @throws std::invalid_argument if bytes are left: the value was longer than its form says. */
  void expect_end() const;

 private:
  std::span<const std::byte> _rest;
};

/**
 * @brief Walks the elements of a one-dimensional PostgreSQL array in binary form: a header (dimension count, has-NULL flag, the
 * element type's OID, and for one dimension its length and lower bound), then each element as a 4-byte length and its bytes.
 *
 * What `FieldCodec<std::vector<T>>` reads with, and a type of your own whose binary form holds an array can too. Refuses,
 * with `std::invalid_argument`, what a `std::vector<T>` cannot hold: a NULL element (length -1) and more than one dimension;
 * and bytes that end early. An empty array (no dimensions) has no elements.
 */
class BinaryArrayReader {
 public:
  /** @throws std::invalid_argument if `bytes` is not a one-dimensional array's binary form. */
  explicit BinaryArrayReader(std::span<const std::byte> bytes);

  /** @return The element type's OID, from the header. */
  [[nodiscard]] std::uint32_t element_type() const noexcept { return _element_type; }

  /** @return How many elements there are. */
  [[nodiscard]] std::size_t size() const noexcept { return _size; }

  /** @return The next element's bytes, valid as long as the array's bytes are. @pre Fewer than `size()` calls so far. @throws std::invalid_argument for a NULL element or bytes that end early. */
  [[nodiscard]] std::span<const std::byte> next();

 private:
  BinaryReader  _reader;
  std::uint32_t _element_type = 0;
  std::size_t   _size = 0;
};

}  // namespace schemacht::postgres
