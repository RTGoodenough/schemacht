#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <type_traits>

#include "schemacht/query/predicate.hpp"
#include "schemacht/schema/schema.hpp"

/**
 * Statements that change rows instead of returning them: `UPDATE` and `DELETE`, written like queries and checked against
 * the schema the same way. `Prepared` lowers them to SQL; a backend's `execute` returns how many rows they changed.
 *
 * @code
 * using Orders = query::On<OrdersSchema>;
 * constexpr auto MARK_PAID = query::update<OrdersSchema>()
 *                                .set(Orders::col<"paid">(), true)
 *                                .set(Orders::col<"total">(), query::arg<0>())
 *                                .where(Orders::col<"id">() == query::arg<1>());
 * std::uint64_t changed = database.execute(query::Prepared<MARK_PAID>::bind(Money{1999}, 42)).get();
 *
 * constexpr auto PURGE = query::delete_where(Orders::col<"paid">() == false);
 * @endcode
 *
 * An update or delete of every row has to be asked for (`all_rows()`, `delete_all`): one that forgets its `where` doesn't compile.
 */
namespace schemacht::query {

/** @brief Which rows an update or delete changes. */
enum class RowScope : std::uint8_t {
  Unset,     ///< Not said yet: `Prepared` refuses it.
  Matching,  ///< Those its predicate matches.
  All,       ///< Every row, as asked for.
};

/** @brief One `SET` of an `UPDATE`: a column and its new value (a literal, or a bound argument), or NULL. */
struct SetTerm {
  std::size_t column = 0;
  Operand     value{};
  bool        is_null = false;

  [[nodiscard]] constexpr bool operator==(const SetTerm&) const = default;
};

/** @brief Whether `column_t` is a column of `schema_t` (a `ColumnRef`, as `On<schema_t>::col` gives). */
template <typename column_t, typename schema_t>
concept ColumnOf = schema::IsSchema<schema_t> && requires { column_t::INDEX; } && std::same_as<column_t, ColumnRef<schema_t, column_t::INDEX>>;

/** @brief Whether an update can set `column_t`: a column of `schema_t` that the database does not compute (`GENERATED`). */
template <typename column_t, typename schema_t>
concept SettableColumn = ColumnOf<column_t, schema_t> && ! schema_t::template field_at<column_t::INDEX>::GENERATED;

/** @brief Whether an update can set `column_t` to NULL: a settable column whose field is a `std::optional`. */
template <typename column_t, typename schema_t>
concept NullableColumn = SettableColumn<column_t, schema_t> && column_t::NULLABLE;

/**
 * @brief An `UPDATE` of one schema's table: the columns it sets, and which rows. A value that exists at compile time, like
 * `Query`; start with `update<schema_t>()`.
 *
 * @tparam set_capacity       How many `SET` terms fit.
 * @tparam predicate_capacity Node capacity of the `WHERE` clause.
 */
template <schema::IsSchema schema_t, std::size_t set_capacity, std::size_t predicate_capacity>
struct Update {
  using schema_type = schema_t;

  std::array<SetTerm, set_capacity>       sets{};
  std::size_t                             set_count = 0;
  Predicate<schema_t, predicate_capacity> predicate{};
  RowScope                                scope = RowScope::Unset;

  /** @return This update, also setting `column` to the argument `arg<N>()`, which is bound as the column's value type. */
  template <typename column_t, std::size_t slot_idx>
    requires SettableColumn<column_t, schema_t>
  [[nodiscard]] constexpr Update<schema_t, set_capacity + 1, predicate_capacity> set(column_t /*column*/, ArgRef<slot_idx> /*arg*/) const {
    return with(SetTerm{.column = column_t::INDEX, .value = Operand{.kind = OperandKind::Arg, .slot = slot_idx}});
  }

  /**
   * @return This update, also setting `column` to a literal (a `bool`, or an integer that fits the column: see `ConstantFor`).
   * @throws std::out_of_range if it does not fit -- inside a constant expression, a compile error.
   */
  template <typename column_t, typename value_t>
    requires SettableColumn<column_t, schema_t> && ConstantFor<typename column_t::data_type, value_t>
  [[nodiscard]] constexpr Update<schema_t, set_capacity + 1, predicate_capacity> set(column_t /*column*/, const value_t& value) const {
    const Constant constant = make_constant<typename column_t::data_type>(value);
    return with(SetTerm{.column = column_t::INDEX, .value = Operand{.kind = OperandKind::Constant, .constant = constant}});
  }

