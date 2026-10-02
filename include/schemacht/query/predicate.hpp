#pragma once

#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

#include "schemacht/schema/schema.hpp"
#include "schemacht/util/compile_time.hpp"
#include "schemacht/util/identifier.hpp"
#include "schemacht/util/type_utils.hpp"

namespace schemacht::query {

/** @brief How a column is compared with an operand. */
enum class Comparison : std::uint8_t {
  Equal,
  NotEqual,
  Less,
  LessEqual,
  Greater,
  GreaterEqual,
};

/** @brief The kinds of node a predicate tree is built from. */
enum class NodeKind : std::uint8_t {
  True,       ///< Matches every row.
  False,      ///< Matches no row.
  Compare,    ///< `column <comparison> operand`
  InList,     ///< `column` is one of the values in a list argument.
  And,        ///< `lhs AND rhs`
  Or,         ///< `lhs OR rhs`
  Not,        ///< `NOT lhs`
  Call,       ///< `operation(column, operand)`: a test a module supplies, such as "the geometries intersect".
  IsNull,     ///< `column IS NULL`
  IsNotNull,  ///< `column IS NOT NULL`
};

/** @brief What a literal in a query is: the kinds that can be written into a statement as they are. */
enum class ConstantKind : std::uint8_t {
  Bool,
  Signed,
  Unsigned,
};

/** @brief A literal value, stored as raw bits so it can live in a compile-time tree. */
struct Constant {
  ConstantKind  kind = ConstantKind::Bool;
  std::uint64_t bits = 0;

  [[nodiscard]] constexpr bool operator==(const Constant&) const = default;
};

/** @brief Whether an operand is a literal or a value supplied when the query is bound. */
enum class OperandKind : std::uint8_t {
  Constant,
  Arg,
};

/** @brief The right-hand side of a comparison: a literal, or the `slot`-th bound argument. */
struct Operand {
  OperandKind kind = OperandKind::Arg;
  Constant    constant{};
  std::size_t slot = 0;

  [[nodiscard]] constexpr bool operator==(const Operand&) const = default;
};

/**
 * @brief A test on a column and one argument that a module supplies, beyond the built-in comparisons.
 *
 * What a database runs: a SQL function or operator a module names. Made by `ColumnRef::apply`; not written by hand.
 */
struct Operation {
  /// The SQL function, written `name("column", $n)`, or, if `infix`, the SQL operator, written `"column" name $n`.
  /// Prefer an operator (`@>`, `&&`, `~~`): an index can always answer one, but answers a function call only if the
  /// function has a planner support function (`jsonb_contains`, for one, has none).
  std::string_view name;
  bool             infix = false;       ///< Whether `name` is an operator, written between the column and the argument.
  bool             takes_real = false;  ///< Whether the test has a second, numeric argument: `name("column", $n, $m)`. Functions only.
  /// Fixed SQL written around the argument (`$n::type`), to convert it first. E.g. `lower(` and `)` give
  /// `"column" ~~ lower($1::text)`. The value itself is always a parameter.
  std::string_view argument_prefix;
  std::string_view argument_suffix;
};

/**
 * @brief A way of ranking rows by their column and an argument, smallest first: the SQL operator that does it
 * (`"column" <-> $n`, where an index answers "nearest first").
 */
struct Ranking {
  std::string_view op;  ///< The SQL operator, written between the column and the argument.
};

/**
 * @brief One node of a predicate tree.
 *
 * Which members are meaningful depends on `kind`; the rest stay at their
 * defaults so nodes can be compared member-wise.
 */
struct Node {
  NodeKind         kind = NodeKind::True;
  Comparison       comparison = Comparison::Equal;  ///< `Compare` only.
  std::size_t      column = 0;                      ///< Every kind that tests a column: index into the schema's fields.
  Operand          operand{};                       ///< `Compare`: the right-hand side. `InList`: the list argument.
  std::size_t      lhs = 0;                         ///< `And`/`Or`/`Not`: index of the first child.
  std::size_t      rhs = 0;                         ///< `And`/`Or`: index of the second child.
  const Operation* operation = nullptr;             ///< `Call`: the test; `operand` is its argument.
  Operand          extra{};                         ///< `Call` of an operation that `takes_real`: the numeric argument.
};

/**
 * @brief A `WHERE` clause over one schema, as a value that exists at compile time.
 *
 * Nodes are stored flat, children before parents, so the whole predicate is a
 * structural type and can be passed as a template argument (see `Prepared`).
 * Values that are only known at run time are not stored: they are `arg<N>()`
 * placeholders, supplied when the query is bound.
 *
 * @tparam schema_t The schema whose columns the predicate refers to.
 * @tparam capacity How many nodes fit.
 */
template <schema::IsSchema schema_t, std::size_t capacity>
struct Predicate {
  using schema_type = schema_t;

