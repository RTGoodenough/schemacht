#pragma once

#include <array>
#include <cstddef>
#include <string_view>
#include <tuple>
#include <utility>

#include "schemacht/schema/concepts/schema.hpp"  // IWYU pragma: export
#include "schemacht/schema/field.hpp"
#include "schemacht/util/compile_time.hpp"
#include "schemacht/util/identifier.hpp"

namespace schemacht::schema {

namespace detail {

/** @return How many of `flags` are set. */
template <std::size_t count>
[[nodiscard]] consteval std::size_t count_set(const std::array<bool, count>& flags) {
  std::size_t result = 0;
  for ( const bool flag : flags ) {
    result += flag ? 1 : 0;
  }
  return result;
}

/** @return The entries of `names` whose flag is set, in order. */
template <std::size_t selected, std::size_t count>
[[nodiscard]] consteval std::array<std::string_view, selected> select_names(
    const std::array<std::string_view, count>& names, const std::array<bool, count>& flags
) {
  std::array<std::string_view, selected> result{};
  std::size_t                            next = 0;
  for ( std::size_t i = 0; i < count; ++i ) {
    if ( flags.at(i) ) {
      result.at(next++) = names.at(i);
    }
  }

  return result;
}

/** @return The indices whose flag is set, in order. */
template <std::size_t selected, std::size_t count>
[[nodiscard]] consteval std::array<std::size_t, selected> select_indices(const std::array<bool, count>& flags) {
  std::array<std::size_t, selected> result{};
  std::size_t                       next = 0;
  for ( std::size_t i = 0; i < count; ++i ) {
    if ( flags.at(i) ) {
      result.at(next++) = i;
    }
  }

  return result;
}

/** @brief Declared for its type only: the `std::tuple` of the fields of `row_t` at `positions`, in that order. */
template <typename row_t, auto positions, std::size_t... position>
std::tuple<std::tuple_element_t<positions.at(position), row_t>...> fields_at(std::index_sequence<position...> /*positions*/);

/** @brief Makes a row (a `std::tuple` of `Field`s) from a plain value for each field, in order. */
template <typename row_t>
struct RowMaker;

template <IsField... field_ts>
struct RowMaker<std::tuple<field_ts...>> {
  template <typename... arg_ts>
  [[nodiscard]] static constexpr std::tuple<field_ts...> make(arg_ts&&... args) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay) -- a string literal becomes a std::string here
    return std::tuple<field_ts...>{field_ts{typename field_ts::data_type{std::forward<arg_ts>(args)}}...};
  }
};

}  // namespace detail

/**
 * @brief Compile-time description of a database table.
 *
 * @tparam table_str The table's name, as a compile-time string: the table every statement runs on, unless it is aimed at
 *                   another of its shape named while the program runs (a `Table`, which a statement is aimed at with `on`).
 * @tparam field_ts  The table's columns, each a `Field<...>`, with distinct names.
 */
template <util::CTString table_str, IsField... field_ts>
class Schema {
 public:
  static constexpr std::string_view TABLE_NAME{table_str.view()};
  /** @brief The table's name as a `util::CTString`, for building other compile-time strings. Most callers want `TABLE_NAME`. */
  static constexpr util::CTString TABLE_NAME_CTSTRING = table_str;
  static constexpr std::size_t    FIELD_COUNT = sizeof...(field_ts);

  /** @brief One row's worth of field values, in declaration order. */
  using row_type = std::tuple<field_ts...>;

  /** @return Every column's name, in declaration order. */
  [[nodiscard]] static constexpr std::array<std::string_view, FIELD_COUNT> column_names() { return {field_ts::COLUMN_NAME.view()...}; }

  /** @brief Every column's key role, in declaration order. */
  static constexpr std::array<KeyRole, FIELD_COUNT> KEY_ROLES{field_ts::KEY_ROLE...};

  /**
   * @return A row from a plain value for each column, in declaration order: `Orders::make_row(1, "ann", std::nullopt)` instead of
   * `{Id{1}, Name{"ann"}, Nick{std::nullopt}}`. Each value converts to its column's `data_type` without narrowing.
   */
  template <typename... arg_ts>
    requires RowValues<row_type, arg_ts...>
  [[nodiscard]] static constexpr row_type make_row(arg_ts&&... args) {
    return detail::RowMaker<row_type>::make(std::forward<arg_ts>(args)...);
  }

  static_assert(
      util::is_identifier(TABLE_NAME),
      "a table name must be lowercase letters, digits and underscores, not start with a digit, and be at most 63 bytes"
  );
  static_assert(
      util::all_distinct(std::array<std::string_view, FIELD_COUNT>{field_ts::COLUMN_NAME.view()...}),
      "two fields of the schema have the same column name"
  );

  /**
   * @brief Finds a field's index by its column name.
   * @return The field's index, or `FIELD_COUNT` if no field has that name.
   */
  template <util::CTString name>
  [[nodiscard]] static consteval std::size_t field_index() {
    for ( std::size_t i = 0; i < FIELD_COUNT; ++i ) {
      if ( column_names().at(i) == name.view() ) {
        return i;
      }
    }
    return FIELD_COUNT;
  }