  /** @return This update, also setting `column`, which can be NULL, to NULL. */
  template <typename column_t>
    requires NullableColumn<column_t, schema_t>
  [[nodiscard]] constexpr Update<schema_t, set_capacity + 1, predicate_capacity> set_null(column_t /*column*/) const {
    return with(SetTerm{.column = column_t::INDEX, .is_null = true});
  }

  /**
   * @return This update, changing only the rows `rows` matches.
   * @throws std::logic_error if it already says which rows -- inside a constant expression, a compile error.
   */
  template <std::size_t capacity>
  [[nodiscard]] constexpr Update<schema_t, set_capacity, capacity> where(const Predicate<schema_t, capacity>& rows) const {
    if ( scope != RowScope::Unset ) {
      throw std::logic_error("an update says which rows it changes once: a single where(...) or all_rows()");
    }

    Update<schema_t, set_capacity, capacity> result{.sets = sets, .set_count = set_count, .predicate = rows, .scope = RowScope::Matching};
    return result;
  }

  /**
   * @return This update, changing every row of the table.
   * @throws std::logic_error if it already says which rows -- inside a constant expression, a compile error.
   */
  [[nodiscard]] constexpr Update all_rows() const {
    if ( scope != RowScope::Unset ) {
      throw std::logic_error("an update says which rows it changes once: a single where(...) or all_rows()");
    }

    Update result = *this;
    result.scope = RowScope::All;

    return result;
  }

 private:
  /** @throws std::logic_error if `term`'s column is already set -- inside a constant expression, a compile error. */
  [[nodiscard]] constexpr Update<schema_t, set_capacity + 1, predicate_capacity> with(const SetTerm& term) const {
    for ( std::size_t i = 0; i < set_count; ++i ) {
      if ( sets.at(i).column == term.column ) {
        throw std::logic_error("an update can set a column only once");
      }
    }

    Update<schema_t, set_capacity + 1, predicate_capacity> result{.set_count = set_count + 1, .predicate = predicate, .scope = scope};
    for ( std::size_t i = 0; i < set_count; ++i ) {
      result.sets.at(i) = sets.at(i);
    }
    result.sets.at(set_count) = term;

    return result;
  }
};

/** @return An update of `schema_t`'s table that sets nothing yet: add `set(...)`, then `where(...)` or `all_rows()`. */
template <schema::IsSchema schema_t>
[[nodiscard]] constexpr Update<schema_t, 0, 1> update() {
  return {};
}

/** @brief A `DELETE` from one schema's table: which rows. Made by `delete_where` or `delete_all`. */
template <schema::IsSchema schema_t, std::size_t predicate_capacity>
struct Delete {
  using schema_type = schema_t;

  Predicate<schema_t, predicate_capacity> predicate{};
  RowScope                                scope = RowScope::Unset;
};

/**
 * @return A delete of the rows `rows` matches.
 * @throws std::logic_error for `match_all()`, which would delete every row: say so with `delete_all` -- inside a constant
 * expression, a compile error.
 */
template <schema::IsSchema schema_t, std::size_t capacity>
[[nodiscard]] constexpr Delete<schema_t, capacity> delete_where(const Predicate<schema_t, capacity>& rows) {
  if ( rows.count == 0 || rows.nodes.at(rows.root).kind == NodeKind::True ) {
    throw std::logic_error("delete_where(match_all()) would delete every row: write delete_all<Schema>() for that");
  }

  return Delete<schema_t, capacity>{.predicate = rows, .scope = RowScope::Matching};
}

/** @return A delete of every row of `schema_t`'s table. */
template <schema::IsSchema schema_t>
[[nodiscard]] constexpr Delete<schema_t, 1> delete_all() {
  return Delete<schema_t, 1>{.scope = RowScope::All};
}

namespace detail {

template <typename data_t>
struct is_update : std::false_type {};

template <schema::IsSchema schema_t, std::size_t set_capacity, std::size_t predicate_capacity>
struct is_update<Update<schema_t, set_capacity, predicate_capacity>> : std::true_type {};

template <typename data_t>
struct is_delete : std::false_type {};

template <schema::IsSchema schema_t, std::size_t predicate_capacity>
struct is_delete<Delete<schema_t, predicate_capacity>> : std::true_type {};

}  // namespace detail

/** @brief Detects whether `data_t` is an `Update<...>`. */
template <typename data_t>
concept IsUpdate = detail::is_update<data_t>::value;

/** @brief Detects whether `data_t` is a `Delete<...>`. */
template <typename data_t>
concept IsDelete = detail::is_delete<data_t>::value;

/** @brief What `Prepared` accepts besides a query: a statement that changes rows and returns none. */
template <typename data_t>
concept IsCommandSource = IsUpdate<data_t> || IsDelete<data_t>;

}  // namespace schemacht::query
