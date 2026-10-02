#pragma once

#include <array>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "schemacht/schema/field.hpp"

namespace schemacht::postgres {

namespace detail {

[[nodiscard]] std::vector<std::byte> parse_hex_bytes(std::string_view text);
[[nodiscard]] std::string            format_hex_bytes(const std::vector<std::byte>& bytes);

/**
 * @brief Walks the elements of a one-dimensional PostgreSQL array in text form: `{}`, `{1,2,3}`, `{"a b","c\"d"}`.
 *
 * Not a template, so its code is compiled once and not for each element type; `FieldCodec<std::vector<T>>` hands each element's
 * text to `FieldCodec<T>`. Refuses, with `std::invalid_argument`, what a `std::vector<T>` cannot hold: a NULL element, a
 * nested (multi-dimensional) array, a dimension prefix (`[0:2]={...}`), and text that is not an array.
 */
class ArrayReader {
 public:
  /** @throws std::invalid_argument if `text` is not a one-dimensional array. */
  explicit ArrayReader(std::string_view text);

  ArrayReader(const ArrayReader&) = default;
  ArrayReader(ArrayReader&&) noexcept = default;
  ArrayReader& operator=(const ArrayReader&) = default;
  ArrayReader& operator=(ArrayReader&&) noexcept = default;
  ~ArrayReader() = default;

  /** @return At least how many elements there are (a count of the commas, so it may be more): to reserve for. */
  [[nodiscard]] std::size_t element_estimate() const noexcept { return _estimate; }

  /** @return The next element's text, valid until the next call (a quoted one is unescaped), or `std::nullopt` after the last. */
  [[nodiscard]] std::optional<std::string_view> next();

 private:
  [[noreturn]] void fail(std::string_view reason) const;
  void              skip_spaces();
  std::string_view  bare_element();
  std::string_view  quoted_element();

  std::string_view _text;  // all of it, for the messages
  std::string_view _body;  // between the braces
  std::size_t      _at = 0;
  std::size_t      _estimate = 0;
  bool             _after_comma = false;
  std::string      _unescaped;
};

/** @brief The PostgreSQL type OIDs of the built-in types whose binary form the codecs below read (`pg_type.oid`; fixed by the server). */
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

/** @return A whole number in PostgreSQL's binary form: big endian, signed, 2, 4 or 8 bytes (`int2`, `int4`, `int8`). @throws std::invalid_argument for any other length. */
[[nodiscard]] std::int64_t binary_integer(std::span<const std::byte> bytes);

/** @return A floating point number in PostgreSQL's binary form: big endian IEEE, 4 or 8 bytes (`float4`, `float8`). @throws std::invalid_argument for any other length. */
[[nodiscard]] double binary_real(std::span<const std::byte> bytes);

/**
 * @brief Walks the elements of a one-dimensional PostgreSQL array in binary form: a header (dimension count, has-NULL flag, the
 * element type's OID, and for one dimension its length and lower bound), then each element as a 4-byte length and its bytes.
 *
 * Not a template, like `ArrayReader`. Refuses, with `std::invalid_argument`, what a `std::vector<T>` cannot hold: a NULL
 * element (length -1) and more than one dimension; and bytes that end early. An empty array (no dimensions) has no elements.
 */
class BinaryArrayReader {
 public:
  /** @throws std::invalid_argument if `bytes` is not a one-dimensional array's binary form. */
  explicit BinaryArrayReader(std::span<const std::byte> bytes);

  BinaryArrayReader(const BinaryArrayReader&) = default;
  BinaryArrayReader(BinaryArrayReader&&) noexcept = default;
  BinaryArrayReader& operator=(const BinaryArrayReader&) = default;
  BinaryArrayReader& operator=(BinaryArrayReader&&) noexcept = default;
  ~BinaryArrayReader() = default;

  /** @return The element type's OID, from the header. */
  [[nodiscard]] std::uint32_t element_type() const noexcept { return _element_type; }

  /** @return How many elements there are. */
  [[nodiscard]] std::size_t size() const noexcept { return _size; }

  /** @return The next element's bytes, valid as long as the array's bytes are. @pre Fewer than `size()` calls so far. @throws std::invalid_argument for a NULL element or bytes that end early. */
  [[nodiscard]] std::span<const std::byte> next();

