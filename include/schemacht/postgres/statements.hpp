#pragma once

#include <algorithm>
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
#include "schemacht/postgres/row_reader.hpp"
#include "schemacht/query/compiled_query.hpp"
#include "schemacht/query/raw_statement.hpp"
#include "schemacht/query/sql.hpp"
#include "schemacht/query/sql_type.hpp"
#include "schemacht/schema/schema.hpp"
#include "schemacht/util/type_utils.hpp"

/**
 * Turning bound queries and rows into what is sent to Postgres: statement text plus each argument's text. Free of libpq.
 */
namespace schemacht::postgres {

/**
 * @brief A schema whose every column's type has a text form (`HasTextForm`): its rows can be inserted and read, and any of its
 * columns' values given as a query's argument.
 */
template <typename schema_t>
concept TextSchema = schema::IsSchema<schema_t> && TextRow<typename schema_t::row_type>;

}  // namespace schemacht::postgres

namespace schemacht::postgres::detail {

/** @brief Appends a bound query argument's text, in the form Postgres reads for that type. */
class TextBinder {
 public:
  explicit TextBinder(std::vector<std::string>* texts) : _texts(texts) {}

  TextBinder(const TextBinder&) = default;
  TextBinder(TextBinder&&) noexcept = default;
  TextBinder& operator=(const TextBinder&) = default;
  TextBinder& operator=(TextBinder&&) noexcept = default;
  ~TextBinder() = default;

  void operator()(std::monostate /*unset*/) const { throw std::logic_error("query argument was never set"); }

  /** @brief A list argument (a `std::vector`, other than `bytea`'s) becomes one array parameter (`= ANY($n)`), not one per element. */
  template <HasTextForm data_t>
  void operator()(const data_t& value) const {
    _texts->push_back(format_field<data_t>(value));
  }

 private:
  std::vector<std::string>* _texts;
};

/** @return The text of each of `query`'s bound arguments, in placeholder order: element 0 is `$1`. */
template <schema::IsSchema schema_t>
[[nodiscard]] std::vector<std::string> param_texts(const query::CompiledQuery<schema_t>& query) {
  std::vector<std::string> texts;
  texts.reserve(query.params().size());
  for ( const auto& param : query.params() ) {
    std::visit(TextBinder{&texts}, param);
  }
  return texts;
}

/** @brief An `INSERT` and the text of each value it binds, in placeholder order. */
struct InsertStatement {
  std::string              sql;
  std::vector<std::string> values;
};

/** @brief The most parameters one statement can bind: the protocol counts them in 16 bits. */
inline constexpr std::size_t MAX_STATEMENT_PARAMETERS = 65535;

namespace insert_detail {

template <schema::IsSchema schema_t, std::size_t... idx>
consteval bool any_insertable_array(std::index_sequence<idx...> /*indices*/) {
  return ((! schema_t::template field_at<idx>::GENERATED && util::SqlArray<typename schema_t::template field_at<idx>::value_type>) || ...);
}

template <schema::IsSchema schema_t, std::size_t... idx>
consteval bool every_insertable_has_sql_type(std::index_sequence<idx...> /*indices*/) {
  return ((schema_t::template field_at<idx>::GENERATED || ! query::sql::type_name<typename schema_t::template field_at<idx>::value_type>().empty()) &&
          ...);
}

/**
 * @brief Whether rows of `schema_t` are inserted with `unnest`: one array argument per column. A column that is itself an
 * array can't be: `unnest` would flatten an array of arrays into its elements.
 */
template <schema::IsSchema schema_t>
inline constexpr bool USES_UNNEST = ! any_insertable_array<schema_t>(std::make_index_sequence<schema_t::FIELD_COUNT>{});

/** @brief The `unnest` INSERT for `schema_t`, as a static string. */
template <schema::IsSchema schema_t>
class UnnestSql {
  static constexpr auto CHARS = [] {
    constexpr auto TEXT = query::sql::insert_unnest_statement<schema_t>();
    return query::sql::terminated<TEXT.size()>(TEXT);
  }();

