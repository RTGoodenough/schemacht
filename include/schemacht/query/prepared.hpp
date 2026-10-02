#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "schemacht/query/command.hpp"
#include "schemacht/query/compiled_query.hpp"
#include "schemacht/query/predicate.hpp"
#include "schemacht/query/query.hpp"
#include "schemacht/query/sql.hpp"
#include "schemacht/schema/schema.hpp"
#include "schemacht/util/concepts/type_utils.hpp"

namespace schemacht::query {

namespace detail {

/** @brief What one `arg<N>()` is: compared with a column (a value, or a list of values), or the count of a `LIMIT`. */
struct SlotInfo {
  std::size_t column = 0;
  bool        is_list = false;
  bool        is_limit = false;
  bool        is_real = false;  ///< The number an operation takes (a threshold, a radius), a `double`.
  bool        seen = false;
};

template <std::size_t capacity>
struct Slots {
  std::array<SlotInfo, capacity> info{};
  std::size_t                    count = 0;
};

/** @brief Records that argument `slot` is used as `use`, checking it's used the same way everywhere. */
template <std::size_t capacity>
constexpr void note_slot(Slots<capacity>& slots, std::size_t slot, const SlotInfo& use) {
  if ( slot >= capacity ) {
    throw std::logic_error("query arguments must be numbered from 0 without gaps");
  }

  SlotInfo& known = slots.info.at(slot);
  if ( known.seen &&
       (known.column != use.column || known.is_list != use.is_list || known.is_limit != use.is_limit || known.is_real != use.is_real) ) {
    throw std::logic_error("query argument is used with columns or list-ness that disagree");
  }

  known = use;
  known.seen = true;
  if ( slot + 1 > slots.count ) {
    slots.count = slot + 1;
  }
}

/** @brief Records the arguments `predicate` uses: each compared with a column, a list for one, or an operation's number. */
template <std::size_t capacity, schema::IsSchema schema_t, std::size_t predicate_capacity>
constexpr void note_predicate_slots(Slots<capacity>& slots, const Predicate<schema_t, predicate_capacity>& predicate) {
  for ( std::size_t i = 0; i < predicate.count; ++i ) {
    const Node& node = predicate.nodes.at(i);
    if ( (node.kind == NodeKind::Compare || node.kind == NodeKind::InList || node.kind == NodeKind::Call) && node.operand.kind == OperandKind::Arg ) {
      note_slot(slots, node.operand.slot, SlotInfo{.column = node.column, .is_list = node.kind == NodeKind::InList});
    }

    if ( node.kind == NodeKind::Call && node.operation->takes_real ) {
      note_slot(slots, node.extra.slot, SlotInfo{.is_real = true});
    }
  }
}

/** @throws std::logic_error (a compile error) if an argument number below the highest is not used. */
template <std::size_t capacity>
constexpr void check_no_gaps(const Slots<capacity>& slots) {
  for ( std::size_t i = 0; i < slots.count; ++i ) {
    if ( ! slots.info.at(i).seen ) {
      throw std::logic_error("query arguments must be numbered from 0 without gaps");
    }
  }
}

/**
 * @brief Works out the type of each argument the user declared, from what they're used with.
 *
 * `arg<N>()` is the statement's `$N+1`.
 *
 * @throws std::logic_error (a compile error) if an argument is used inconsistently, or slots have gaps.
 */
template <schema::IsSchema schema_t, std::size_t predicate_capacity, std::size_t order_capacity>
[[nodiscard]] constexpr Slots<(2 * predicate_capacity) + order_capacity + 1> collect_slots(
    const Query<schema_t, predicate_capacity, order_capacity>& query
) {
  Slots<(2 * predicate_capacity) + order_capacity + 1> slots;  // a call takes up to two, each ordering term one, and the limit one
  note_predicate_slots(slots, query.predicate);

  for ( std::size_t i = 0; i < query.order_count; ++i ) {
    if ( const OrderTerm& term = query.order.at(i); term.ranking != nullptr ) {
      note_slot(slots, term.slot, SlotInfo{.column = term.column});
    }
  }

  if ( query.row_limit.kind == LimitKind::Arg ) {
    note_slot(slots, query.row_limit.slot, SlotInfo{.is_limit = true});
  }

  check_no_gaps(slots);

  return slots;
}

/** @brief As above, for an update: an argument a column is set to has that column's value type. */
template <schema::IsSchema schema_t, std::size_t set_capacity, std::size_t predicate_capacity>
[[nodiscard]] constexpr Slots<set_capacity + (2 * predicate_capacity) + 1> collect_slots(
    const Update<schema_t, set_capacity, predicate_capacity>& update
) {
  Slots<set_capacity + (2 * predicate_capacity) + 1> slots;
  for ( std::size_t i = 0; i < update.set_count; ++i ) {
    if ( const SetTerm& term = update.sets.at(i); ! term.is_null && term.value.kind == OperandKind::Arg ) {
      note_slot(slots, term.value.slot, SlotInfo{.column = term.column});
    }
  }

  note_predicate_slots(slots, update.predicate);

  check_no_gaps(slots);

  return slots;
}

/** @brief As above, for a delete. */
template <schema::IsSchema schema_t, std::size_t predicate_capacity>
[[nodiscard]] constexpr Slots<(2 * predicate_capacity) + 1> collect_slots(const Delete<schema_t, predicate_capacity>& del) {
  Slots<(2 * predicate_capacity) + 1> slots;
  note_predicate_slots(slots, del.predicate);

  check_no_gaps(slots);

  return slots;
}

/** @return `source` as what `Prepared` lowers: a query (a bare predicate becomes one), or the update or delete itself. */
template <typename source_t>
  requires IsQuerySource<source_t> || IsCommandSource<source_t>
[[nodiscard]] constexpr auto statement_of(const source_t& source) {
  if constexpr ( IsCommandSource<source_t> ) {
    return source;
  } else {
    return as_query(source);
  }
}

/** @return The columns `statement` returns, as indices into its schema's fields, in result order: an update's or delete's `RETURNING`. */
template <typename statement_t>
[[nodiscard]] constexpr auto returned_columns(const statement_t& statement) {
  if constexpr ( IsQuery<statement_t> ) {
    return statement.result_columns();
  } else {
    return statement.returned.columns;
  }
}

/** @return How many columns `statement` returns: none for an update or delete without `returning`. */
template <typename statement_t>
[[nodiscard]] constexpr std::size_t returned_column_count(const statement_t& statement) {
  if constexpr ( IsQuery<statement_t> ) {
    return statement.result_column_count();
  } else {
    return statement.returned.count;
  }
}

/** @return Whether `statement` says which rows it changes (a query always does). */
template <typename statement_t>
[[nodiscard]] constexpr bool rows_chosen(const statement_t& statement) {
  if constexpr ( IsCommandSource<statement_t> ) {
    return statement.scope != RowScope::Unset;
  } else {
    return true;
  }
}

/** @return Whether `statement` sets at least one column (a query and a delete need not). */
template <typename statement_t>
[[nodiscard]] constexpr bool sets_a_column(const statement_t& statement) {
  if constexpr ( IsUpdate<statement_t> ) {
    return statement.set_count > 0;
  } else {
    return true;
  }
}

/** @return `statement`'s SQL: a `SELECT`, `UPDATE` or `DELETE`. */
template <typename statement_t>
[[nodiscard]] consteval auto statement_text(const statement_t& statement) {
  if constexpr ( IsUpdate<statement_t> ) {
    return sql::update_statement(statement);

  } else if constexpr ( IsDelete<statement_t> ) {
    return sql::delete_statement(statement);

  } else {
    return sql::select_statement(statement);
  }
}

/** @brief Declared for its type only: the `std::tuple` of the `Field`s at `columns`, in that order. */
template <schema::IsSchema schema_t, auto columns, std::size_t... position>
std::tuple<typename schema_t::template field_at<columns.at(position)>...> projected_row(std::index_sequence<position...> /*positions*/);

}  // namespace detail

/**
 * @brief A query, update or delete lowered to SQL at compile time; call `bind` to supply its arguments.
 *
 * @code
 * using Places = query::On<PlacesSchema>;
 * constexpr auto NEARBY = query::select(Places::col<"zone">().in(query::arg<0>()) && Places::col<"id">() > 10U)
 *                             .order_by(Places::col<"name">().asc())
 *                             .limit(query::arg<1>())
 *                             .project(Places::col<"name">());
 * using NearbyPlaces = query::Prepared<NEARBY>;
 *
 * // one std::tuple<Name-field> per row: only that column is fetched
 * std::vector<NearbyPlaces::result_type> names = database.execute(NearbyPlaces::bind(std::vector<std::int64_t>{...}, 10)).get();
 * @endcode
 *
 * Without `returning(...)`, an update or delete (see command.hpp) returns no rows: its `result_type` is `std::tuple<>`, and a
 * backend's `execute` gives the count of rows it changed instead.
 *
 * @tparam source A constant `Query`, a `Predicate` (a query with no ordering or limit), an `Update` or a `Delete`.
 */
template <auto source>
  requires IsQuerySource<std::remove_cvref_t<decltype(source)>> || IsCommandSource<std::remove_cvref_t<decltype(source)>>
class Prepared {
 public:
  /** @brief `source` as what is lowered: a `Query`, an `Update` or a `Delete`. */
  static constexpr auto QUERY = detail::statement_of(source);