  std::array<Node, capacity> nodes{};
  std::size_t                count = 0;  ///< Nodes in use.
  std::size_t                root = 0;   ///< Index of the top node.
};

/** @brief Whether `operation_t` names a SQL function (`FUNCTION`): the test is written `FUNCTION("column", $n)`. */
template <typename operation_t>
concept NamesFunction = requires {
  { operation_t::FUNCTION } -> std::convertible_to<std::string_view>;
};

/** @brief Whether `operation_t` names a SQL operator (`OPERATOR`): the test is written `"column" OPERATOR $n`. */
template <typename operation_t>
concept NamesOperator = requires {
  { operation_t::OPERATOR } -> std::convertible_to<std::string_view>;
};

/** @brief Whether `operation_t` takes a number as well as an argument: it says so with `static constexpr bool TAKES_NUMBER = true`. */
template <typename operation_t>
concept TakesNumber = requires { requires operation_t::TAKES_NUMBER; };

/** @brief The SQL an operation writes around its argument: `ARGUMENT_PREFIX` before it and `ARGUMENT_SUFFIX` after it, both empty if it does not say. */
template <typename operation_t>
[[nodiscard]] constexpr std::string_view argument_prefix() {
  if constexpr ( requires {
                   { operation_t::ARGUMENT_PREFIX } -> std::convertible_to<std::string_view>;
                 } ) {
    return operation_t::ARGUMENT_PREFIX;
  } else {
    return {};
  }
}

template <typename operation_t>
[[nodiscard]] constexpr std::string_view argument_suffix() {
  if constexpr ( requires {
                   { operation_t::ARGUMENT_SUFFIX } -> std::convertible_to<std::string_view>;
                 } ) {
    return operation_t::ARGUMENT_SUFFIX;
  } else {
    return {};
  }
}

/**
 * @brief Whether `operation_t` is a test usable in a query (see `ColumnRef::apply`): it names the SQL that does the test, as a
 * function (`FUNCTION`) or as an operator (`OPERATOR`; the form an index can answer for `@>`, `&&`, `LIKE` and the like). Naming
 * both, or neither, is not a test.
 */
template <typename operation_t>
concept OperationOn = (NamesFunction<operation_t> != NamesOperator<operation_t>) && ! TakesNumber<operation_t>;

/**
 * @brief Like `OperationOn`, for a test that also takes a number, such as "within this distance" (which says so with
 * `TAKES_NUMBER`). It is a function: an operator has only two sides.
 */
template <typename operation_t>
concept OperationWithReal = NamesFunction<operation_t> && ! NamesOperator<operation_t> && TakesNumber<operation_t>;

/** @brief Whether `ranking_t` ranks rows by their distance from an argument: it names the SQL operator (`OPERATOR`), smaller sorting first. */
template <typename ranking_t>
concept RankingOn = requires {
  { ranking_t::OPERATOR } -> std::convertible_to<std::string_view>;
};

namespace detail {

/** @return The SQL name of `operation_t`: its operator if it has one, else its function. */
template <typename operation_t>
[[nodiscard]] constexpr std::string_view spelling() {
  if constexpr ( NamesOperator<operation_t> ) {
    return operation_t::OPERATOR;
  } else {
    return operation_t::FUNCTION;
  }
}

/** @brief The `Operation` for `operation_t`: the one object every use of it shares. */
template <typename operation_t>
class BoundOperation {
 public:
  static_assert(! NamesOperator<operation_t> || util::is_sql_operator(spelling<operation_t>()),
                "an operation's OPERATOR must be a single SQL operator: only the characters <>-|#&~=@+*/%^!?, no -- or comment start");
  static_assert(NamesOperator<operation_t> || util::is_sql_function_name(spelling<operation_t>()),
                "an operation's FUNCTION must be a SQL function name: letters, digits and underscores, not starting with a digit");

