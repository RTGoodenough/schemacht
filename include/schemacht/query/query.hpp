#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <type_traits>

#include "schemacht/query/concepts/query.hpp"  // IWYU pragma: export
#include "schemacht/query/predicate.hpp"
#include "schemacht/schema/schema.hpp"
#include "schemacht/util/compile_time.hpp"

namespace schemacht::query {

/** @brief What a query's `LIMIT` is, if it has one. */
enum class LimitKind : std::uint8_t {
  None,
  Constant,  ///< A count written into the query.
  Arg,       ///< A count supplied when the query is bound.
};

/** @brief A query's `LIMIT`: none, a count written into it, or a count bound when it runs. */
struct Limit {
  LimitKind    kind = LimitKind::None;  ///< Which of the three.
  std::int64_t count = 0;               ///< `Constant` only.
  std::size_t  slot = 0;                ///< `Arg` only.

  [[nodiscard]] constexpr bool operator==(const Limit&) const = default;
};

/** @return The indices of every column of `schema_t`, in declaration order. */
template <schema::IsSchema schema_t>
[[nodiscard]] constexpr std::array<std::size_t, schema_t::FIELD_COUNT> all_columns() {
  std::array<std::size_t, schema_t::FIELD_COUNT> columns{};
  for ( std::size_t i = 0; i < columns.size(); ++i ) {
    columns.at(i) = i;
  }
  return columns;
}

/**
 * @brief A whole `SELECT` over one schema: which rows, in what order, how many, and which columns of each.
 *
 * A value that exists at compile time, like `Predicate`. Start with `select`,
 * then add `order_by` and `limit` in any order; the result is what `Prepared`
 * takes. (A bare `Predicate` is also accepted, as a query with no ordering
 * and no limit.)
 *
 * Without `project`, a query returns every column, as the schema declares them.
 *
 * @tparam schema_t           The schema queried.
 * @tparam predicate_capacity Node capacity of the `WHERE` clause.
 * @tparam order_capacity     How many `ORDER BY` terms fit.
 */
template <schema::IsSchema schema_t, std::size_t predicate_capacity, std::size_t order_capacity>
struct Query {
  using schema_type = schema_t;  ///< The schema the query is of.

  Predicate<schema_t, predicate_capacity> predicate{};  ///< Which rows: the `WHERE` clause.
  std::array<OrderTerm, order_capacity>   order{};      ///< The `ORDER BY` terms; `order_count` of them are in use.
  std::size_t                             order_count = 0;
  Limit                                   row_limit{};  ///< The `LIMIT`, if any.

  /// The columns returned, in result order; `column_count` of them are in use, and 0 means every column.
  std::array<std::size_t, schema_t::FIELD_COUNT> columns{};
  std::size_t                                    column_count = 0;

  /** @return The columns returned, in result order: the projection if there is one, else every column. */
  [[nodiscard]] constexpr std::array<std::size_t, schema_t::FIELD_COUNT> result_columns() const {
    return column_count == 0 ? all_columns<schema_t>() : columns;
  }

  /** @return How many columns are returned. */
  [[nodiscard]] constexpr std::size_t result_column_count() const { return column_count == 0 ? schema_t::FIELD_COUNT : column_count; }

  /** @return This query, additionally sorted by `terms` (after any earlier ones). */
  template <typename... ordering_ts>
    requires(std::same_as<ordering_ts, Ordering<schema_t>> && ...)
  [[nodiscard]] constexpr Query<schema_t, predicate_capacity, order_capacity + sizeof...(ordering_ts)> order_by(const ordering_ts&... terms) const {
    Query<schema_t, predicate_capacity, order_capacity + sizeof...(ordering_ts)> result;
    result.predicate = predicate;
    result.row_limit = row_limit;
    result.columns = columns;
    result.column_count = column_count;
    for ( std::size_t i = 0; i < order_count; ++i ) {
      result.order.at(i) = order.at(i);
    }

    std::size_t next = order_count;
    ((result.order.at(next++) = terms.term), ...);
    result.order_count = next;

    return result;
  }

  /**
   * @return This query, returning at most `count` rows.
   * @throws std::out_of_range if `count` is negative or beyond `bigint` -- inside a constant expression, a compile error.
   */
  template <std::integral value_t>
  [[nodiscard]] constexpr Query limit(value_t count) const {
    if ( count < 0 || ! std::in_range<std::int64_t>(count) ) {
      throw std::out_of_range("query limit is negative or too large");
    }

    Query result = *this;
    result.row_limit = Limit{.kind = LimitKind::Constant, .count = static_cast<std::int64_t>(count)};

    return result;
  }

  /**
   * @return This query, returning only `selected`, in that order, instead of every column. Each result row is then a
   * `std::tuple` of just those columns' `Field`s, and only those columns are fetched. The `WHERE` and `ORDER BY` may
   * still use columns that aren't returned. A later `project` replaces an earlier one.
   * @throws std::logic_error if a column is repeated -- inside a constant expression, a compile error.
   */
  template <std::size_t... column_idxs>
    requires(sizeof...(column_idxs) > 0 && sizeof...(column_idxs) <= schema_t::FIELD_COUNT)
  [[nodiscard]] constexpr Query project(const ColumnRef<schema_t, column_idxs>&... /*selected*/) const {
    Query result = *this;
    result.columns = std::array<std::size_t, schema_t::FIELD_COUNT>{column_idxs...};
    result.column_count = sizeof...(column_idxs);

    if ( ! util::all_distinct(std::span(result.columns).first(result.column_count)) ) {
      throw std::logic_error("a column can be projected only once");
    }

    return result;
  }

  /** @return This query, returning at most as many rows as the integer argument `slot_idx` (bound as a `std::int64_t`). */
  template <std::size_t slot_idx>
  [[nodiscard]] constexpr Query limit(ArgRef<slot_idx> /*arg*/) const {
    Query result = *this;
    result.row_limit = Limit{.kind = LimitKind::Arg, .slot = slot_idx};
    return result;
  }
};

/** @return A query for the rows matching `predicate`, in no particular order and without a limit. */
template <schema::IsSchema schema_t, std::size_t capacity>
[[nodiscard]] constexpr Query<schema_t, capacity, 0> select(const Predicate<schema_t, capacity>& predicate) {
  return Query<schema_t, capacity, 0>{.predicate = predicate};
}

/** @return A query for every row of `schema_t`. */
template <schema::IsSchema schema_t>
[[nodiscard]] constexpr Query<schema_t, 1, 0> select() {
  return select(match_all<schema_t>());
}

/** @return `source` as a `Query`. */
template <typename source_t>
  requires IsQuerySource<std::remove_cvref_t<source_t>>
[[nodiscard]] constexpr auto as_query(const source_t& source) {
  if constexpr ( IsQuery<std::remove_cvref_t<source_t>> ) {
    return source;
  } else {
    return select(source);
  }
}

}  // namespace schemacht::query
