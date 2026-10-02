#pragma once

#include <concepts>
#include <cstddef>
#include <string_view>
#include <tuple>

#include "schemacht/postgres/concepts/field_codec.hpp"
#include "schemacht/postgres/concepts/row_reader.hpp"
#include "schemacht/query/compiled_query.hpp"
#include "schemacht/query/concepts/raw_statement.hpp"
#include "schemacht/schema/concepts/schema.hpp"

namespace schemacht::postgres {

/**
 * @brief A schema whose every column's type has a text form (`HasTextForm`): its rows can be inserted and read, and any of its
 * columns' values given as a query's argument.
 */
template <typename schema_t>
concept TextSchema = schema::IsSchema<schema_t> && TextRow<typename schema_t::row_type>;

namespace detail {

template <typename arguments_t>
inline constexpr bool EVERY_ARGUMENT_HAS_TEXT_FORM = false;

template <typename... argument_ts>
inline constexpr bool EVERY_ARGUMENT_HAS_TEXT_FORM<std::tuple<argument_ts...>> = (HasTextForm<argument_ts> && ...);

}  // namespace detail

/**
 * @brief What a `Database` can send, with a `result_type` (a `TextRow`): a query or command (`Prepared::bind`) of a `TextSchema`, or a
 * bound raw statement (`RawStatement::bind`), which is about no table, whose every argument has a text form (`HasTextForm`) to be sent as.
 */
template <typename statement_t>
concept SendableStatement =
    (query::IsBoundRawStatement<statement_t> && detail::EVERY_ARGUMENT_HAS_TEXT_FORM<typename statement_t::arguments_type> &&
     TextRow<typename statement_t::result_type>) ||
    (std::derived_from<statement_t, query::CompiledQuery<typename statement_t::schema_type>> && TextSchema<typename statement_t::schema_type>);

/** @brief A statement that returns rows (`Database::execute` gives them): a query, or a raw statement that names its columns. */
template <typename statement_t>
concept RunnableStatement = SendableStatement<statement_t> && (std::tuple_size_v<typename statement_t::result_type> > 0);

/**
 * @brief A statement that returns no rows (`Database::execute` gives the count of rows it affected): a typed update or delete,
 * or a raw statement with no columns, such as an `UPDATE`, a `DELETE` or a `CREATE TABLE`.
 */
template <typename statement_t>
concept CommandStatement = SendableStatement<statement_t> && (std::tuple_size_v<typename statement_t::result_type> == 0);

/** @brief A result whose columns can be counted and named (`QueryResult` is one). */
template <typename result_t>
concept NamedColumns = requires(const result_t& result, std::size_t column) {
  { result.row_count() } -> std::convertible_to<std::size_t>;
  { result.column_count() } -> std::convertible_to<std::size_t>;
  { result.column_name(column) } -> std::convertible_to<std::string_view>;
};

}  // namespace schemacht::postgres