 public:
  static constexpr std::string_view VALUE{CHARS.data(), CHARS.size() - 1};
};

/** @brief `INSERT INTO <table> (<columns>) VALUES ` for `schema_t`, as a static string. */
template <schema::IsSchema schema_t>
class ValuesHeadSql {
  static constexpr auto CHARS = [] {
    constexpr auto TEXT = query::sql::insert_values_head<schema_t>();
    return query::sql::terminated<TEXT.size()>(TEXT);
  }();

 public:
  static constexpr std::string_view VALUE{CHARS.data(), CHARS.size() - 1};
};

/** @return The array literal of column `idx`'s value in every row (`NULL` for a NULL), for the `unnest` INSERT. */
template <schema::IsSchema schema_t, std::size_t idx>
[[nodiscard]] std::string column_array(std::span<const typename schema_t::row_type> rows) {
  using field_t = schema_t::template field_at<idx>;
  std::string literal = "{";
  bool        first = true;
  for ( const typename schema_t::row_type& row : rows ) {
    if ( ! first ) {
      literal += ',';
    }
    first = false;
    if ( const auto* value = schema::value_if_present(std::get<idx>(row)); value != nullptr ) {
      postgres::detail::append_array_element(literal, format_field<typename field_t::value_type>(*value));
    } else {
      literal += "NULL";
    }
  }
  literal += '}';
  return literal;
}

template <schema::IsSchema schema_t, std::size_t... idx>
[[nodiscard]] InsertStatement build_unnest(std::span<const typename schema_t::row_type> rows, std::index_sequence<idx...> /*indices*/) {
  InsertStatement statement{.sql = std::string(UnnestSql<schema_t>::VALUE), .values = {}};
  statement.values.reserve(schema_t::INSERTABLE_FIELD_COUNT);
  (
      [&] {
        if constexpr ( ! schema_t::template field_at<idx>::GENERATED ) {
          statement.values.push_back(column_array<schema_t, idx>(rows));
        }
      }(),
      ...);
  return statement;
}

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

/** @return One multi-row `INSERT ... VALUES` for `rows`, which must fit in `MAX_STATEMENT_PARAMETERS`. */
template <schema::IsSchema schema_t>
[[nodiscard]] InsertStatement build_values(std::span<const typename schema_t::row_type> rows) {
  InsertStatement statement{.sql = std::string(ValuesHeadSql<schema_t>::VALUE), .values = {}};
  for ( std::size_t row = 0; row < rows.size(); ++row ) {
    if ( row != 0 ) {
      statement.sql += ", ";
    }
    statement.sql += '(';
    append_row_values<schema_t>(rows[row], statement, std::make_index_sequence<schema_t::FIELD_COUNT>{});
    statement.sql += ')';
  }
  return statement;
}

}  // namespace insert_detail

/** @brief `CREATE TABLE` for `schema_t` (see `query::sql::create_table_statement`), as a static string. */
template <schema::IsSchema schema_t>
class CreateTableSql {
  static constexpr auto CHARS = [] {
    constexpr auto TEXT = query::sql::create_table_statement<schema_t>();
    return query::sql::terminated<TEXT.size()>(TEXT);
  }();

 public:
  static constexpr std::string_view VALUE{CHARS.data(), CHARS.size() - 1};
};

/** @brief `DROP TABLE IF EXISTS` for `schema_t`, as a static string. */
template <schema::IsSchema schema_t>
class DropTableSql {
  static constexpr auto CHARS = [] {
    constexpr auto TEXT = query::sql::drop_table_statement<schema_t>();
    return query::sql::terminated<TEXT.size()>(TEXT);
  }();

