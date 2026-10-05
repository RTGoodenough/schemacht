#pragma once

#include <concepts>
#include <cstddef>
#include <string_view>
#include <tuple>
#include <type_traits>

#include "schemacht/postgres/concepts/field_codec.hpp"
#include "schemacht/postgres/concepts/row_reader.hpp"
#include "schemacht/query/compiled_query.hpp"
#include "schemacht/query/concepts/predicate.hpp"
#include "schemacht/query/concepts/raw_statement.hpp"
#include "schemacht/schema/concepts/schema.hpp"
#include "schemacht/util/compile_time.hpp"

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

template <schema::IsSchema schema_t, typename result_t>
class SchemaStatement;  // postgres/statements.hpp

class UncheckedSql;  // postgres/statements.hpp

/**
 * @brief Whether `merge_t` says how an upsert merges a column of the row already there with the one that would have gone in
 * (see `MergeWith`): an operation (`query::OperationOn`) that names a SQL function (`FUNCTION`) or a SQL operator (`OPERATOR`),
 * and takes nothing else: no `argument_type`, and no `ARGUMENT_PREFIX` or `ARGUMENT_SUFFIX`.
 */
template <typename merge_t>
concept MergeOperation = query::OperationOn<merge_t> && ! query::DeclaresArgumentType<merge_t> && ! query::WritesAroundArgument<merge_t>;

template <util::CTString column, MergeOperation merge_t>
struct MergeWith;  // postgres/statements.hpp

template <util::CTString column>
struct Keep;  // postgres/statements.hpp

namespace detail {

template <typename statement_t>
inline constexpr bool IS_SCHEMA_STATEMENT = false;

template <typename schema_t, typename result_t>
inline constexpr bool IS_SCHEMA_STATEMENT<SchemaStatement<schema_t, result_t>> = true;

template <typename merge_t>
inline constexpr bool IS_MERGE_WITH = false;

template <util::CTString column, MergeOperation merge_t>
inline constexpr bool IS_MERGE_WITH<MergeWith<column, merge_t>> = true;

template <typename rule_t>
inline constexpr bool IS_KEEP = false;

template <util::CTString column>
inline constexpr bool IS_KEEP<Keep<column>> = true;

}  // namespace detail

/** @brief Whether `statement_t` is a statement made from a schema (`insert_statement`, `create_table_statement`, ...). */
template <typename statement_t>
concept IsSchemaStatement = detail::IS_SCHEMA_STATEMENT<statement_t>;

/** @brief Whether `statement_t` is a statement given as text while the program runs (`unchecked_sql`). */
template <typename statement_t>
concept IsUncheckedSql = std::same_as<statement_t, UncheckedSql>;

/**
 * @brief Whether `argument_t` can be an argument of `unchecked_sql`: text (a `std::string`, a `std::string_view`, a string
 * literal), or a type with a text form (`HasTextForm`). Text is tested first: asking a string literal's type for its codec is an
 * error, not a no.
 */
template <typename argument_t>
concept UncheckedSqlArgument = std::convertible_to<const argument_t&, std::string_view> || HasTextForm<argument_t>;

/** @brief Whether `merge_t` is a `MergeWith`: how an upsert sets one column of a row already there. */
template <typename merge_t>
concept IsMergeWith = detail::IS_MERGE_WITH<merge_t>;

/** @brief Whether `rule_t` is a `Keep`: a column an upsert leaves as it is in a row already there. */
template <typename rule_t>
concept IsKeep = detail::IS_KEEP<rule_t>;

/** @brief Whether `rule_t` says how an upsert treats one column of a row already there: merged (`MergeWith`) or kept (`Keep`). */
template <typename rule_t>
concept UpsertRule = IsMergeWith<rule_t> || IsKeep<rule_t>;

/**
 * @brief What a `Database` can send, with a `result_type` (a `TextRow`): a query or command (`Prepared::bind`) of a `TextSchema`, a
 * statement made from a schema (`SchemaStatement`), a bound raw statement (`RawStatement::bind`), which is about no table,
 * whose every argument has a text form (`HasTextForm`) to be sent as, or a statement given as text while the program runs
 * (`UncheckedSql`).
 */
template <typename statement_t>
concept SendableStatement =
    (query::IsBoundRawStatement<statement_t> && detail::EVERY_ARGUMENT_HAS_TEXT_FORM<typename statement_t::arguments_type> &&
     TextRow<typename statement_t::result_type>) ||
    (std::derived_from<statement_t, query::CompiledQuery<typename statement_t::schema_type>> && TextSchema<typename statement_t::schema_type>) ||
    (IsSchemaStatement<statement_t> && TextRow<typename statement_t::result_type>) || IsUncheckedSql<statement_t>;

/** @brief A statement that returns rows (`Database::execute` gives them): a query, or a raw statement that names its columns. */
template <typename statement_t>
concept RunnableStatement = SendableStatement<statement_t> && (std::tuple_size_v<typename statement_t::result_type> > 0);

/**
 * @brief A statement that returns no rows (`Database::execute` gives the count of rows it affected): a typed update or delete,
 * an insert or upsert, a raw statement with no columns, such as an `UPDATE`, a `DELETE` or a `CREATE TABLE`, or `unchecked_sql`.
 */
template <typename statement_t>
concept CommandStatement = SendableStatement<statement_t> && (std::tuple_size_v<typename statement_t::result_type> == 0);

/**
 * @brief Whether `statement_t` is a `SendableStatement`, given by reference or as a temporary: what a call taking it by forwarding
 * reference accepts.
 */
template <typename statement_t>
concept SendableStatementRef = SendableStatement<std::remove_cvref_t<statement_t>>;

/** @brief As `SendableStatementRef`, for a `RunnableStatement`. */
template <typename statement_t>
concept RunnableStatementRef = RunnableStatement<std::remove_cvref_t<statement_t>>;

/** @brief As `SendableStatementRef`, for a `CommandStatement`. */
template <typename statement_t>
concept CommandStatementRef = CommandStatement<std::remove_cvref_t<statement_t>>;

/** @brief A result whose columns can be counted and named (`QueryResult` is one). */
template <typename result_t>
concept NamedColumns = requires(const result_t& result, std::size_t column) {
  { result.row_count() } -> std::convertible_to<std::size_t>;
  { result.column_count() } -> std::convertible_to<std::size_t>;
  { result.column_name(column) } -> std::convertible_to<std::string_view>;
};

}  // namespace schemacht::postgres
