#include "schemacht/postgres/binary_form.hpp"

#include <bit>
#include <climits>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>

namespace schemacht::postgres {

namespace {

constexpr std::size_t INT16_BYTES = 2;
constexpr std::size_t INT32_BYTES = 4;
constexpr std::size_t INT64_BYTES = 8;

/** @return The `value_t` at the front of `bytes`, from its big-endian bytes (which the caller has checked are there). */
template <std::unsigned_integral value_t>
value_t load_big_endian(std::span<const std::byte> bytes) {
  value_t value = 0;
  for ( const std::byte byte : bytes.first(sizeof(value_t)) ) {
    value = static_cast<value_t>(value << CHAR_BIT) | std::to_integer<value_t>(byte);
  }

  return value;
}

}  // namespace

std::int64_t binary_integer(std::span<const std::byte> bytes) {
  BinaryReader reader(bytes);
  switch ( bytes.size() ) {
    case INT16_BYTES:
      return reader.int16();
    case INT32_BYTES:
      return reader.int32();
    case INT64_BYTES:
      return reader.int64();
    default:
      throw std::invalid_argument("parse_binary: a whole number is 2, 4 or 8 bytes, not " + std::to_string(bytes.size()));
  }
}

double binary_real(std::span<const std::byte> bytes) {
  switch ( bytes.size() ) {
    case INT32_BYTES:
      return static_cast<double>(std::bit_cast<float>(load_big_endian<std::uint32_t>(bytes)));
    case INT64_BYTES:
      return std::bit_cast<double>(load_big_endian<std::uint64_t>(bytes));
    default:
      throw std::invalid_argument("parse_binary: a floating point number is 4 or 8 bytes, not " + std::to_string(bytes.size()));
  }
}

// ---- BinaryReader -----------------------------------------------------------------------------------------------------

std::span<const std::byte> BinaryReader::bytes(std::size_t count) {
  if ( count > _rest.size() ) {
    throw std::invalid_argument("parse_binary: the value ends " + std::to_string(count - _rest.size()) + " bytes early");
  }

  const std::span<const std::byte> taken = _rest.first(count);
  _rest = _rest.subspan(count);

  return taken;
}

std::uint16_t BinaryReader::uint16() { return load_big_endian<std::uint16_t>(bytes(INT16_BYTES)); }
std::uint32_t BinaryReader::uint32() { return load_big_endian<std::uint32_t>(bytes(INT32_BYTES)); }
std::int16_t  BinaryReader::int16() { return static_cast<std::int16_t>(uint16()); }
std::int32_t  BinaryReader::int32() { return static_cast<std::int32_t>(uint32()); }
std::int64_t  BinaryReader::int64() { return static_cast<std::int64_t>(load_big_endian<std::uint64_t>(bytes(INT64_BYTES))); }
double        BinaryReader::float64() { return std::bit_cast<double>(load_big_endian<std::uint64_t>(bytes(INT64_BYTES))); }

void BinaryReader::expect_end() const {
  if ( ! _rest.empty() ) {
    throw std::invalid_argument("parse_binary: " + std::to_string(_rest.size()) + " bytes are left after the value");
  }
}

// ---- BinaryArrayReader ------------------------------------------------------------------------------------------------

BinaryArrayReader::BinaryArrayReader(std::span<const std::byte> bytes) : _reader(bytes) {
  constexpr std::size_t               ELEMENT_LENGTH_BYTES = 4;  // each element starts with its length
  const std::int32_t                  dimensions = _reader.int32();
  [[maybe_unused]] const std::int32_t has_null = _reader.int32();  // a NULL element is found, and refused, where it is
  // NOLINTNEXTLINE(cppcoreguidelines-prefer-member-initializer) -- the header is read in order, and the element type is its third field
  _element_type = _reader.uint32();

  if ( dimensions == 0 ) {
    return;  // an empty array has no dimension entry
  }

  if ( dimensions != 1 ) {
    throw std::invalid_argument("parse_binary: a multi-dimensional array does not fit a list");
  }

  const std::int32_t                  length = _reader.int32();
  [[maybe_unused]] const std::int32_t lower_bound = _reader.int32();
  // Every element takes at least its length field, so a count above that is a lie (and would be a huge reserve).
  if ( length < 0 || static_cast<std::size_t>(length) > _reader.remaining() / ELEMENT_LENGTH_BYTES ) {
    throw std::invalid_argument("parse_binary: the array says it has " + std::to_string(length) + " elements, more than its bytes can hold");
  }

  _size = static_cast<std::size_t>(length);
}

std::span<const std::byte> BinaryArrayReader::next() {
  const std::int32_t length = _reader.int32();
  if ( length < 0 ) {
    throw std::invalid_argument("parse_binary: a NULL element does not fit a list");
  }

  return _reader.bytes(static_cast<std::size_t>(length));
}

}  // namespace schemacht::postgres
