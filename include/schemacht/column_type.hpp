#pragma once

/**
 * Column types of your own. Specialize `schemacht::ColumnType<T>` once, next to the type (schema/column_type.hpp says what it
 * holds), and `T` can be a `Field`'s type, a query's argument, and inserted and read like a built-in type. For the common
 * kinds of type, a helper here writes the whole specialization, binary form included where the stored type has one:
 *
 * | Helper                | Stores                                                    | e.g.                                         |
 * |-----------------------|-----------------------------------------------------------|----------------------------------------------|
 * | `WrappedColumn`       | a type wrapping one value, as that value's type           | a strongly typed id, as a `bigint`           |
 * | `EnumColumn`          | an enum, as a PostgreSQL enum (or `text`) of its labels   | `Mood::Happy` as `'happy'`                   |
 * | `EnumIntegerColumn`   | an enum, as its number                                    | `Priority::High` as a `smallint` 2           |
 * | `ScaledNumericColumn` | an exact amount held as a count of units, as a `numeric`  | cents as a `numeric(12,2)`                   |
 * | `ConvertedColumn`     | any type, as another, through two functions               | a path as `text`, a duration as `bigint` ms  |
 *
 * @code
 * struct OrderId { std::int64_t value; auto operator<=>(const OrderId&) const = default; };
 * template <> struct schemacht::ColumnType<OrderId> : schemacht::WrappedColumn<&OrderId::value> {};
 *
 * enum class Mood : std::uint8_t { Sad, Ok, Happy };
 * template <> struct schemacht::ColumnType<Mood> : schemacht::EnumColumn<Mood, "mood", "sad", "ok", "happy"> {};
 *
 * struct Money { std::int64_t cents; auto operator<=>(const Money&) const = default; };
 * template <> struct schemacht::ColumnType<Money> : schemacht::ScaledNumericColumn<&Money::cents, 2, "numeric(12,2)"> {};
 * @endcode
 *
 * For anything else, write the specialization: postgres/binary_form.hpp and postgres/numeric.hpp have what a `parse_binary`
 * is written with. Give the type `operator<=>` if queries should compare with it (`<`, `>`) or order by it.
 * examples/custom_types.cpp walks through a few, and json/json.hpp and uuid.hpp are two the library ships.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "schemacht/concepts/column_type.hpp"  // IWYU pragma: export
#include "schemacht/postgres/binary_form.hpp"  // IWYU pragma: export
#include "schemacht/postgres/field_codec.hpp"  // IWYU pragma: export
#include "schemacht/postgres/numeric.hpp"      // IWYU pragma: export
#include "schemacht/query/sql_type.hpp"        // IWYU pragma: export
#include "schemacht/schema/column_type.hpp"    // IWYU pragma: export
#include "schemacht/schema/field.hpp"          // IWYU pragma: export
#include "schemacht/util/compile_time.hpp"

namespace schemacht {

/**
 * @brief The `ColumnType` of a type that wraps one value of a type the database already knows: stored as that type, in text and
 * in binary form alike (when the inner type has one).
 *
 * @code
 * template <> struct schemacht::ColumnType<OrderId> : schemacht::WrappedColumn<&OrderId::value> {};  // a bigint
 * @endcode
 */
template <auto member>
  requires WrappableMember<member>
struct WrappedColumn {
  using wrapper_type = detail::MemberOf<member>::class_type;
  using inner_type = detail::MemberOf<member>::value_type;

  static constexpr std::string_view SQL_TYPE = query::sql::type_name<inner_type>();

  static wrapper_type parse(std::string_view text) { return wrapper_type{postgres::FieldCodec<inner_type>::parse(text)}; }
  static std::string  format(const wrapper_type& value) { return postgres::FieldCodec<inner_type>::format(value.*member); }

  static wrapper_type parse_binary(std::span<const std::byte> bytes)
    requires postgres::HasBinaryForm<inner_type>
  {
    return wrapper_type{postgres::FieldCodec<inner_type>::parse_binary(bytes)};
  }

  static bool binary_accepts(std::uint32_t type_oid)
    requires requires(std::uint32_t oid) { postgres::FieldCodec<inner_type>::binary_accepts(oid); }
  {
    return postgres::FieldCodec<inner_type>::binary_accepts(type_oid);
  }
};

/**
 * @brief The `ColumnType` of a C++ enum stored as a PostgreSQL enum (`CREATE TYPE sql_type AS ENUM (labels...)`): each value
 * is its label, in text and in binary form alike (PostgreSQL sends an enum's binary form as its label).
 *
 * @code
 * enum class Mood : std::uint8_t { Sad, Ok, Happy };
 * template <> struct schemacht::ColumnType<Mood> : schemacht::EnumColumn<Mood, "mood", "sad", "ok", "happy"> {};
 * @endcode
 *
 * @tparam sql_type The PostgreSQL enum type's name.
 * @tparam labels   One label per value, in the order of the C++ enum's values, which must count up from 0.
 *
 * There is no `binary_accepts`: an enum's type OID is chosen when `CREATE TYPE` runs, so it differs between databases.
 */
template <Enumeration enum_t, util::CTString sql_type, util::CTString... labels>
  requires(sizeof...(labels) > 0)
struct EnumColumn {
  static constexpr std::string_view                                SQL_TYPE = sql_type.view();
  static constexpr std::array<std::string_view, sizeof...(labels)> LABELS{labels.view()...};

  static_assert(util::all_distinct(LABELS), "an EnumColumn's labels must differ from each other");

