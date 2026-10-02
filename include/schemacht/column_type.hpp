#pragma once

/**
 * Column types of your own. Specialize `schemacht::ColumnType<T>` once, next to the type (schema/column_type.hpp says what it
 * holds), and `T` can be a `Field`'s type, a query's argument, and inserted and read like a built-in type. For two common kinds
 * of type, a helper here writes the whole specialization:
 *
 * @code
 * struct OrderId { std::int64_t value; auto operator<=>(const OrderId&) const = default; };
 * template <> struct schemacht::ColumnType<OrderId> : schemacht::WrappedColumn<&OrderId::value> {};
 *
 * enum class Mood : std::uint8_t { Sad, Ok, Happy };
 * template <> struct schemacht::ColumnType<Mood> : schemacht::EnumColumn<Mood, "mood", "sad", "ok", "happy"> {};
 * @endcode
 *
 * Give the type `operator<=>` if queries should compare with it (`<`, `>`) or order by it. examples/custom_types.cpp walks
 * through these and a type with a hand-written text form; json/json.hpp is one the library ships.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "schemacht/postgres/field_codec.hpp"  // IWYU pragma: export
#include "schemacht/query/sql_type.hpp"        // IWYU pragma: export
#include "schemacht/schema/column_type.hpp"    // IWYU pragma: export
#include "schemacht/schema/field.hpp"          // IWYU pragma: export
#include "schemacht/util/compile_time.hpp"
#include "schemacht/util/type_utils.hpp"

namespace schemacht {

namespace detail {

template <auto member>
struct MemberOf {
  static constexpr bool IS_DATA_MEMBER = false;
};

template <typename class_t, typename value_t, value_t class_t::* member>
struct MemberOf<member> {
  static constexpr bool IS_DATA_MEMBER = true;
  using class_type = class_t;
  using value_type = value_t;
};

}  // namespace detail

/**
 * @brief Whether `member` (`&Wrapper::value`) is a data member whose type has a text form, of a class that is made from that one
 * value (`Wrapper{inner}`): a type that wraps a built-in one, as a strongly typed id does.
 */
template <auto member>
concept WrappableMember = detail::MemberOf<member>::IS_DATA_MEMBER && postgres::HasTextForm<typename detail::MemberOf<member>::value_type> &&
                          util::NonNarrowingTo<const typename detail::MemberOf<member>::value_type&, typename detail::MemberOf<member>::class_type>;

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

/** @brief Whether `enum_t` is an enumeration, whose values `EnumColumn` stores as labels. */
template <typename enum_t>
concept Enumeration = std::is_enum_v<enum_t>;

namespace detail {

template <std::size_t count>
[[nodiscard]] consteval bool distinct_labels(const std::array<std::string_view, count>& labels) {
  for ( std::size_t i = 0; i < count; ++i ) {
    for ( std::size_t j = i + 1; j < count; ++j ) {
      if ( labels.at(i) == labels.at(j) ) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace detail

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

  static_assert(detail::distinct_labels(LABELS), "an EnumColumn's labels must differ from each other");

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

}  // namespace schemacht
