#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace schemacht::query::sql {

namespace detail {

template <typename data_t>
[[nodiscard]] constexpr std::string_view builtin_type_name() {
  if constexpr ( std::same_as<data_t, bool> ) {
    return "boolean";
  } else if constexpr ( std::same_as<data_t, std::string> ) {
    return "text";
  } else if constexpr ( std::same_as<data_t, std::vector<std::byte>> ) {
    return "bytea";
  } else if constexpr ( std::same_as<data_t, float> ) {
    return "real";
  } else if constexpr ( std::same_as<data_t, double> ) {
    return "double precision";
  } else if constexpr ( std::integral<data_t> && sizeof(data_t) <= sizeof(std::int16_t) ) {
    return "smallint";
  } else if constexpr ( std::integral<data_t> && sizeof(data_t) == sizeof(std::int32_t) ) {
    return "integer";
  } else if constexpr ( std::integral<data_t> && sizeof(data_t) == sizeof(std::int64_t) ) {
    return "bigint";
  } else {
    return "";
  }
}

}  // namespace detail

/**
 * @brief The name of a column type in SQL, used to cast list arguments (`= ANY($1::<name>[])`).
 *
 * The primary template covers the built-in types; for any other it is empty, and Postgres infers the type. **A
 * column type with its own SQL type supplies it by specializing this**, in the module that defines the type:
 *
 * @code
 * template <>
 * class query::sql::SQL_Type<Money> {
 *  public:
 *   static constexpr std::string_view NAME = "numeric";
 * };
 * @endcode
 */
template <typename data_t>
class SQL_Type {
 public:
  static constexpr std::string_view NAME = detail::builtin_type_name<data_t>();
};

/**
 * @brief How a column of `data_t` is written in a `SELECT` list: by default as itself (`"col"`), and if the type says otherwise, as a function of it
 * under the column's own name (`FUNCTION("col") AS "col"`).
 *
 * For a type whose value is read through another form than the column's own: PostGIS's `raster` has no binary output, so it is selected as
 * `ST_AsBinary("rast") AS "rast"`, whose bytes the type's `Field_Codec` reads (in binary as they are, in text after a `\x`). The type says it once,
 * and every query that returns the column reads it correctly, projected or not. Only the returned columns are affected: a `WHERE` or `ORDER BY` names the
 * column. **A column type supplies it by specializing this**, in the module that defines the type:
 *
 * @code
 * template <>
 * class query::sql::Select_Expression<Raster_Tile> {
 *  public:
 *   static constexpr std::string_view FUNCTION = "ST_AsBinary";
 * };
 * @endcode
 */
template <typename data_t>
class Select_Expression {
 public:
  static constexpr std::string_view FUNCTION{};  ///< Empty: the column is selected as it is.
};

/** @return The SQL name of `data_t`, or an empty view if it has none. */
template <typename data_t>
[[nodiscard]] constexpr std::string_view type_name() {
  return SQL_Type<data_t>::NAME;
}

}  // namespace schemacht::query::sql
