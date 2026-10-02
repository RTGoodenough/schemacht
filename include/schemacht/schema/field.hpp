#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "schemacht/util/compile_time.hpp"
#include "schemacht/util/identifier.hpp"
#include "schemacht/util/type_utils.hpp"

namespace schemacht::schema {

/**
 * @brief How a column is indexed, if it is.
 *
 * - `Primary`/`Unique` mark a field as (part of) a key: no two rows share its value.
 * - A composite key is expressed by tagging more than one field `Primary`
 *   within the same `Schema` -- `Schema` aggregates them itself; `Field`
 *   doesn't need to know about its siblings.
 *
 * @see Schema::HAS_PRIMARY_KEY
 * @see Schema::primary_key_column_names()
 */
enum class KeyRole : std::uint8_t {
  None,
  Primary,
  Unique,
};

namespace detail {

/** @brief `t` without a `std::optional` around it, and whether there was one. */
template <typename t>
struct Unwrapped {
  using type = t;
  static constexpr bool NULLABLE = false;
};

template <typename t>
struct Unwrapped<std::optional<t>> {
  using type = t;
  static constexpr bool NULLABLE = true;
};

}  // namespace detail

/**
 * @brief One typed, named database column.
 *
 * A column that can be NULL is declared with a `std::optional`: `Field<std::optional<std::string>, "name">`. The row
 * stores the `std::optional` (`data_type`), and everything else that names a type -- what a query compares the column
 * with, its SQL type, a bound argument, a sink's `put` -- uses the type inside it (`value_type`).
 *
 * @tparam data_t     The column's C++ value type, in a `std::optional` if the column can be NULL.
 * @tparam column_str The column's name, as a compile-time string.
 * @tparam key_role   Whether this column is (part of) a primary/unique key.
 * @tparam generated  True for a column the database computes (`GENERATED ALWAYS AS (...) STORED`). It is read like any
 *                    other, and never inserted.
 */
template <typename data_t, util::CTString column_str, KeyRole key_role = KeyRole::None, bool generated = false>
struct Field {
  /// What a row holds for this column: `value_type`, or a `std::optional` of it if the column can be NULL.
  using data_type = data_t;
  /// The type of the column's values: `data_type` without the `std::optional`.
  using value_type = detail::Unwrapped<data_t>::type;
  /// Whether the column can be NULL (its `data_type` is a `std::optional`).
  static constexpr bool NULLABLE = detail::Unwrapped<data_t>::NULLABLE;

  static_assert(util::is_identifier(column_str.view()),
                "a column name must be lowercase letters, digits and underscores, not start with a digit, and be at most 63 bytes");
  static_assert(! detail::Unwrapped<value_type>::NULLABLE, "a Field's std::optional cannot be nested: NULL is one state");
  static_assert(! std::unsigned_integral<value_type> || sizeof(value_type) < sizeof(std::int64_t),
                "a 64-bit unsigned column does not fit PostgreSQL's largest integer type (bigint): use std::int64_t");
  static_assert(! NULLABLE || key_role != KeyRole::Primary, "a primary key column cannot be NULL, so it cannot be a std::optional");

  static constexpr util::CTString COLUMN_NAME{column_str};
  static constexpr KeyRole        KEY_ROLE = key_role;
  static constexpr bool           GENERATED = generated;

  /// Reads the value without naming `.value`: `std::string text = name_field;`.
  // NOLINTNEXTLINE(google-explicit-constructor, hicpp-explicit-conversions, cppcoreguidelines-explicit-constructor, misc-explicit-constructor) -- implicit on purpose: a Field reads as its value
  constexpr operator const data_type&() const& noexcept { return value; }