  /** @return Whether the schema has a column named `name`. */
  template <util::CTString name>
  [[nodiscard]] static consteval bool has_column() {
    return field_index<name>() != FIELD_COUNT;
  }

  /** @brief The field type at a given index. */
  template <std::size_t idx_sz>
  using field_at = std::tuple_element_t<idx_sz, row_type>;

  /** @return `field_index<name>()`; an unknown name is a compile error that says so. */
  template <util::CTString name>
  [[nodiscard]] static consteval std::size_t checked_field_index() {
    static_assert(has_column<name>(), "the schema has no column with that name");

    return has_column<name>() ? field_index<name>() : 0;  // 0 after the assertion fails, so no second error follows
  }

  /** @brief The field type with a given column name; an unknown name is a compile error that says so. */
  template <util::CTString name>
  using field_by_name = field_at<checked_field_index<name>()>;

  /** @brief How many fields are tagged `KeyRole::Primary`; more than one makes a composite key. */
  static constexpr std::size_t PRIMARY_KEY_FIELD_COUNT =
      detail::count_set(std::array<bool, FIELD_COUNT>{(field_ts::KEY_ROLE == KeyRole::Primary)...});
  /** @brief True if any field is tagged `KeyRole::Primary`. */
  static constexpr bool HAS_PRIMARY_KEY = PRIMARY_KEY_FIELD_COUNT > 0;

  /** @return The primary key's column name(s), in declaration order. */
  [[nodiscard]] static constexpr std::array<std::string_view, PRIMARY_KEY_FIELD_COUNT> primary_key_column_names() {
    return detail::select_names<PRIMARY_KEY_FIELD_COUNT>(column_names(), {(field_ts::KEY_ROLE == KeyRole::Primary)...});
  }

  /** @brief How many fields are tagged `KeyRole::Unique`: each is unique on its own (see `KeyRole`). */
  static constexpr std::size_t UNIQUE_FIELD_COUNT = detail::count_set(std::array<bool, FIELD_COUNT>{(field_ts::KEY_ROLE == KeyRole::Unique)...});

  /** @return The names of the columns tagged `KeyRole::Unique`, in declaration order. */
  [[nodiscard]] static constexpr std::array<std::string_view, UNIQUE_FIELD_COUNT> unique_column_names() {
    return detail::select_names<UNIQUE_FIELD_COUNT>(column_names(), {(field_ts::KEY_ROLE == KeyRole::Unique)...});
  }

  /** @brief How many fields are not `GENERATED`: the columns an INSERT supplies (the database computes generated ones). */
  static constexpr std::size_t INSERTABLE_FIELD_COUNT = detail::count_set(std::array<bool, FIELD_COUNT>{(! field_ts::GENERATED)...});

  /** @return The non-generated column names, in declaration order. */
  [[nodiscard]] static constexpr std::array<std::string_view, INSERTABLE_FIELD_COUNT> insertable_column_names() {
    return detail::select_names<INSERTABLE_FIELD_COUNT>(column_names(), {(! field_ts::GENERATED)...});
  }

  /** @brief The indices of the non-generated fields, in declaration order: where each column an INSERT sends is in a `row_type`. */
  static constexpr std::array<std::size_t, INSERTABLE_FIELD_COUNT> INSERTABLE_INDICES =
      detail::select_indices<INSERTABLE_FIELD_COUNT>(std::array<bool, FIELD_COUNT>{(! field_ts::GENERATED)...});

  /**
   * @brief A row as an INSERT sends it: the non-generated fields, in declaration order. The same type as `row_type` when no
   * field is generated.
   */
  using insert_row_type = decltype(detail::fields_at<row_type, INSERTABLE_INDICES>(std::make_index_sequence<INSERTABLE_FIELD_COUNT>{}));

  /**
   * @return An `insert_row_type` from a plain value for each non-generated column, in declaration order: for a table whose
   * database fills in some columns (an identity key, say), `Orders::make_insert_row("ann", 1999)` with no value for them.
   */
  template <typename... arg_ts>
    requires RowValues<insert_row_type, arg_ts...>
  [[nodiscard]] static constexpr insert_row_type make_insert_row(arg_ts&&... args) {
    return detail::RowMaker<insert_row_type>::make(std::forward<arg_ts>(args)...);
  }
};

namespace detail {

template <HasSingleColumnKey schema_t>
[[nodiscard]] consteval std::size_t primary_key_index() {
  std::size_t index = 0;
  while ( schema_t::KEY_ROLES.at(index) != KeyRole::Primary ) {
    ++index;
  }
  return index;
}

}  // namespace detail

/** @brief The field of `schema_t`'s primary key, which is one column. */
template <HasSingleColumnKey schema_t>
using primary_key_field = schema_t::template field_at<detail::primary_key_index<schema_t>()>;

}  // namespace schemacht::schema