  static constexpr Operation VALUE{
      .name = spelling<operation_t>(),
      .infix = NamesOperator<operation_t>,
      .takes_real = TakesNumber<operation_t>,
      .argument_prefix = argument_prefix<operation_t>(),
      .argument_suffix = argument_suffix<operation_t>(),
  };
};

/** @brief The `Ranking` for `ranking_t`. */
template <typename ranking_t>
class BoundRanking {
 public:
  static_assert(util::is_sql_operator(ranking_t::OPERATOR),
                "a ranking's OPERATOR must be a single SQL operator: only the characters <>-|#&~=@+*/%^!?, no -- or comment start");

  static constexpr Ranking VALUE{.op = ranking_t::OPERATOR};
};

template <typename data_t>
struct is_predicate : std::false_type {};

template <schema::IsSchema schema_t, std::size_t capacity>
struct is_predicate<Predicate<schema_t, capacity>> : std::true_type {};
}  // namespace detail

/** @brief Detects whether `data_t` is a `Predicate<...>` specialization. */
template <typename data_t>
concept IsPredicate = detail::is_predicate<data_t>::value;

// ============================================================================
// Constants
// ============================================================================

/**
 * @brief Whether a column of `data_t` can be compared with a literal (rather than only with a bound argument).
 *
 * Only booleans and integers can: a literal is written into the statement, so it must be storable as bits and have an
 * obvious SQL spelling. Everything else (strings, doubles, bytes, and any column type a module adds) is compared with an `arg<N>()`.
 */
template <typename data_t>
concept ConstantData =
    std::same_as<data_t, bool> || (std::integral<data_t> && ! std::same_as<data_t, char> && sizeof(data_t) <= sizeof(std::uint64_t));

/** @brief Whether `value_t` is an acceptable literal for a column of `data_t`. */
template <typename data_t, typename value_t>
concept ConstantFor =
    (std::same_as<data_t, bool> && std::same_as<value_t, bool>) || (ConstantData<data_t> && ! std::same_as<data_t, bool> && std::integral<value_t> &&
                                                                    ! std::same_as<value_t, bool> && ! std::same_as<value_t, char>);

/**
 * @brief Converts a literal to its stored form, checking it fits the column.
 * @throws std::out_of_range if an integer doesn't fit `data_t` -- inside a constant expression, a compile error.
 */
template <typename data_t, typename value_t>
  requires ConstantFor<data_t, value_t>
[[nodiscard]] constexpr Constant make_constant(const value_t& value) {
  if constexpr ( std::same_as<data_t, bool> ) {
    return Constant{.kind = ConstantKind::Bool, .bits = value ? 1U : 0U};
  } else {
    if ( ! std::in_range<data_t>(value) ) {
      throw std::out_of_range("query literal does not fit the column's type");
    }

    if constexpr ( std::signed_integral<data_t> ) {
      return Constant{.kind = ConstantKind::Signed, .bits = std::bit_cast<std::uint64_t>(static_cast<std::int64_t>(value))};
    } else {
      return Constant{.kind = ConstantKind::Unsigned, .bits = static_cast<std::uint64_t>(value)};
    }
  }
}

/** @brief The inverse of `make_constant`: a stored literal as a column's `data_type`. */
template <typename data_t>
  requires ConstantData<data_t>
[[nodiscard]] constexpr data_t constant_value(const Constant& constant) {
  if constexpr ( std::same_as<data_t, bool> ) {
    return constant.bits != 0;
  } else if constexpr ( std::signed_integral<data_t> ) {
    return static_cast<data_t>(std::bit_cast<std::int64_t>(constant.bits));
  } else {
    return static_cast<data_t>(constant.bits);
  }
}

// ============================================================================
// Building predicates
// ============================================================================

/** @brief A placeholder for a value supplied when the query is bound; `slot_idx` numbers the arguments from 0. */
template <std::size_t slot_idx>
class ArgRef {
 public:
  static constexpr std::size_t SLOT = slot_idx;
};

/** @return The `slot_idx`-th bound argument. Its type comes from the column it's compared with. */
template <std::size_t slot_idx>
[[nodiscard]] constexpr ArgRef<slot_idx> arg() {
  return {};
}

namespace detail {

/** @brief Copies `source`'s nodes to the end of `dest`, shifting child indices; @return the copied root's new index. */
template <schema::IsSchema schema_t, std::size_t dest_capacity, std::size_t source_capacity>
constexpr std::size_t append_nodes(std::array<Node, dest_capacity>& dest, std::size_t& dest_count,
                                   const Predicate<schema_t, source_capacity>& source) {
  const std::size_t offset = dest_count;

  for ( std::size_t i = 0; i < source.count; ++i ) {
    Node node = source.nodes.at(i);
    if ( node.kind == NodeKind::And || node.kind == NodeKind::Or ) {
      node.lhs += offset;
      node.rhs += offset;
    } else if ( node.kind == NodeKind::Not ) {
      node.lhs += offset;
    }
    dest.at(dest_count++) = node;
  }

  return source.root + offset;
}

template <NodeKind kind, schema::IsSchema schema_t, std::size_t lhs_capacity, std::size_t rhs_capacity>
[[nodiscard]] constexpr Predicate<schema_t, lhs_capacity + rhs_capacity + 1> join(const Predicate<schema_t, lhs_capacity>& lhs,
                                                                                  const Predicate<schema_t, rhs_capacity>& rhs) {
  Predicate<schema_t, lhs_capacity + rhs_capacity + 1> result;
  std::size_t                                          count = 0;
  const std::size_t                                    lhs_root = append_nodes(result.nodes, count, lhs);
  const std::size_t                                    rhs_root = append_nodes(result.nodes, count, rhs);

  result.nodes.at(count) = Node{.kind = kind, .lhs = lhs_root, .rhs = rhs_root};
  result.root = count;
  result.count = count + 1;

  return result;
}

}  // namespace detail

/** @brief `lhs AND rhs` */
template <schema::IsSchema schema_t, std::size_t lhs_capacity, std::size_t rhs_capacity>
[[nodiscard]] constexpr auto operator&&(const Predicate<schema_t, lhs_capacity>& lhs, const Predicate<schema_t, rhs_capacity>& rhs) {
  return detail::join<NodeKind::And>(lhs, rhs);
}

/** @brief `lhs OR rhs` */
template <schema::IsSchema schema_t, std::size_t lhs_capacity, std::size_t rhs_capacity>
[[nodiscard]] constexpr auto operator||(const Predicate<schema_t, lhs_capacity>& lhs, const Predicate<schema_t, rhs_capacity>& rhs) {
  return detail::join<NodeKind::Or>(lhs, rhs);
}

/** @brief `NOT operand` */
template <schema::IsSchema schema_t, std::size_t capacity>
[[nodiscard]] constexpr Predicate<schema_t, capacity + 1> operator!(const Predicate<schema_t, capacity>& operand) {
  Predicate<schema_t, capacity + 1> result;
  std::size_t                       count = 0;
  const std::size_t                 operand_root = detail::append_nodes(result.nodes, count, operand);

  result.nodes.at(count) = Node{.kind = NodeKind::Not, .lhs = operand_root};
  result.root = count;
  result.count = count + 1;

  return result;
}

/** @return A predicate matching every row. */
template <schema::IsSchema schema_t>
[[nodiscard]] constexpr Predicate<schema_t, 1> match_all() {
  return Predicate<schema_t, 1>{.nodes = {Node{.kind = NodeKind::True}}, .count = 1, .root = 0};
}

/** @return A predicate matching no row. */
template <schema::IsSchema schema_t>
[[nodiscard]] constexpr Predicate<schema_t, 1> match_none() {
  return Predicate<schema_t, 1>{.nodes = {Node{.kind = NodeKind::False}}, .count = 1, .root = 0};
}

/** @brief Whether rows can be sorted by a column of `data_t`. */
template <typename data_t>
concept Orderable = std::totally_ordered<data_t>;

/** @brief One `ORDER BY` term: which column, and which way. */
struct OrderTerm {
  std::size_t    column = 0;
  bool           descending = false;
  const Ranking* ranking = nullptr;  ///< Set for "nearest first" and the like: sorts by `ranking`'s number for the column and the argument.
  std::size_t    slot = 0;           ///< `ranking` only: the argument.