  /** @throws std::invalid_argument for a label that is not one of `labels` (one added to the type in the database, say). */
  static enum_t parse(std::string_view label) {
    const auto found = std::ranges::find(LABELS, label);
    if ( found == LABELS.end() ) {
      throw std::invalid_argument("'" + std::string(label) + "' is not a label of the enum " + std::string(SQL_TYPE));
    }

    return static_cast<enum_t>(found - LABELS.begin());
  }

  /** @throws std::invalid_argument for a value that has no label. */
  static std::string format(const enum_t& value) {
    const auto index = std::to_underlying(value);
    if ( index < 0 || static_cast<std::size_t>(index) >= LABELS.size() ) {
      throw std::invalid_argument("the value " + std::to_string(index) + " has no label in the enum " + std::string(SQL_TYPE));
    }

    return std::string(LABELS.at(static_cast<std::size_t>(index)));
  }

  static enum_t parse_binary(std::span<const std::byte> bytes) { return parse(postgres::as_text(bytes)); }
};

/**
 * @brief The `ColumnType` of an enum stored as its number: the enum's underlying integer type, in text and in binary form
 * (`std::uint8_t` is a `smallint`, as any unsigned type is the next wider signed one). For a table without a PostgreSQL enum
 * type; `EnumColumn` stores labels instead, which read better in the database and survive reordering the C++ enum.
 *
 * @code
 * enum class Priority : std::uint8_t { Low, Normal, High };
 * template <> struct schemacht::ColumnType<Priority> : schemacht::EnumIntegerColumn<Priority> {};
 * @endcode
 *
 * A number read back is not checked against the enum's values: C++ has no list of them to check with.
 */
template <Enumeration enum_t>
  requires postgres::HasBinaryForm<std::underlying_type_t<enum_t>>
struct EnumIntegerColumn {
  using number_type = std::underlying_type_t<enum_t>;

  static constexpr std::string_view SQL_TYPE = query::sql::type_name<number_type>();

  static enum_t      parse(std::string_view text) { return static_cast<enum_t>(postgres::FieldCodec<number_type>::parse(text)); }
  static std::string format(const enum_t& value) { return postgres::FieldCodec<number_type>::format(std::to_underlying(value)); }

  static enum_t parse_binary(std::span<const std::byte> bytes) { return static_cast<enum_t>(postgres::FieldCodec<number_type>::parse_binary(bytes)); }
  static constexpr bool binary_accepts(std::uint32_t type_oid) { return postgres::FieldCodec<number_type>::binary_accepts(type_oid); }
};

/**
 * @brief The `ColumnType` of an exact amount held as a count of 10^-`scale` units, stored as a `numeric`: in text as
 * PostgreSQL prints one, and in binary form as its base-10000 digits (postgres/numeric.hpp).
 *
 * @code
 * struct Money { std::int64_t cents; auto operator<=>(const Money&) const = default; };
 * template <> struct schemacht::ColumnType<Money> : schemacht::ScaledNumericColumn<&Money::cents, 2, "numeric(12,2)"> {};
 * @endcode
 *
 * A value is refused rather than rounded: `NaN`, an infinity, or a nonzero digit past `scale` places (a `total / 3`).
 *
 * @tparam sql_type The column's type, with the precision and scale the table gives it (`numeric(12,2)`).
 */
template <auto member, unsigned scale, util::CTString sql_type>
  requires ScaledMember<member> && postgres::NumericScale<scale>
struct ScaledNumericColumn {
  using amount_type = detail::MemberOf<member>::class_type;

  static constexpr std::string_view SQL_TYPE = sql_type.view();

  static amount_type parse(std::string_view text) { return amount_type{postgres::parse_scaled_numeric<scale>(text)}; }
  static std::string format(const amount_type& amount) { return postgres::format_scaled_numeric<scale>(amount.*member); }

  static amount_type    parse_binary(std::span<const std::byte> bytes) { return amount_type{postgres::parse_binary_scaled_numeric<scale>(bytes)}; }
  static constexpr bool binary_accepts(std::uint32_t type_oid) { return type_oid == postgres::OID_NUMERIC; }
};

/**
 * @brief The `ColumnType` of a type stored as another the database knows, through two functions (or captureless lambdas): the
 * stored type's SQL type, text form, and binary form when it has one.
 *
 * @code
 * template <>
 * struct schemacht::ColumnType<std::filesystem::path>
 *     : schemacht::ConvertedColumn<std::filesystem::path, std::string,
 *                                  [](const std::filesystem::path& path) { return path.string(); },
 *                                  [](const std::string& text) { return std::filesystem::path(text); }> {};
 * @endcode
 *
 * `from_stored` may throw (`std::invalid_argument`, say) for a stored value that is not one of `value_t`'s.
 */
template <typename value_t, typename stored_t, auto to_stored, auto from_stored>
  requires ConvertsThrough<value_t, stored_t, to_stored, from_stored>
struct ConvertedColumn {
  static constexpr std::string_view SQL_TYPE = query::sql::type_name<stored_t>();

  static value_t     parse(std::string_view text) { return std::invoke(from_stored, postgres::FieldCodec<stored_t>::parse(text)); }
  static std::string format(const value_t& value) { return postgres::FieldCodec<stored_t>::format(std::invoke(to_stored, value)); }

  static value_t parse_binary(std::span<const std::byte> bytes)
    requires postgres::HasBinaryForm<stored_t>
  {
    return std::invoke(from_stored, postgres::FieldCodec<stored_t>::parse_binary(bytes));
  }

  static bool binary_accepts(std::uint32_t type_oid)
    requires requires(std::uint32_t oid) { postgres::FieldCodec<stored_t>::binary_accepts(oid); }
  {
    return postgres::FieldCodec<stored_t>::binary_accepts(type_oid);
  }
};

}  // namespace schemacht