 private:
  std::span<const std::byte> _rest;
  std::uint32_t              _element_type = 0;
  std::size_t                _size = 0;
};

/** @brief Whether the primary `FieldCodec` handles `data_t`, in text and in binary: the built-in types. */
template <typename data_t>
inline constexpr bool BUILT_IN_CODEC = std::same_as<data_t, bool> || std::same_as<data_t, std::string> ||
                                       std::same_as<data_t, std::vector<std::byte>> || std::integral<data_t> || std::floating_point<data_t>;

}  // namespace detail

/**
 * @brief How a value of `data_t` is written as text (query arguments, inserted values) and read back from PostgreSQL's
 * text or binary result form.
 *
 * The primary template covers the built-in types: `bool`, integers, floating point, `std::string` and
 * `std::vector<std::byte>` (`bytea`, as `\x` and hex). A `std::vector` of any other type is an array (below). A new column
 * type specializes this, next to the type's definition so it is visible wherever the type is used:
 *
 * @code
 * template <>
 * class postgres::FieldCodec<Money> {
 *  public:
 *   static Money       parse(std::string_view text);
 *   static std::string format(const Money& amount);
 *   static Money       parse_binary(std::span<const std::byte> bytes);  // optional
 *   static bool        binary_accepts(std::uint32_t type_oid);          // optional
 * };
 * @endcode
 *
 * `parse_binary` reads the value as PostgreSQL sends it in binary form, which `Database` asks for by default
 * (`HasBinaryForm`). Rows with a column whose codec lacks it are read as text instead. `binary_accepts`, if present,
 * rejects a result column of the wrong PostgreSQL type before any row is read.
 */
template <typename data_t>
class FieldCodec {
 public:
  [[nodiscard]] constexpr static data_t parse(std::string_view text)
    requires(detail::BUILT_IN_CODEC<data_t>)
  {
    if constexpr ( std::same_as<data_t, bool> ) {
      if ( text == "t" || text == "true" ) {
        return true;
      }
      if ( text == "f" || text == "false" ) {
        return false;
      }
      throw std::invalid_argument("parse_field: '" + std::string(text) + "' is not a boolean (t, f, true or false)");
    } else if constexpr ( std::same_as<data_t, std::string> ) {
      return std::string(text);
    } else if constexpr ( std::same_as<data_t, std::vector<std::byte>> ) {
      if ( text.starts_with("\\x") ) {
        text.remove_prefix(2);  // what a PostgreSQL `bytea` prints: `\x` and then the hex
      }
      return detail::parse_hex_bytes(text);
    } else {  // a number
      data_t            value{};
      const char* const end = text.data() + text.size();  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage) -- the end is passed: from_chars needs no terminator
      const std::from_chars_result result = std::from_chars(text.data(), end, value);
      if ( result.ec != std::errc{} || result.ptr != end ) {
        throw std::invalid_argument("parse_field: could not parse '" + std::string(text) + "' as a number of the column's type");
      }
      return value;
    }
  }

  [[nodiscard]] constexpr static std::string format(const data_t& value)
    requires(detail::BUILT_IN_CODEC<data_t>)
  {
    if constexpr ( std::same_as<data_t, bool> ) {
      return value ? "t" : "f";
    } else if constexpr ( std::same_as<data_t, std::string> ) {
      return value;
    } else if constexpr ( std::same_as<data_t, std::vector<std::byte>> ) {
      return "\\x" + detail::format_hex_bytes(value);  // the `\x` says hex: without it, PostgreSQL would store the hex digits as text
    } else {                                           // a number
      static constexpr std::size_t            NUMERIC_TEXT_CAPACITY = 64;
      std::array<char, NUMERIC_TEXT_CAPACITY> buffer{};
      const auto                              result = std::to_chars(buffer.begin(), buffer.end(), value);
      return std::string(buffer.data(), result.ptr);
    }
  }