  data_t value;
};

namespace detail {
template <typename t>
struct is_field : std::false_type {};

template <typename data_t, util::CTString column_str, KeyRole key_role, bool generated>
struct is_field<Field<data_t, column_str, key_role, generated>> : std::true_type {};
}  // namespace detail

/** @brief Detects whether `data_t` is a `Field<...>` specialization. */
template <typename data_t>
concept IsField = detail::is_field<data_t>::value;

namespace detail {
template <typename t>
struct is_field_tuple : std::false_type {};

template <IsField... field_ts>
struct is_field_tuple<std::tuple<field_ts...>> : std::true_type {};
}  // namespace detail

/** @brief A row as queries return it: a `std::tuple` of `Field`s, one per returned column, in result order. */
template <typename row_t>
concept FieldTuple = detail::is_field_tuple<row_t>::value;

namespace detail {

template <util::CTString name, IsField... field_ts>
[[nodiscard]] consteval std::size_t count_named(const std::tuple<field_ts...>* /*row*/) {
  return ((field_ts::COLUMN_NAME.view() == name.view() ? 1U : 0U) + ... + 0U);
}

template <util::CTString name, IsField... field_ts>
[[nodiscard]] consteval std::size_t index_of_named(const std::tuple<field_ts...>* /*row*/) {
  constexpr std::array<std::string_view, sizeof...(field_ts)> NAMES{field_ts::COLUMN_NAME.view()...};
  for ( std::size_t i = 0; i < NAMES.size(); ++i ) {
    if ( NAMES.at(i) == name.view() ) {
      return i;
    }
  }
  return NAMES.size();
}

template <typename row_t, typename... arg_ts>
inline constexpr bool ROW_VALUES = false;

template <IsField... field_ts, typename... arg_ts>
  requires(sizeof...(field_ts) == sizeof...(arg_ts))
inline constexpr bool ROW_VALUES<std::tuple<field_ts...>, arg_ts...> = (util::NonNarrowingTo<arg_ts, typename field_ts::data_type> && ...);

}  // namespace detail

/** @brief Whether exactly one field of the row `row_t` (a `FieldTuple`) is the column `name`, so `get<name>` can find it. */
template <typename row_t, util::CTString name>
concept HasColumnNamed = FieldTuple<row_t> && (detail::count_named<name>(static_cast<const row_t*>(nullptr)) == 1);

/**
 * @brief Whether `arg_ts` are a value for each field of the row `row_t`, in order: each converts to its field's `data_type`
 * without narrowing (`std::nullopt` for a column that can be NULL).
 */
template <typename row_t, typename... arg_ts>
concept RowValues = FieldTuple<row_t> && detail::ROW_VALUES<row_t, arg_ts...>;

/**
 * @return The value of the column `name` in `row`, a row as a schema declares it or as a query returns it:
 * `get<"total">(row)` instead of `std::get<1>(row).value`. A reference, as `std::get` gives: into `row`, so it lives as long as `row` does.
 */
template <util::CTString name, typename row_t>
  requires HasColumnNamed<std::remove_cvref_t<row_t>, name>
[[nodiscard]] constexpr decltype(auto) get(row_t&& row) noexcept {
  constexpr std::size_t INDEX = detail::index_of_named<name>(static_cast<const std::remove_cvref_t<row_t>*>(nullptr));
  // NOLINTNEXTLINE(readability-redundant-parentheses) -- not redundant: with them, decltype(auto) gives a reference
  return (std::get<INDEX>(std::forward<row_t>(row)).value);
}

/** @return The address of `field`'s value, or null if the field is NULL; a field that cannot be NULL always has one. */
template <IsField field_t>
[[nodiscard]] constexpr const field_t::value_type* value_if_present(const field_t& field) {
  if constexpr ( field_t::NULLABLE ) {
    return field.value.has_value() ? &*field.value : nullptr;
  } else {
    return &field.value;
  }
}

/**
 * @return The error for a NULL that has to go into `field_t`, which is not a `std::optional`: the database (or file)
 * holds a NULL the schema says it cannot. It names the column.
 */
template <IsField field_t>
[[nodiscard]] std::runtime_error null_in_required_field() {
  return std::runtime_error("column '" + std::string(field_t::COLUMN_NAME.view()) + "' is NULL, but its schema field is not a std::optional");
}

}  // namespace schemacht::schema