  [[nodiscard]] constexpr bool operator==(const OrderTerm&) const = default;
};

/** @brief An `OrderTerm` known to refer to a column of `schema_t`; make one with `ColumnRef::asc()`/`desc()`. */
template <schema::IsSchema schema_t>
struct Ordering {
  OrderTerm term;
};

/**
 * @brief A reference to one of a schema's columns, used to build comparisons.
 *
 * Comparisons are checked against the column's type when they're written: a
 * literal of the wrong type, or one that doesn't fit, doesn't compile. Only
 * bool and integer columns can be compared with a literal; anything
 * else (strings, doubles, bytes) is compared with an `arg<N>()`.
 */
template <schema::IsSchema schema_t, std::size_t column_idx>
  requires(column_idx < schema_t::FIELD_COUNT)
class ColumnRef {
 public:
  /// The type of the column's values: arguments and literals it is compared with are of this type, even if the column
  /// can be NULL (its field's `data_type` is then a `std::optional` of it).
  using data_type = schema_t::template field_at<column_idx>::value_type;

  /// Whether the column can be NULL. A comparison with a NULL is neither true nor false, so no row with a NULL here
  /// satisfies `col == x` or `col != x`, or `!(col == x)` either; `is_null()` is the test for it.
  static constexpr bool NULLABLE = schema_t::template field_at<column_idx>::NULLABLE;

