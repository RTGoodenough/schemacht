#pragma once

#include <concepts>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "schemacht/postgres/field_codec.hpp"
#include "schemacht/postgres/sql_utils.hpp"
#include "schemacht/query/compiled_query.hpp"
#include "schemacht/query/raw_statement.hpp"
#include "schemacht/schema/schema.hpp"

/**
 * What every way of talking to Postgres has in common -- the blocking libpqxx
 * backends and the non-blocking libpq ones -- kept free of either library: the
 * text a bound argument or a written row turns into, and how the text of a
 * result field reaches a `query::Row_Sink`. A NULL is never text: a result field is `std::optional<std::string_view>`,
 * empty for NULL, and a written NULL is the SQL keyword rather than a parameter.
 */
namespace schemacht::postgres::detail {

/** @brief Appends a bound query argument's text, in the form Postgres reads for that type. */
class Text_Binder {
 public:
  explicit Text_Binder(std::vector<std::string>* texts) : _texts(texts) {}

  Text_Binder(const Text_Binder&) = default;
  Text_Binder(Text_Binder&&) noexcept = default;
  Text_Binder& operator=(const Text_Binder&) = default;
  Text_Binder& operator=(Text_Binder&&) noexcept = default;
  ~Text_Binder() = default;

  void operator()(std::monostate /*unset*/) const { throw std::logic_error("query argument was never set"); }

  template <typename data_t>
  void operator()(const data_t& value) const {
    _texts->push_back(format_field<data_t>(value));
  }

  /** @brief A list argument becomes one array parameter (`= ANY($n)`), not one parameter per element. */
  template <typename data_t>
    requires(! std::same_as<data_t, std::byte>)  // a `std::vector<std::byte>` is a bytea value, not a list
  void operator()(const std::vector<data_t>& values) const {
    _texts->push_back(format_array<data_t>(values));
  }

 private:
  std::vector<std::string>* _texts;
};

/** @return The text of each of `query`'s bound arguments, in placeholder order: element 0 is `$1`. */
template <schema::IsSchema schema_t>
[[nodiscard]] std::vector<std::string> param_texts(const query::Compiled_Query<schema_t>& query) {
  std::vector<std::string> texts;
  texts.reserve(query.params().size());
  for ( const auto& param : query.params() ) {
    std::visit(Text_Binder{&texts}, param);
  }
  return texts;
}

/** @brief An `INSERT` and the text of each value it binds, in placeholder order. */
struct InsertStatement {
  std::string              sql;
  std::vector<std::string> values;
};

namespace insert_detail {

template <schema::IsSchema schema_t, std::size_t idx>
void append_one_value(const typename schema_t::row_type& row, InsertStatement& statement, bool& first) {
  using field_t = schema_t::template field_at<idx>;
  if constexpr ( ! field_t::GENERATED ) {
    if ( ! first ) {
      statement.sql += ", ";
    }
    first = false;
    if ( const auto* value = schema::value_if_present(std::get<idx>(row)); value != nullptr ) {
      statement.values.push_back(format_field<typename field_t::value_type>(*value));
      statement.sql += "$" + std::to_string(statement.values.size());
    } else {
      statement.sql += "NULL";  // written into the statement: it takes the column's type, and needs no parameter
    }
  }
}

template <schema::IsSchema schema_t, std::size_t... idx>
void append_row_values(const typename schema_t::row_type& row, InsertStatement& statement, std::index_sequence<idx...> /*indices*/) {
  bool first = true;
  (append_one_value<schema_t, idx>(row, statement, first), ...);
}

}  // namespace insert_detail

/**
 * @brief Builds one multi-row `INSERT` for `rows`, skipping generated columns (which the database computes itself).
 * @pre `rows` isn't empty.
 */
template <schema::IsSchema schema_t>
[[nodiscard]] InsertStatement build_insert(std::span<const typename schema_t::row_type> rows) {
  InsertStatement statement;
  statement.sql = "INSERT INTO " + quote_identifier(std::string(schema_t::TABLE_NAME)) + " (" +
                  quote_identifier_list(schema_t::insertable_column_names()) + ") VALUES ";
  bool first_row = true;
  for ( const auto& row : rows ) {
    if ( ! first_row ) {
      statement.sql += ", ";
    }
    first_row = false;
    statement.sql += '(';
    insert_detail::append_row_values<schema_t>(row, statement, std::make_index_sequence<schema_t::FIELD_COUNT>{});
    statement.sql += ')';
  }
  return statement;
}

// ---- what a backend sends: a query or a raw statement, bound ------------------------------------------------------

/** @brief A statement as it goes to the database: its text, the text of each argument, and the columns it must return (by name, in order). */
struct WireStatement {
  std::string_view              sql;
  std::vector<std::string>      params;
  std::vector<std::string_view> column_names;
};

/**
 * @return `statement` as it goes to the database: a `Compiled_Query` (with the names of the columns it returns, from its schema), or a
 * `Bound_Raw_Statement` (which is about no table). The text views point at static storage.
 */
template <typename statement_t>
[[nodiscard]] WireStatement wire(const statement_t& statement) {
  WireStatement wired;
  wired.sql = statement.sql();
  if constexpr ( query::IsBoundRawStatement<statement_t> ) {
    wired.params.reserve(std::tuple_size_v<typename statement_t::arguments_type>);
    std::apply([&wired](const auto&... arguments) { (Text_Binder{&wired.params}(arguments), ...); }, statement.arguments());
    wired.column_names.assign(statement.column_names().begin(), statement.column_names().end());
  } else {
    using schema_t = statement_t::schema_type;
    wired.params = param_texts<schema_t>(statement);
    const auto names = schema_t::column_names();
    wired.column_names.reserve(statement.columns().size());
    for ( const std::size_t column : statement.columns() ) {
      wired.column_names.push_back(names.at(column));
    }
  }
  return wired;
}

/**
 * @brief Checks that a result has the columns `statement` names, in that order: a result whose columns are not the fields' is an error, not silently the wrong values.
 * @param name_at The name of the result's column at a position.
 * @throws std::runtime_error if the result has rows and its number of columns is not the statement's, or a column is named otherwise (the message names both).
 */
template <typename name_at_t>
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- a row count and a column count, both counts of the same result
void check_result_columns(const WireStatement& statement, std::size_t row_count, std::size_t column_count, name_at_t name_at) {
  if ( column_count != statement.column_names.size() ) {
    if ( row_count > 0 ) {  // a result without rows and without columns is the end of a stream, not a mistake
      throw std::runtime_error("the result has " + std::to_string(column_count) + " columns, and the statement returns " +
                               std::to_string(statement.column_names.size()));
    }
    return;
  }
  for ( std::size_t column = 0; column < column_count; ++column ) {
    const std::string_view expected = statement.column_names.at(column);
    if ( name_at(column) != expected ) {
      throw std::runtime_error("the statement returned the column '" + std::string(name_at(column)) + "' at position " + std::to_string(column + 1) +
                               ", where its fields have '" + std::string(expected) + "' (name the select list's columns with AS)");
    }
  }
}

}  // namespace schemacht::postgres::detail

namespace schemacht::postgres {

/** @brief What a `Database` runs: a query (`Prepared::bind`) of any schema, or a bound raw statement (`Raw_Statement::bind`), which is about no table. Either has a `result_type`. */
template <typename statement_t>
concept RunnableStatement =
    query::IsBoundRawStatement<statement_t> || std::derived_from<statement_t, query::Compiled_Query<typename statement_t::schema_type>>;

}  // namespace schemacht::postgres
