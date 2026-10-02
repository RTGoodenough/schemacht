#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

#include "schemacht/schema/concepts/schema.hpp"

namespace schemacht::query {

template <schema::IsSchema schema_t, std::size_t capacity>
struct Predicate;  // query/predicate.hpp

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

template <typename data_t>
struct is_predicate : std::false_type {};

template <schema::IsSchema schema_t, std::size_t capacity>
struct is_predicate<Predicate<schema_t, capacity>> : std::true_type {};

}  // namespace detail

/** @brief Detects whether `data_t` is a `Predicate<...>` specialization. */
template <typename data_t>
concept IsPredicate = detail::is_predicate<data_t>::value;

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

/** @brief Whether rows can be sorted by a column of `data_t`. */
template <typename data_t>
concept Orderable = std::totally_ordered<data_t>;

}  // namespace schemacht::query
