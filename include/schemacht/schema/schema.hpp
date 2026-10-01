#pragma once

#include <array>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "schemacht/schema/field.hpp"
#include "schemacht/util/compile_time.hpp"
#include "schemacht/util/type_utils.hpp"
#include "schemacht/util/visit_index.hpp"

namespace schemacht::schema {

/**
 * @brief Compile-time description of a database table.
 *
 * @tparam table_str The table's name, as a compile-time string.
 * @tparam field_ts  The table's columns, each a `Field<...>`.
 */
template <util::CTString table_str, IsField... field_ts>
  requires util::UniqueTypes<field_ts...>
class Schema {
 public:
  static constexpr std::string_view TABLE_NAME{table_str.view()};
  /** @brief The table's name, kept as the `util::CTString` it was declared with (`Field::COLUMN_NAME` is one too); for splicing into another compile-time string (`util::concat_ctstrings`). Most callers want `TABLE_NAME`. */
  static constexpr util::CTString TABLE_NAME_CTSTRING = table_str;
  static constexpr size_t         FIELD_COUNT = sizeof...(field_ts);

  /** @brief One row's worth of field values, in declaration order. */
  using row_type = std::tuple<field_ts...>;

  /** @return Every column's name, in declaration order. */
  [[nodiscard]] static constexpr auto column_names() -> std::array<std::string_view, FIELD_COUNT> {
    return std::array<std::string_view, FIELD_COUNT>{field_ts::COLUMN_NAME.view()...};
  }

  /**
   * @brief Finds a field's index by its column name.
   *
   * @tparam name The column name to look for.
   * @return The field's index, or `FIELD_COUNT` if no field has that name.
   */
  template <util::CTString name>
  [[nodiscard]] static constexpr size_t field_index();

  /**
   * @brief Finds a field's index by its `data_type`.
   *
   * Lets a layer that owns a column type find "its" column, e.g. a domain module locating a schema's column of its own type.
   *
   * @tparam target_t The `data_type` to look for.
   * @return The first matching field's index, or `FIELD_COUNT` if none match.
   */
  template <typename target_t>
  [[nodiscard]] static constexpr size_t field_index_by_type();

  /** @brief The field type at a given index. */
  template <size_t idx_sz>
  using field_at = std::tuple_element_t<idx_sz, std::tuple<field_ts...>>;

  /** @brief The field type with a given column name. */
  template <util::CTString name>
  using field_by_name = field_at<Schema::field_index<name>()>;

  /**
   * @brief Turns a run-time column index into a compile-time one; see `util::visit_index`.
   *
   * Calls `visitor.template operator()<idx>()` with `idx == column`, so the visitor can name `field_at<idx>` and
   * `std::get<idx>(row)`. Every column must give the same result type.
   *
   * @throws std::out_of_range if `column >= FIELD_COUNT`.
   */
  template <typename visitor_t>
  static constexpr decltype(auto) visit_column(std::size_t column, visitor_t&& visitor) {
    return util::visit_index<FIELD_COUNT>(column, std::forward<visitor_t>(visitor));
  }

  /**
   * @brief How many fields are tagged `KeyRole::Primary`.
   *
   * A composite key is any schema with more than one such field; nothing
   * here assumes exactly one.
   */
  [[nodiscard]] static constexpr size_t primary_key_field_count();

  static constexpr size_t PRIMARY_KEY_FIELD_COUNT = primary_key_field_count();
  /** @brief True if any field is tagged `KeyRole::Primary`. */
  static constexpr bool HAS_PRIMARY_KEY = PRIMARY_KEY_FIELD_COUNT > 0;

  /** @return The primary key's column name(s), in declaration order. */
  [[nodiscard]] static constexpr auto primary_key_column_names() -> std::array<std::string_view, PRIMARY_KEY_FIELD_COUNT>;

  /**
   * @brief The index of the (sole) field tagged `KeyRole::Primary`.
   *
   * For a composite key this returns the first tagged field's index --
   * meaningful mainly via `primary_key_field` below, which is only useful
   * when `PRIMARY_KEY_FIELD_COUNT == 1`.
   *
   * @return The field's index, or `FIELD_COUNT` if there's no primary key.
   */
  [[nodiscard]] static constexpr size_t primary_key_field_index();

  /**
   * @brief The field type of a single-column primary key.
   *
   * Always well-formed, even for a schema with no primary key at all --
   * falls back to `field_at<0>` rather than hard-erroring, because a
   * `requires`-constrained method elsewhere (e.g. a future keyed-lookup
   * method) may still need to *name* this type in its signature for
   * schemas where it doesn't apply. A constrained method's parameter types
   * are instantiated as soon as the enclosing class template is, regardless
   * of whether its `requires` clause is satisfied -- unlike a function
   * body, which is only instantiated on actual use. Callers must still
   * gate actual use on `PRIMARY_KEY_FIELD_COUNT == 1`; this alone doesn't
   * mean a single-column primary key exists.
   */
  using primary_key_field = field_at<primary_key_field_index() == FIELD_COUNT ? 0 : primary_key_field_index()>;