  /**
   * @brief Reads a value from PostgreSQL's binary form (a result requested in binary): what the wire carries for the type, not text.
   * The built-in types have it; a whole number is read by its length (2, 4 or 8 bytes) and refused if it does not fit `data_t`.
   * A specialization supplies its own (see `HasBinaryForm`).
   * @throws std::invalid_argument if the bytes are not a value of the type.
   */
  [[nodiscard]] static data_t parse_binary(std::span<const std::byte> bytes)
    requires(detail::BUILT_IN_CODEC<data_t>)
  {
    if constexpr ( std::same_as<data_t, bool> ) {
      if ( bytes.size() != 1 ) {
        throw std::invalid_argument("parse_binary: a bool is one byte, not " + std::to_string(bytes.size()));
      }
      return bytes.front() != std::byte{0};
    } else if constexpr ( std::same_as<data_t, std::string> ) {
      return std::string(reinterpret_cast<const char*>(bytes.data()),
                         bytes.size());  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast) -- text is bytes
    } else if constexpr ( std::same_as<data_t, std::vector<std::byte>> ) {
      return std::vector<std::byte>(bytes.begin(), bytes.end());
    } else if constexpr ( std::integral<data_t> ) {
      const std::int64_t value = detail::binary_integer(bytes);
      if ( ! std::in_range<data_t>(value) ) {
        throw std::invalid_argument("parse_binary: " + std::to_string(value) + " does not fit the column's type");
      }
      return static_cast<data_t>(value);
    } else {
      return static_cast<data_t>(detail::binary_real(bytes));
    }
  }

  /**
   * @brief Whether a column of the PostgreSQL type `type_oid` can be read by `parse_binary`: a mismatch (a `text` column read as a
   * number) is refused before any row is read. A type that has no such function (a specialization for an extension type, whose
   * OID differs by database) is not checked, and has to reject bytes it cannot read itself.
   */
  [[nodiscard]] static constexpr bool binary_accepts(std::uint32_t type_oid)
    requires(detail::BUILT_IN_CODEC<data_t>)
  {
    if constexpr ( std::same_as<data_t, bool> ) {
      return type_oid == detail::OID_BOOL;
    } else if constexpr ( std::same_as<data_t, std::string> ) {
      return type_oid == detail::OID_TEXT || type_oid == detail::OID_VARCHAR || type_oid == detail::OID_BPCHAR || type_oid == detail::OID_NAME;
    } else if constexpr ( std::same_as<data_t, std::vector<std::byte>> ) {
      return type_oid == detail::OID_BYTEA;
    } else if constexpr ( std::integral<data_t> ) {
      return type_oid == detail::OID_INT2 || type_oid == detail::OID_INT4 || type_oid == detail::OID_INT8;
    } else {
      return type_oid == detail::OID_FLOAT4 || type_oid == detail::OID_FLOAT8;
    }
  }
};

/**
 * @brief Whether `data_t` has a text form: its `FieldCodec` has `parse` and `format`. The built-in types do, as does a
 * `std::vector` of a type that does (an array); any other type needs a `FieldCodec` specialization to be a column's type or
 * a statement's argument.
 */
template <typename data_t>
concept HasTextForm = requires(const data_t& value, std::string_view text) {
  { FieldCodec<data_t>::format(value) } -> std::same_as<std::string>;
  { FieldCodec<data_t>::parse(text) } -> std::same_as<data_t>;
};

/** @brief Whether `data_t` can be read from PostgreSQL's binary form: its `FieldCodec` has `parse_binary`. */
template <typename data_t>
concept HasBinaryForm = requires(std::span<const std::byte> bytes) {
  { FieldCodec<data_t>::parse_binary(bytes) } -> std::same_as<data_t>;
};

/**
 * @brief Parses a column's text representation into a `Field`'s declared `data_type`.
 *
 * The one place text turns into a typed value; `read_row` uses it through `parse_column`.
 */
template <HasTextForm data_t>
[[nodiscard]] constexpr data_t parse_field(std::string_view text) {
  return FieldCodec<data_t>::parse(text);
}

/**
 * @brief `parse_field` for the value of one column (`field_t`, a `schema::Field`): a text that does not parse is an
 * `std::invalid_argument` that names the column, so a bad value in a wide row can be found.
 */
template <schema::IsField field_t>
[[nodiscard]] field_t::value_type parse_column(std::string_view text) {
  try {
    return parse_field<typename field_t::value_type>(text);
  } catch ( const std::invalid_argument& error ) {
    throw std::invalid_argument("column '" + std::string(field_t::COLUMN_NAME.view()) + "': " + error.what());
  }
}

/** @brief `parse_column` for a value in binary form (`FieldCodec::parse_binary`); the error names the column the same way. */
template <schema::IsField field_t>
  requires HasBinaryForm<typename field_t::value_type>
