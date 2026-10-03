#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "schemacht/schema/column_type.hpp"
#include "schemacht/util/concepts/type_utils.hpp"

namespace schemacht::query::sql {

template <typename data_t>
class SqlType;

namespace detail {

/** @brief The SQL name of an array of `element_t` (`text[]`), as a static string; empty if `element_t` has no name. */
template <typename element_t>
class ArrayTypeName {
  static constexpr std::string_view ELEMENT = SqlType<element_t>::NAME;
  static constexpr auto             CHARS = [] {
    std::array<char, ELEMENT.size() + 3> chars{};  // the element's name, "[]", and a terminating null
    std::ranges::copy(ELEMENT, chars.begin());

    chars.at(ELEMENT.size()) = '[';
    chars.at(ELEMENT.size() + 1) = ']';

    return chars;
  }();

 public:
  static constexpr std::string_view VALUE = ELEMENT.empty() ? std::string_view() : std::string_view(CHARS.data(), CHARS.size() - 1);
};

/** @return The PostgreSQL integer type of at least `bytes` bytes. */
[[nodiscard]] constexpr std::string_view integer_type_name(std::size_t bytes) {
  if ( bytes <= sizeof(std::int16_t) ) {
    return "smallint";
  }
  return bytes <= sizeof(std::int32_t) ? "integer" : "bigint";
}

template <typename data_t>
inline constexpr bool IS_SYS_TIME = false;

template <typename duration_t>
inline constexpr bool IS_SYS_TIME<std::chrono::sys_time<duration_t>> = true;

template <typename data_t>
inline constexpr bool IS_LOCAL_TIME = false;

template <typename duration_t>
inline constexpr bool IS_LOCAL_TIME<std::chrono::local_time<duration_t>> = true;

template <typename data_t>
[[nodiscard]] constexpr std::string_view builtin_type_name() {
  if constexpr ( std::same_as<data_t, bool> ) {
    return "boolean";

  } else if constexpr ( std::same_as<data_t, std::string> ) {
    return "text";

  } else if constexpr ( std::same_as<data_t, std::vector<std::byte>> ) {
    return "bytea";

  } else if constexpr ( util::SqlArray<data_t> ) {
    return ArrayTypeName<typename data_t::value_type>::VALUE;  // a `std::vector` is an array: `text[]`

  } else if constexpr ( std::same_as<data_t, float> ) {
    return "real";

  } else if constexpr ( std::same_as<data_t, double> ) {
    return "double precision";

  } else if constexpr ( IS_SYS_TIME<data_t> ) {
    return "timestamptz";

  } else if constexpr ( IS_LOCAL_TIME<data_t> ) {
    return "timestamp";

  } else if constexpr ( std::same_as<data_t, std::chrono::year_month_day> ) {
    return "date";

  } else if constexpr ( std::signed_integral<data_t> ) {
    return integer_type_name(sizeof(data_t));

  } else if constexpr ( std::unsigned_integral<data_t> && sizeof(data_t) < sizeof(std::int64_t) ) {
    return integer_type_name(sizeof(data_t) * 2);  // PostgreSQL has no unsigned types: the next wider signed one holds every value

  } else {
    return "";
  }
}

/** @return `data_t`'s SQL name: its `ColumnType::SQL_TYPE` if it has one, else the built-in name (empty for none). */
template <typename data_t>
[[nodiscard]] constexpr std::string_view default_type_name() {
  if constexpr ( UserColumnType<data_t> ) {
    return ColumnType<data_t>::SQL_TYPE;
  } else {
    return builtin_type_name<data_t>();
  }
}

}  // namespace detail

/**
 * @brief The name of a column type in SQL: its type in `create_table`, and the cast on its arguments (`= ANY($1::<name>[])`).
 *
 * The primary template covers the built-in types (a `std::vector` of one is its array, `text[]`), and a type of your own
 * through its `ColumnType::SQL_TYPE` (schema/column_type.hpp), which is how a type should say it. For any other type it is
 * empty, and Postgres infers the type. Specializing this directly is for a type whose codec is a `postgres::FieldCodec`
 * specialization rather than a `ColumnType`.
 */
template <typename data_t>
class SqlType {
 public:
  static constexpr std::string_view NAME = detail::default_type_name<data_t>();
};

/**
 * @brief How a column of `data_t` is written in a `SELECT` list: by default as itself (`"col"`), or, if the type names a
 * function, as that function of it under the column's own name (`FUNCTION("col") AS "col"`).
 *
 * For a type that is read in another form than the column's own. For example, PostGIS's `raster` has no binary output,
 * so a raster type would be selected as `ST_AsBinary("rast") AS "rast"`. Only the returned columns are affected: `WHERE`
 * and `ORDER BY` still name the column itself. A column type opts in by specializing this with its own `FUNCTION`.
 */
template <typename data_t>
class SelectExpression {
 public:
  static constexpr std::string_view FUNCTION{};  ///< Empty: the column is selected as it is.
};

/** @return The SQL name of `data_t`, or an empty view if it has none. */
template <typename data_t>
[[nodiscard]] constexpr std::string_view type_name() {
  return SqlType<data_t>::NAME;
}

}  // namespace schemacht::query::sql