  /**
   * @brief How many fields are *not* tagged `GENERATED`.
   *
   * These are the columns a future INSERT/UPSERT should supply a value
   * for; a `GENERATED` column (e.g. the raster table's `geom GENERATED
   * ALWAYS AS (...) STORED`) is computed by the database itself, which
   * would reject a supplied value for it.
   */
  [[nodiscard]] static constexpr size_t insertable_field_count();

  static constexpr size_t INSERTABLE_FIELD_COUNT = insertable_field_count();

  /** @return The non-generated column names, in declaration order. */
  [[nodiscard]] static constexpr auto insertable_column_names() -> std::array<std::string_view, INSERTABLE_FIELD_COUNT>;
};

template <util::CTString table_str, IsField... field_ts>
  requires util::UniqueTypes<field_ts...>
template <util::CTString name>
constexpr size_t Schema<table_str, field_ts...>::field_index() {
  constexpr std::array<std::string_view, FIELD_COUNT> NAMES = {field_ts::COLUMN_NAME.view()...};

  for ( size_t i = 0; i < NAMES.size(); ++i ) {
    if ( NAMES[i] == name.view() ) {
      return i;
    }
  }
  return FIELD_COUNT;  // not found
}

template <util::CTString table_str, IsField... field_ts>
  requires util::UniqueTypes<field_ts...>
template <typename target_t>
constexpr size_t Schema<table_str, field_ts...>::field_index_by_type() {
  constexpr std::array<bool, FIELD_COUNT> MATCHES = {std::is_same_v<typename field_ts::data_type, target_t>...};

  for ( size_t i = 0; i < MATCHES.size(); ++i ) {
    if ( MATCHES[i] ) {
      return i;
    }
  }
  return FIELD_COUNT;  // not found
}

template <util::CTString table_str, IsField... field_ts>
  requires util::UniqueTypes<field_ts...>
constexpr size_t Schema<table_str, field_ts...>::primary_key_field_index() {
  constexpr std::array<bool, FIELD_COUNT> IS_PRIMARY = {(field_ts::KEY_ROLE == KeyRole::Primary)...};

  for ( size_t i = 0; i < FIELD_COUNT; ++i ) {
    if ( IS_PRIMARY[i] ) {
      return i;
    }
  }
  return FIELD_COUNT;  // no primary key
}

template <util::CTString table_str, IsField... field_ts>
  requires util::UniqueTypes<field_ts...>
constexpr size_t Schema<table_str, field_ts...>::primary_key_field_count() {
  constexpr std::array<bool, FIELD_COUNT> IS_PRIMARY = {(field_ts::KEY_ROLE == KeyRole::Primary)...};

  size_t count = 0;
  for ( const bool is_primary : IS_PRIMARY ) {
    if ( is_primary ) {
      ++count;
    }
  }
  return count;
}

template <util::CTString table_str, IsField... field_ts>
  requires util::UniqueTypes<field_ts...>
constexpr auto Schema<table_str, field_ts...>::primary_key_column_names() -> std::array<std::string_view, PRIMARY_KEY_FIELD_COUNT> {
  constexpr std::array<bool, FIELD_COUNT>             IS_PRIMARY = {(field_ts::KEY_ROLE == KeyRole::Primary)...};
  constexpr std::array<std::string_view, FIELD_COUNT> NAMES = {field_ts::COLUMN_NAME.view()...};

  std::array<std::string_view, PRIMARY_KEY_FIELD_COUNT> result{};
  size_t                                                idx = 0;
  for ( size_t i = 0; i < FIELD_COUNT; ++i ) {
    if ( IS_PRIMARY[i] ) {
      result[idx++] = NAMES[i];
    }
  }
  return result;
}

template <util::CTString table_str, IsField... field_ts>
  requires util::UniqueTypes<field_ts...>
constexpr size_t Schema<table_str, field_ts...>::insertable_field_count() {
  constexpr std::array<bool, FIELD_COUNT> IS_INSERTABLE = {(! field_ts::GENERATED)...};

  size_t count = 0;
  for ( const bool is_insertable : IS_INSERTABLE ) {
    if ( is_insertable ) {
      ++count;
    }
  }
  return count;
}

template <util::CTString table_str, IsField... field_ts>
  requires util::UniqueTypes<field_ts...>
constexpr auto Schema<table_str, field_ts...>::insertable_column_names() -> std::array<std::string_view, INSERTABLE_FIELD_COUNT> {
  constexpr std::array<bool, FIELD_COUNT>             IS_INSERTABLE = {(! field_ts::GENERATED)...};
  constexpr std::array<std::string_view, FIELD_COUNT> NAMES = {field_ts::COLUMN_NAME.view()...};

  std::array<std::string_view, INSERTABLE_FIELD_COUNT> result{};
  size_t                                               idx = 0;
  for ( size_t i = 0; i < FIELD_COUNT; ++i ) {
    if ( IS_INSERTABLE[i] ) {
      result[idx++] = NAMES[i];
    }
  }
  return result;
}

// ============================================================================
// Schema concept
// ============================================================================
namespace detail {
template <typename t>
struct is_schema : std::false_type {};

template <util::CTString table_str, typename... field_ts>
struct is_schema<Schema<table_str, field_ts...>> : std::true_type {};
}  // namespace detail

/** @brief Detects whether `data_t` is a `Schema<...>` specialization. */
template <typename data_t>
concept IsSchema = detail::is_schema<data_t>::value;

}  // namespace schemacht::schema
