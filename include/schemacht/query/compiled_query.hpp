#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "schemacht/query/command.hpp"
#include "schemacht/query/query.hpp"
#include "schemacht/schema/schema.hpp"

namespace schemacht::query {

template <auto source>
  requires IsQuerySource<std::remove_cvref_t<decltype(source)>> || IsCommandSource<std::remove_cvref_t<decltype(source)>>
class Prepared;

namespace detail {

template <typename variant_t, typename value_t>
struct AddUnique;

template <typename... existing_ts, typename value_t>
struct AddUnique<std::variant<existing_ts...>, value_t> {
  using type = std::conditional_t<(std::is_same_v<existing_ts, value_t> || ...), std::variant<existing_ts...>, std::variant<existing_ts..., value_t>>;
};

/** @brief Adds each data type, and a `std::vector` of it, to a variant unless already present. */
template <typename variant_t, typename... data_ts>
struct AddAll {
  using type = variant_t;
};

template <typename variant_t, typename head_t, typename... tail_ts>
struct AddAll<variant_t, head_t, tail_ts...> {
  using with_scalar = AddUnique<variant_t, head_t>::type;
  using with_list = AddUnique<with_scalar, std::vector<head_t>>::type;
  using type = AddAll<with_list, tail_ts...>::type;
};

template <schema::IsSchema schema_t, typename indices_t>
struct ParamOf;

template <schema::IsSchema schema_t, std::size_t... idx>
struct ParamOf<schema_t, std::index_sequence<idx...>> {
  // A `LIMIT` argument is a row count, and the number an operation takes (a distance) is a real, whatever the schema holds.
  using with_limit =
      AddUnique<typename AddAll<std::variant<std::monostate>, typename schema_t::template field_at<idx>::value_type...>::type, std::int64_t>::type;
  using type = AddUnique<with_limit, double>::type;
};

}  // namespace detail

/**
 * @brief A value bound to a query argument.
 *
 * The alternatives are exactly what a query over `schema_t` can be given: each
 * column's `value_type` (its `data_type` without any `std::optional`), and a `std::vector` of it for list arguments. That is
 * a closed set fixed by the schema (plus `std::int64_t` for a `LIMIT` and `double` for an operation's number), not
 * an open-ended dynamic type.
 */
template <schema::IsSchema schema_t>
using Param = detail::ParamOf<schema_t, std::make_index_sequence<schema_t::FIELD_COUNT>>::type;

/**
 * @brief A query that has been lowered to SQL at compile time and had its arguments bound.
 *
 * Everything a backend needs to run it: `sql()` with `params()` bound as `$1`, `$2`, ..., and the `columns()` it returns.
 *
 * The type depends only on the schema, not on the query, so a backend's code that sends a statement is
 * compiled once per schema, not once per query. Only `Prepared<...>::bind(...)` makes one: `sql()` and `columns()` view
 * its static storage.
 */
template <schema::IsSchema schema_t>
class CompiledQuery {
 public:
  using schema_type = schema_t;
  using row_type = schema_t::row_type;
  using param_type = Param<schema_t>;

  /** @return The statement, with `$n` placeholders for `params()`. */
  [[nodiscard]] std::string_view sql() const noexcept { return _sql; }

  /** @return The bound arguments, in placeholder order: `params().at(0)` is `$1`. */
  [[nodiscard]] std::span<const param_type> params() const noexcept { return _params; }

  /**
   * @return The columns a backend returns for each row, as indices into the schema's fields, in the order the result
   * wants them. Every column, in declaration order, unless the query projects.
   */
  [[nodiscard]] std::span<const std::size_t> columns() const noexcept { return _columns; }

 private:
  template <auto source>
    requires IsQuerySource<std::remove_cvref_t<decltype(source)>> || IsCommandSource<std::remove_cvref_t<decltype(source)>>
  friend class Prepared;

  std::string_view             _sql;
  std::span<const std::size_t> _columns;
  std::vector<param_type>      _params;

  /**
   * @param sql     The statement, in static storage.
   * @param columns The columns the statement returns, as indices into the schema's fields, in result order; in static storage.
   * @param params  The arguments, in placeholder order: `params[0]` is `$1`.
   */
  CompiledQuery(std::string_view sql, std::span<const std::size_t> columns, std::vector<param_type> params)
      : _sql(sql), _columns(columns), _params(std::move(params)) {}

 public:
  CompiledQuery(const CompiledQuery&) = default;
  CompiledQuery(CompiledQuery&&) noexcept = default;
  CompiledQuery& operator=(const CompiledQuery&) = default;
  CompiledQuery& operator=(CompiledQuery&&) noexcept = default;
  ~CompiledQuery() = default;
};

/**
 * @brief A `CompiledQuery` that also knows the type of the rows it produces.
 *
 * Which columns come back is part of the query, so the result row is too: a
 * `std::tuple` of the returned columns' `Field`s -- the schema's full `row_type`
 * unless the query projects. `CompiledQuery<schema_t>` is what a backend sends; this is what callers hold, so that
 * `execute` can hand back the right type.
 *
 * @tparam result_t The row type; see `Prepared::result_type`.
 */
template <schema::IsSchema schema_t, typename result_t>
class TypedQuery : public CompiledQuery<schema_t> {
 public:
  using result_type = result_t;

 private:
  template <auto source>
    requires IsQuerySource<std::remove_cvref_t<decltype(source)>> || IsCommandSource<std::remove_cvref_t<decltype(source)>>
  friend class Prepared;

  explicit TypedQuery(CompiledQuery<schema_t> query) : CompiledQuery<schema_t>(std::move(query)) {}

 public:
  TypedQuery(const TypedQuery&) = default;
  TypedQuery(TypedQuery&&) noexcept = default;
  TypedQuery& operator=(const TypedQuery&) = default;
  TypedQuery& operator=(TypedQuery&&) noexcept = default;
  ~TypedQuery() = default;
};

}  // namespace schemacht::query