  /** @brief This column's index in the schema's fields. */
  static constexpr std::size_t INDEX = column_idx;

  template <std::size_t slot_idx>
  [[nodiscard]] constexpr auto operator==(ArgRef<slot_idx> /*arg*/) const {
    return compare_arg<slot_idx>(Comparison::Equal);
  }
  template <std::size_t slot_idx>
  [[nodiscard]] constexpr auto operator!=(ArgRef<slot_idx> /*arg*/) const {
    return compare_arg<slot_idx>(Comparison::NotEqual);
  }
  template <std::size_t slot_idx>
  [[nodiscard]] constexpr auto operator<(ArgRef<slot_idx> /*arg*/) const {
    return compare_arg<slot_idx>(Comparison::Less);
  }
  template <std::size_t slot_idx>
  [[nodiscard]] constexpr auto operator<=(ArgRef<slot_idx> /*arg*/) const {
    return compare_arg<slot_idx>(Comparison::LessEqual);
  }
  template <std::size_t slot_idx>
  [[nodiscard]] constexpr auto operator>(ArgRef<slot_idx> /*arg*/) const {
    return compare_arg<slot_idx>(Comparison::Greater);
  }
  template <std::size_t slot_idx>
  [[nodiscard]] constexpr auto operator>=(ArgRef<slot_idx> /*arg*/) const {
    return compare_arg<slot_idx>(Comparison::GreaterEqual);
  }