  using query_type = std::remove_cvref_t<decltype(QUERY)>;
  using schema_type = query_type::schema_type;
  using row_type = schema_type::row_type;
  using param_type = Param<schema_type>;

  static_assert(
      detail::rows_chosen(QUERY),
      "an update or delete must say which rows it changes: .where(...), or .all_rows() / delete_all<Schema>() for every row"
  );
  static_assert(detail::sets_a_column(QUERY), "an update must set at least one column");

  /** @brief The columns the statement returns, in result order; none for an update or delete without `returning`. */
  static constexpr auto COLUMNS = detail::returned_columns(QUERY);

  /**
   * @brief The type of a result row: a `std::tuple` of the returned columns' `Field`s, in the order returned.
   * The same as `row_type` unless the query projects; `std::tuple<>` for an update or delete without `returning`.
   */
  using result_type = decltype(detail::projected_row<schema_type, COLUMNS>(std::make_index_sequence<detail::returned_column_count(QUERY)>{}));

  /** @brief The types of the arguments `bind` takes: `arg<N>()` is `$N+1`, and every number from 0 is used. */
  static constexpr auto        SLOTS = detail::collect_slots(QUERY);
  static constexpr std::size_t ARG_COUNT = SLOTS.count;

 private:
  static constexpr auto SQL_CHARS = [] {
    constexpr auto TEXT = detail::statement_text(QUERY);
    return sql::terminated<TEXT.size()>(TEXT);
  }();