 public:
  static constexpr std::string_view VALUE{CHARS.data(), CHARS.size() - 1};
};

/**
 * @brief Builds the statements that insert `rows`, leaving out generated columns (the database computes those).
 *
 * Normally one statement, `INSERT ... SELECT * FROM unnest(...)`, with one array argument per column: any number of rows,
 * inserted atomically. A schema with an array-typed column can't use `unnest`; its rows go in multi-row `VALUES`
 * statements of at most `MAX_STATEMENT_PARAMETERS` arguments each, which are not atomic together.
 *
 * @pre `rows` isn't empty.
 */
template <TextSchema schema_t>
[[nodiscard]] std::vector<InsertStatement> build_insert(std::span<const typename schema_t::row_type> rows) {
  static_assert(schema_t::INSERTABLE_FIELD_COUNT > 0, "insert: the schema has no column that is not GENERATED");
  if constexpr ( insert_detail::USES_UNNEST<schema_t> ) {
    static_assert(insert_detail::every_insertable_has_sql_type<schema_t>(std::make_index_sequence<schema_t::FIELD_COUNT>{}),
                  "insert: every inserted column's type needs a SQL type name (it is sent as an array of that type): give it "
                  "one (ColumnType::SQL_TYPE, or a query::sql::SqlType specialization)");
    return {insert_detail::build_unnest<schema_t>(rows, std::make_index_sequence<schema_t::FIELD_COUNT>{})};
  } else {
    constexpr std::size_t        ROWS_PER_STATEMENT = MAX_STATEMENT_PARAMETERS / schema_t::INSERTABLE_FIELD_COUNT;
    std::vector<InsertStatement> statements;
    for ( std::size_t first = 0; first < rows.size(); first += ROWS_PER_STATEMENT ) {
      statements.push_back(insert_detail::build_values<schema_t>(rows.subspan(first, std::min(ROWS_PER_STATEMENT, rows.size() - first))));
    }
    return statements;
  }
}

// ---- what a backend sends: a query or a raw statement, bound ------------------------------------------------------

}  // namespace schemacht::postgres::detail

namespace schemacht::postgres {

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

namespace schemacht::postgres::detail {

/** @brief A statement as it goes to the database: its text, the text of each argument, and the columns it must return (by name, in order). */
struct WireStatement {
  std::string_view              sql;
  std::vector<std::string>      params;
  std::vector<std::string_view> column_names;
};

/**
 * @return `statement` as it goes to the database: a `CompiledQuery` (with the names of the columns it returns, from its schema), or a
 * `BoundRawStatement` (which is about no table). The text views point at static storage.
 */
template <SendableStatement statement_t>
[[nodiscard]] WireStatement wire(const statement_t& statement) {
  WireStatement wired;
  wired.sql = statement.sql();
  if constexpr ( query::IsBoundRawStatement<statement_t> ) {
    wired.params.reserve(std::tuple_size_v<typename statement_t::arguments_type>);
    std::apply([&wired](const auto&... arguments) { (TextBinder{&wired.params}(arguments), ...); }, statement.arguments());
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
 * @brief Checks that `result` has the columns `column_names`, in that order: a result whose columns are not the fields' is an error, not silently the wrong values.
 * @throws std::runtime_error if the result has rows and its number of columns is not the statement's, or a column is named otherwise (the message names both).
 */
template <NamedColumns result_t>
void check_result_columns(std::span<const std::string_view> column_names, const result_t& result) {
  const std::size_t column_count = result.column_count();
  if ( column_count != column_names.size() ) {
    if ( result.row_count() > 0 ) {  // a result without rows and without columns is the end of a stream, not a mistake
      throw std::runtime_error("the result has " + std::to_string(column_count) + " columns, and the statement returns " +
                               std::to_string(column_names.size()));
    }
    return;
  }
  for ( std::size_t column = 0; const std::string_view expected : column_names ) {
    if ( const std::string_view name = result.column_name(column++); name != expected ) {  // `column` is now its 1-based position
      throw std::runtime_error("the statement returned the column '" + std::string(name) + "' at position " + std::to_string(column) +
                               ", where its fields have '" + std::string(expected) + "' (name the select list's columns with AS)");
    }
  }
}

}  // namespace schemacht::postgres::detail
