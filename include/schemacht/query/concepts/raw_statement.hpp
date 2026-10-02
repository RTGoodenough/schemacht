#pragma once

#include <concepts>
#include <type_traits>

#include "schemacht/schema/concepts/field.hpp"
#include "schemacht/util/compile_time.hpp"

namespace schemacht::query {

/**
 * @brief A type a raw statement's argument can have: a value type (not a reference, not `const`), which the bound statement
 * holds. Whether a backend can send it (`postgres::HasTextForm`) is checked where the statement is run.
 */
template <typename data_t>
concept RawArgument = std::same_as<data_t, std::remove_cvref_t<data_t>> && std::move_constructible<data_t>;

template <RawArgument... argument_ts>
struct RawArguments;  // query/raw_statement.hpp

namespace detail {

template <typename type_t>
struct is_raw_arguments : std::false_type {};

template <typename... argument_ts>
struct is_raw_arguments<RawArguments<argument_ts...>> : std::true_type {};

}  // namespace detail

/** @brief Detects whether `type_t` is a `RawArguments<...>`: the argument list of a `RawStatement`. */
template <typename type_t>
concept IsRawArguments = detail::is_raw_arguments<type_t>::value;

template <util::CTString sql_text, IsRawArguments arguments_t, schema::IsField... field_ts>
class RawStatement;  // query/raw_statement.hpp

template <typename statement_t>
class BoundRawStatement;  // query/raw_statement.hpp

namespace detail {

template <typename type_t>
struct is_raw_statement : std::false_type {};

template <util::CTString sql_text, IsRawArguments arguments_t, schema::IsField... field_ts>
struct is_raw_statement<RawStatement<sql_text, arguments_t, field_ts...>> : std::true_type {};

template <typename type_t>
struct is_bound_raw_statement : std::false_type {};

template <typename statement_t>
struct is_bound_raw_statement<BoundRawStatement<statement_t>> : std::true_type {};

}  // namespace detail

/** @brief Detects whether `type_t` is a `RawStatement<...>` specialization. */
template <typename type_t>
concept IsRawStatement = detail::is_raw_statement<type_t>::value;

/** @brief Detects whether `type_t` is a `BoundRawStatement<...>`: a raw statement with its arguments. */
template <typename type_t>
concept IsBoundRawStatement = detail::is_bound_raw_statement<type_t>::value;

}  // namespace schemacht::query
