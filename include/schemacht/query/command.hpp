#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

#include "schemacht/query/concepts/command.hpp"  // IWYU pragma: export
#include "schemacht/query/predicate.hpp"
#include "schemacht/query/query.hpp"
#include "schemacht/schema/schema.hpp"
#include "schemacht/util/compile_time.hpp"

/**
 * Statements that change rows instead of returning them: `UPDATE` and `DELETE`, written like queries and checked against
 * the schema the same way. `Prepared` lowers them to SQL; a backend's `execute` returns how many rows they changed, or, with
 * `returning(...)`, the rows themselves. examples/custom_types.cpp has an update and a delete that count, and
 * examples/transfers.cpp ones with `returning`.
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

/** @brief The columns an update or delete returns (`RETURNING`), in result order; none unless it asks for them. */
template <schema::IsSchema schema_t>
struct ReturnedColumns {
  std::array<std::size_t, schema_t::FIELD_COUNT> columns{};
  std::size_t                                    count = 0;  ///< How many of `columns` are in use; 0 returns nothing.

  /** @return Every column, in declaration order. */
  [[nodiscard]] static constexpr ReturnedColumns every_column() { return {.columns = all_columns<schema_t>(), .count = schema_t::FIELD_COUNT}; }

  /**
   * @return The columns `column_idxs`, in that order.
   * @throws std::logic_error if a column is repeated -- inside a constant expression, a compile error.
   */
  template <std::size_t... column_idxs>
    requires(sizeof...(column_idxs) > 0 && sizeof...(column_idxs) <= schema_t::FIELD_COUNT)
  [[nodiscard]] static constexpr ReturnedColumns of() {
    const ReturnedColumns result{.columns = {column_idxs...}, .count = sizeof...(column_idxs)};
    if ( ! util::all_distinct(std::span(result.columns).first(result.count)) ) {
      throw std::logic_error("a column can be returned only once");
    }

    return result;
  }
};

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
  ReturnedColumns<schema_t>               returned{};

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

    Update<schema_t, set_capacity, capacity> result{
        .sets = sets,
        .set_count = set_count,
        .predicate = rows,
        .scope = RowScope::Matching,
        .returned = returned,
    };
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

  /**
   * @return This update, also returning every column of each row it changes, as the row is after the change (`RETURNING`):
   * `execute` then gives those rows instead of a count. A later `returning` replaces an earlier one.
   */
  [[nodiscard]] constexpr Update returning() const {
    Update result = *this;
    result.returned = ReturnedColumns<schema_t>::every_column();

    return result;
  }

  /**
   * @return This update, also returning `columns` of each row it changes, in that order.
   * @throws std::logic_error if a column is repeated -- inside a constant expression, a compile error.
   */
  template <std::size_t... column_idxs>
  [[nodiscard]] constexpr Update returning(const ColumnRef<schema_t, column_idxs>&... /*columns*/) const {
    Update result = *this;
    result.returned = ReturnedColumns<schema_t>::template of<column_idxs...>();

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

    Update<schema_t, set_capacity + 1, predicate_capacity> result{
        .set_count = set_count + 1,
        .predicate = predicate,
        .scope = scope,
        .returned = returned,
    };
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
  ReturnedColumns<schema_t>               returned{};

  /**
   * @return This delete, also returning every column of each row it deletes (`RETURNING`): `execute` then gives those rows
   * instead of a count. A later `returning` replaces an earlier one.
   */
  [[nodiscard]] constexpr Delete returning() const {
    Delete result = *this;
    result.returned = ReturnedColumns<schema_t>::every_column();

    return result;
  }

  /**
   * @return This delete, also returning `columns` of each row it deletes, in that order.
   * @throws std::logic_error if a column is repeated -- inside a constant expression, a compile error.
   */
  template <std::size_t... column_idxs>
  [[nodiscard]] constexpr Delete returning(const ColumnRef<schema_t, column_idxs>&... /*columns*/) const {
    Delete result = *this;
    result.returned = ReturnedColumns<schema_t>::template of<column_idxs...>();

    return result;
  }
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

}  // namespace schemacht::query