 public:
  /** @brief The statement, with `$n` placeholders for the arguments. */
  static constexpr std::string_view SQL{SQL_CHARS.data(), SQL_CHARS.size() - 1};

  /**
   * @brief Supplies the arguments, in the order of their `arg<N>()`.
   *
   * Each argument must convert without narrowing to what its `arg<N>()` is used as:
   * - compared with a column: the column's `value_type` (its `data_type` without any `std::optional`; an argument is never NULL)
   * - `in(...)`: a `std::vector` of the column's `value_type`
   * - `limit(...)`: a `std::int64_t`
   * - an operation's number (a threshold, a radius): a `double`
   *
   * So `"john%"` binds to a `std::string` column and `5` to a `std::int64_t` one, but `-1` does not bind to an unsigned
   * column, `1.5` to a `float` one, or `1` to a `bool` one: those don't compile.
   *
   * @throws std::out_of_range if a limit argument is negative.
   */
  template <typename... arg_ts>
  [[nodiscard]] static TypedQuery<schema_type, result_type> bind(arg_ts&&... args) {
    static_assert(sizeof...(arg_ts) == ARG_COUNT, "wrong number of arguments for this query");

    return bind_in_order(std::index_sequence_for<arg_ts...>{}, std::forward<arg_ts>(args)...);
  }

 private:
  template <std::size_t slot_idx, typename arg_t>
  [[nodiscard]] static param_type make_param(arg_t&& arg) {
    if constexpr ( SLOTS.info.at(slot_idx).is_limit ) {
      static_assert(
          std::integral<std::remove_cvref_t<arg_t>> && util::NonNarrowingTo<arg_t, std::int64_t>,
          "a limit argument must be an integer that converts to std::int64_t without narrowing"
      );

      const std::int64_t count{std::forward<arg_t>(arg)};
      if ( count < 0 ) {
        throw std::out_of_range("query limit argument is negative");
      }

      return param_type{std::in_place_type<std::int64_t>, count};

    } else if constexpr ( SLOTS.info.at(slot_idx).is_real ) {
      static_assert(
          ! std::same_as<std::remove_cvref_t<arg_t>, bool> && util::NonNarrowingTo<arg_t, double>,
          "this argument is an operation's number (such as a threshold) that converts to double without narrowing: write 5.0, not 5"
      );

      return param_type{std::in_place_type<double>, double{std::forward<arg_t>(arg)}};

    } else {
      using data_t = schema_type::template field_at<SLOTS.info.at(slot_idx).column>::value_type;
      using expected_t = std::conditional_t<SLOTS.info.at(slot_idx).is_list, std::vector<data_t>, data_t>;
      static_assert(
          util::NonNarrowingTo<arg_t, expected_t>,
          "query argument has the wrong type for its column, or would be narrowed (e.g. -1 for an unsigned column)"
      );

      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay) -- a string literal argument becomes a std::string here
      return param_type{std::in_place_type<expected_t>, expected_t{std::forward<arg_t>(arg)}};
    }
  }

  template <std::size_t... slot_idx, typename... arg_ts>
  [[nodiscard]] static TypedQuery<schema_type, result_type> bind_in_order(std::index_sequence<slot_idx...> /*slots*/, arg_ts&&... args) {
    std::vector<param_type> params;
    params.reserve(sizeof...(arg_ts));
    (params.push_back(make_param<slot_idx>(std::forward<arg_ts>(args))), ...);

    const std::span<const std::size_t> columns(COLUMNS.data(), detail::returned_column_count(QUERY));
    return TypedQuery<schema_type, result_type>(CompiledQuery<schema_type>(SQL, columns, std::move(params)));
  }
};

}  // namespace schemacht::query