  template <typename value_t>
    requires ConstantFor<data_type, value_t>
  [[nodiscard]] constexpr auto operator==(const value_t& value) const {
    return compare_constant(Comparison::Equal, value);
  }
  template <typename value_t>
    requires ConstantFor<data_type, value_t>
  [[nodiscard]] constexpr auto operator!=(const value_t& value) const {
    return compare_constant(Comparison::NotEqual, value);
  }
  template <typename value_t>
    requires ConstantFor<data_type, value_t>
  [[nodiscard]] constexpr auto operator<(const value_t& value) const {
    return compare_constant(Comparison::Less, value);
  }
  template <typename value_t>
    requires ConstantFor<data_type, value_t>
  [[nodiscard]] constexpr auto operator<=(const value_t& value) const {
    return compare_constant(Comparison::LessEqual, value);
  }
  template <typename value_t>
    requires ConstantFor<data_type, value_t>
  [[nodiscard]] constexpr auto operator>(const value_t& value) const {
    return compare_constant(Comparison::Greater, value);
  }
  template <typename value_t>
    requires ConstantFor<data_type, value_t>
  [[nodiscard]] constexpr auto operator>=(const value_t& value) const {
    return compare_constant(Comparison::GreaterEqual, value);
  }

  /** @brief The column is NULL. Only a column declared with a `std::optional` can be. */
  [[nodiscard]] constexpr Predicate<schema_t, 1> is_null() const
    requires NULLABLE
  {
    return single(Node{.kind = NodeKind::IsNull, .column = column_idx});
  }

  /** @brief The column is not NULL. Only a column declared with a `std::optional` can be NULL. */
  [[nodiscard]] constexpr Predicate<schema_t, 1> is_not_null() const
    requires NULLABLE
  {
    return single(Node{.kind = NodeKind::IsNotNull, .column = column_idx});
  }

  /**
   * @return An ordering by this column, smallest first. A NULL sorts after every value (PostgreSQL's default), so
   * `desc()` puts NULLs first.
   */
  [[nodiscard]] constexpr Ordering<schema_t> asc() const
    requires Orderable<data_type>
  {
    return Ordering<schema_t>{OrderTerm{.column = column_idx, .descending = false}};
  }

  /** @return An ordering by this column, largest first. */
  [[nodiscard]] constexpr Ordering<schema_t> desc() const
    requires Orderable<data_type>
  {
    return Ordering<schema_t>{OrderTerm{.column = column_idx, .descending = true}};
  }

  /**
   * @brief Tests the column against the argument `slot_idx` (a `data_type`) with an operation type that names a SQL
   * `OPERATOR` or `FUNCTION`, e.g. `col.apply<json::Contains>(arg<0>())`. Usually wrapped in a named function, like
   * `json::contains` or `query::like`.
   *
   * The query layer treats it as an opaque test. Being written as a plain function call or operator, it must not depend
   * on anything but the column and its arguments.
   */
  template <typename operation_t, std::size_t slot_idx>
    requires OperationOn<operation_t>
  [[nodiscard]] constexpr Predicate<schema_t, 1> apply(ArgRef<slot_idx> /*arg*/) const {
    return single(Node{
        .kind = NodeKind::Call,
        .column = column_idx,
        .operand = Operand{.kind = OperandKind::Arg, .slot = slot_idx},
        .operation = &detail::BoundOperation<operation_t>::VALUE,
    });
  }

