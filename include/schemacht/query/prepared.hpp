#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "schemacht/query/compiled_query.hpp"
#include "schemacht/query/predicate.hpp"
#include "schemacht/query/query.hpp"
#include "schemacht/query/sql.hpp"

namespace schemacht::query {

namespace detail {

/** @brief What one `arg<N>()` is: compared with a column (a value, or a list of values), or the count of a `LIMIT`. */
struct SlotInfo {
  std::size_t column = 0;
  bool        is_list = false;
  bool        is_limit = false;
  bool        is_real = false;  ///< The number an operation takes (a distance), a `double`.
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

/**
 * @brief Works out the type of each argument the user declared, from what they're used with.
 *
 * `arg<N>()` is the statement's `$N+1`.
 *
 * @throws std::logic_error (a compile error) if an argument is used inconsistently, or slots have gaps.
 */
template <schema::IsSchema schema_t, std::size_t predicate_capacity, std::size_t order_capacity>
[[nodiscard]] constexpr Slots<(2 * predicate_capacity) + order_capacity + 1> collect_slots(
    const Query<schema_t, predicate_capacity, order_capacity>& query) {
  Slots<(2 * predicate_capacity) + order_capacity + 1> slots;  // a call takes up to two, each ordering term one, and the limit one
  for ( std::size_t i = 0; i < query.predicate.count; ++i ) {
    const Node& node = query.predicate.nodes.at(i);
    if ( (node.kind == NodeKind::Compare || node.kind == NodeKind::InList || node.kind == NodeKind::Call) && node.operand.kind == OperandKind::Arg ) {
      note_slot(slots, node.operand.slot, SlotInfo{.column = node.column, .is_list = node.kind == NodeKind::InList});
    }
    if ( node.kind == NodeKind::Call && node.operation->takes_real ) {
      note_slot(slots, node.extra.slot, SlotInfo{.is_real = true});
    }
  }
  for ( std::size_t i = 0; i < query.order_count; ++i ) {
    if ( const OrderTerm& term = query.order.at(i); term.ranking != nullptr ) {
      note_slot(slots, term.slot, SlotInfo{.column = term.column});
    }
  }
  if ( query.row_limit.kind == LimitKind::Arg ) {
    note_slot(slots, query.row_limit.slot, SlotInfo{.is_limit = true});
  }
  for ( std::size_t i = 0; i < slots.count; ++i ) {
    if ( ! slots.info.at(i).seen ) {
      throw std::logic_error("query arguments must be numbered from 0 without gaps");
    }
  }
  return slots;
}

/** @brief Declared for its type only: the `std::tuple` of the `Field`s at `columns`, in that order. */
template <schema::IsSchema schema_t, auto columns, std::size_t... position>
std::tuple<typename schema_t::template field_at<columns.at(position)>...> projected_row(std::index_sequence<position...> /*positions*/);

}  // namespace detail

/**
 * @brief A query lowered to SQL at compile time; call `bind` to supply its arguments.
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
 * std::vector<NearbyPlaces::result_type> names = backend.execute(NearbyPlaces::bind(std::vector<std::uint64_t>{...}, std::uint64_t{10}));
 * @endcode
 *
 * @tparam source A constant `Query`, or a `Predicate` (a query with no ordering or limit).
 */
template <auto source>
  requires IsQuerySource<std::remove_cvref_t<decltype(source)>>
class Prepared {
 public:
  using query_type = std::remove_cvref_t<decltype(as_query(source))>;
  using schema_type = query_type::schema_type;
  using row_type = schema_type::row_type;
  using param_type = Param<schema_type>;

  /** @brief `source` as a `Query`. */
  static constexpr auto QUERY = as_query(source);

  /** @brief The columns the statement returns, in result order. */
  static constexpr auto COLUMNS = QUERY.result_columns();

  /**
   * @brief The type of a result row: a `std::tuple` of the returned columns' `Field`s, in the order returned.
   * The same as `row_type` unless the query projects.
   */
  using result_type = decltype(detail::projected_row<schema_type, COLUMNS>(std::make_index_sequence<QUERY.result_column_count()>{}));

  /** @brief The types of the arguments `bind` takes: `arg<N>()` is `$N+1`, and every number from 0 is used. */
  static constexpr auto        SLOTS = detail::collect_slots(as_query(source));
  static constexpr std::size_t ARG_COUNT = SLOTS.count;

 private:
  static constexpr auto SQL_CHARS = [] {
    constexpr auto TEXT = sql::select_statement(QUERY);
    return sql::terminated<TEXT.size()>(TEXT);
  }();

 public:
  /** @brief The statement, with `$n` placeholders for the arguments. */
  static constexpr std::string_view SQL{SQL_CHARS.data(), SQL_CHARS.size() - 1};

  /**
   * @brief Supplies the arguments, in the order of their `arg<N>()`.
   *
   * Each argument must be its column's `value_type` (its `data_type` without any `std::optional`; an argument is never NULL), or a `std::vector` of it for a list, an unsigned integer for a
   * limit, or a number for an operation that takes one (a distance); anything else doesn't compile.
   */
  template <typename... arg_ts>
  [[nodiscard]] static Typed_Query<schema_type, result_type> bind(arg_ts&&... args) {
    static_assert(sizeof...(arg_ts) == ARG_COUNT, "wrong number of arguments for this query");
    return bind_in_order(std::index_sequence_for<arg_ts...>{}, std::forward<arg_ts>(args)...);
  }

 private:
  template <std::size_t slot_idx, typename arg_t>
  [[nodiscard]] static param_type make_param(arg_t&& arg) {
    if constexpr ( SLOTS.info.at(slot_idx).is_limit ) {
      static_assert(std::unsigned_integral<std::remove_cvref_t<arg_t>>, "a limit argument must be an unsigned integer");
      return param_type{std::in_place_type<std::uint64_t>, static_cast<std::uint64_t>(arg)};
    } else if constexpr ( SLOTS.info.at(slot_idx).is_real ) {
      static_assert(std::is_arithmetic_v<std::remove_cvref_t<arg_t>> && ! std::same_as<std::remove_cvref_t<arg_t>, bool>,
                    "this argument is a number (such as a distance)");
      return param_type{std::in_place_type<double>, static_cast<double>(arg)};
    } else {
      using data_t = schema_type::template field_at<SLOTS.info.at(slot_idx).column>::value_type;
      using expected_t = std::conditional_t<SLOTS.info.at(slot_idx).is_list, std::vector<data_t>, data_t>;
      static_assert(std::constructible_from<expected_t, arg_t>, "query argument has the wrong type for its column");
      return param_type{std::in_place_type<expected_t>, std::forward<arg_t>(arg)};
    }
  }

  template <std::size_t... slot_idx, typename... arg_ts>
  [[nodiscard]] static Typed_Query<schema_type, result_type> bind_in_order(std::index_sequence<slot_idx...> /*slots*/, arg_ts&&... args) {
    std::vector<param_type> params;
    params.reserve(sizeof...(arg_ts));
    (params.push_back(make_param<slot_idx>(std::forward<arg_ts>(args))), ...);

    const std::span<const std::size_t> columns(COLUMNS.data(), QUERY.result_column_count());
    return Typed_Query<schema_type, result_type>(Compiled_Query<schema_type>(SQL, columns, std::move(params)));
  }
};

}  // namespace schemacht::query