[[nodiscard]] field_t::value_type parse_binary_column(std::span<const std::byte> bytes) {
  try {
    return FieldCodec<typename field_t::value_type>::parse_binary(bytes);
  } catch ( const std::invalid_argument& error ) {
    throw std::invalid_argument("column '" + std::string(field_t::COLUMN_NAME.view()) + "': " + error.what());
  }
}

/**
 * @brief Whether a result column of PostgreSQL type `type_oid` may be read into `field_t` in binary: true unless the codec checks types
 * (`binary_accepts`) and does not accept this one. The check the async backend makes once per result.
 */
template <schema::IsField field_t>
[[nodiscard]] constexpr bool binary_column_accepted(std::uint32_t type_oid) {
  using codec_t = FieldCodec<typename field_t::value_type>;
  if constexpr ( requires { codec_t::binary_accepts(type_oid); } ) {
    return codec_t::binary_accepts(type_oid);
  } else {
    return true;
  }
}

/** @brief The inverse of `parse_field`: a typed value's text representation. */
template <HasTextForm data_t>
[[nodiscard]] constexpr std::string format_field(const data_t& value) {
  return FieldCodec<data_t>::format(value);
}

namespace detail {

/** @brief Appends one element's text to an array literal: double-quoted, with `"` and `\` escaped, so any text round-trips. */
inline void append_array_element(std::string& literal, std::string_view text) {
  literal += '"';
  for ( const char chr : text ) {
    if ( chr == '"' || chr == '\\' ) {
      literal += '\\';
    }
    literal += chr;
  }
  literal += '"';
}

}  // namespace detail

/**
 * @brief Formats values as a Postgres array literal (e.g. `{"a","b"}`), for binding as a single array parameter.
 *
 * Each element is `format_field`'s text, quoted and escaped, so any element type, strings included, round-trips.
 */
template <HasTextForm data_t>
[[nodiscard]] std::string format_array(const std::vector<data_t>& values) {
  std::string literal = "{";
  bool        first = true;
  for ( const data_t& value : values ) {
    if ( ! first ) {
      literal += ',';
    }
    first = false;
    detail::append_array_element(literal, format_field<data_t>(value));
  }
  literal += '}';
  return literal;
}

/**
 * @brief The text form of an array column: a `std::vector<element_t>` is a one-dimensional PostgreSQL array (`bigint[]`,
 * `text[]`, ...), for any element type that has a `FieldCodec` of its own.
 *
 * Written as `format_array` writes a list argument, and read from the `{1,2,3}` form the server sends. A NULL element is an error
 * (`std::invalid_argument`): a `std::vector<T>` cannot hold one. `std::vector<std::byte>` is not an array but a `bytea` value
 * (hex), and keeps its own form above.
 */
template <typename element_t>
  requires(! std::same_as<element_t, std::byte> && HasTextForm<element_t>)
class FieldCodec<std::vector<element_t>> {
 public:
  [[nodiscard]] static std::vector<element_t> parse(std::string_view text) {
    detail::ArrayReader    reader(text);
    std::vector<element_t> values;
    values.reserve(reader.element_estimate());
    while ( const std::optional<std::string_view> element = reader.next() ) {
      values.push_back(FieldCodec<element_t>::parse(*element));
    }
    return values;
  }

  [[nodiscard]] static std::string format(const std::vector<element_t>& values) { return format_array<element_t>(values); }

  /**
   * @brief Reads the binary form of an array whose element type `element_t` reads in binary. The element's OID in the array's header is checked
   * against `element_t`'s codec, when it checks types.
   */
  [[nodiscard]] static std::vector<element_t> parse_binary(std::span<const std::byte> bytes)
    requires HasBinaryForm<element_t>
  {
    detail::BinaryArrayReader reader(bytes);
    if constexpr ( requires { FieldCodec<element_t>::binary_accepts(reader.element_type()); } ) {
      if ( ! FieldCodec<element_t>::binary_accepts(reader.element_type()) ) {
        throw std::invalid_argument("parse_binary: the array's elements have the type with OID " + std::to_string(reader.element_type()) +
                                    ", which this column cannot read");
      }
    }
    std::vector<element_t> values;
    values.reserve(reader.size());
    for ( std::size_t i = 0; i < reader.size(); ++i ) {
      values.push_back(FieldCodec<element_t>::parse_binary(reader.next()));
    }
    return values;
  }
};

}  // namespace schemacht::postgres