  /**
   * @brief As above, for a function that also takes a number (it declares `TAKES_NUMBER = true`), such as "within this
   * distance": `col.apply<WithinDistance>(arg<0>(), arg<1>())`, where argument 1 is a `double`.
   */
  template <typename operation_t, std::size_t slot_idx, std::size_t real_slot_idx>
    requires OperationWithReal<operation_t>
  [[nodiscard]] constexpr Predicate<schema_t, 1> apply(ArgRef<slot_idx> /*arg*/, ArgRef<real_slot_idx> /*real*/) const {
    return single(Node{
        .kind = NodeKind::Call,
        .column = column_idx,
        .operand = Operand{.kind = OperandKind::Arg, .slot = slot_idx},
        .operation = &detail::BoundOperation<operation_t>::VALUE,
        .extra = Operand{.kind = OperandKind::Arg, .slot = real_slot_idx},
    });
  }

  /**
   * @brief Orders rows by a module's ranking of this column against the argument `slot_idx` (a `data_type`), smallest
   * first: `query.order_by(col.ranked_by<Nearest>(arg<0>()))`, where `Nearest::OPERATOR` is `<->`. With a `limit`, it
   * is "the nearest N".
   */
  template <typename ranking_t, std::size_t slot_idx>
    requires RankingOn<ranking_t>
  [[nodiscard]] constexpr Ordering<schema_t> ranked_by(ArgRef<slot_idx> /*arg*/) const {
    return Ordering<schema_t>{
        OrderTerm{.column = column_idx, .descending = false, .ranking = &detail::BoundRanking<ranking_t>::VALUE, .slot = slot_idx},
    };
  }

  /**
   * @brief The column's value is one of the values in the list argument `slot_idx` (a `std::vector<data_type>`). Not for a
   * column that is itself an array: a list of arrays would be a two-dimensional array, which `= ANY` reads element by element.
   */
  template <std::size_t slot_idx>
  [[nodiscard]] constexpr Predicate<schema_t, 1> in(ArgRef<slot_idx> /*arg*/) const {
    static_assert(! util::SqlArray<typename schema_t::template field_at<column_idx>::value_type>,
                  "in(...) can't test a column that is itself an array (a std::vector): compare it with == instead");

    return single(Node{
        .kind = NodeKind::InList,
        .column = column_idx,
        .operand = Operand{.kind = OperandKind::Arg, .slot = slot_idx},
    });
  }

 private:
  [[nodiscard]] static constexpr Predicate<schema_t, 1> single(const Node& node) {
    return Predicate<schema_t, 1>{.nodes = {node}, .count = 1, .root = 0};
  }

  template <std::size_t slot_idx>
  [[nodiscard]] static constexpr Predicate<schema_t, 1> compare_arg(Comparison comparison) {
    return single(Node{
        .kind = NodeKind::Compare,
        .comparison = comparison,
        .column = column_idx,
        .operand = Operand{.kind = OperandKind::Arg, .slot = slot_idx},
    });
  }

  template <typename value_t>
  [[nodiscard]] static constexpr Predicate<schema_t, 1> compare_constant(Comparison comparison, const value_t& value) {
    return single(Node{
        .kind = NodeKind::Compare,
        .comparison = comparison,
        .column = column_idx,
        .operand = Operand{.kind = OperandKind::Constant, .constant = make_constant<data_type>(value)},
    });
  }
};

/**
 * @brief Entry point for writing queries against one schema.
 *
 * @code
 * using Places = query::On<PlacesSchema>;
 * constexpr auto NEARBY = Places::col<"zone">().in(query::arg<0>()) && Places::col<"id">() > 10U;
 * @endcode
 */
template <schema::IsSchema schema_t>
struct On {
  /** @return A reference to the column named `name`; an unknown name doesn't compile. */
  template <util::CTString name>
  [[nodiscard]] static constexpr auto col() {
    constexpr std::size_t IDX = schema_t::template field_index<name>();
    static_assert(IDX != schema_t::FIELD_COUNT, "query::On::col: the schema has no column with that name");

    return ColumnRef<schema_t, IDX>{};
  }
};

}  // namespace schemacht::query
