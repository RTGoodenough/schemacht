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

#include "schemacht/postgres/concepts/statements.hpp"  // IWYU pragma: export
#include "schemacht/postgres/field_codec.hpp"
#include "schemacht/query/compiled_query.hpp"
#include "schemacht/query/raw_statement.hpp"
#include "schemacht/query/sql.hpp"
#include "schemacht/query/sql_type.hpp"
#include "schemacht/schema/schema.hpp"
#include "schemacht/schema/table_name.hpp"
#include "schemacht/util/concepts/type_utils.hpp"

/**
 * Turning bound queries and rows into what is sent to Postgres: statement text plus each argument's text. Free of libpq.
 */
namespace schemacht::postgres::detail {

/** @brief Appends a bound query argument's text, in the form Postgres reads for that type. */
class TextBinder {
 public:
  explicit TextBinder(std::vector<std::string>* texts) : _texts(texts) {}

  void operator()(std::monostate /*unset*/) const { throw std::logic_error("query argument was never set"); }

  /** @brief A list argument (a `std::vector`, other than `bytea`'s) becomes one array parameter (`= ANY($n)`), not one per element. */
  template <HasTextForm data_t>
  void operator()(const data_t& value) const {
    _texts->push_back(format_field<data_t>(value));
  }

  /** @brief An operation's argument of a type of its own: formatted when it was bound. */
  void operator()(const query::ArgumentText& value) const { _texts->push_back(value.text); }

 private:
  std::vector<std::string>* _texts;

 public:
  TextBinder(const TextBinder&) = default;
  TextBinder(TextBinder&&) noexcept = default;
  TextBinder& operator=(const TextBinder&) = default;
  TextBinder& operator=(TextBinder&&) noexcept = default;
  ~TextBinder() = default;
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

/** @return `sql_t::VALUE`, naming `table` instead of the schema's own if `table` isn't null. */
template <typename sql_t>
[[nodiscard]] std::string sql_on(const schema::TableName* table) {
  return table == nullptr ? std::string(sql_t::VALUE) : query::sql::on_table(sql_t::VALUE, sql_t::TABLE_SLOT, *table);
}

namespace insert_detail {

template <schema::IsSchema schema_t, std::size_t... idx>
consteval bool any_insertable_array(std::index_sequence<idx...> /*indices*/) {
  return ((! schema_t::template field_at<idx>::GENERATED && util::SqlArray<typename schema_t::template field_at<idx>::value_type>) || ...);
}

template <schema::IsSchema schema_t, std::size_t... idx>
consteval bool every_insertable_has_sql_type(std::index_sequence<idx...> /*indices*/) {
  return (
      (schema_t::template field_at<idx>::GENERATED || ! query::sql::type_name<typename schema_t::template field_at<idx>::value_type>().empty()) && ...
  );
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
  static constexpr auto PARTS = [] {
    constexpr auto TEXT = query::sql::insert_unnest_statement<schema_t>();
    return std::pair{query::sql::terminated<TEXT.size()>(TEXT), TEXT.table_slot()};
  }();

 public:
  static constexpr std::string_view      VALUE{PARTS.first.data(), PARTS.first.size() - 1};
  static constexpr query::sql::TableSlot TABLE_SLOT = PARTS.second;  ///< Where `VALUE` names the table (see `query::sql::on_table`).
};

/** @brief `INSERT INTO <table> (<columns>) VALUES ` for `schema_t`, as a static string. */
template <schema::IsSchema schema_t>
class ValuesHeadSql {
  static constexpr auto PARTS = [] {
    constexpr auto TEXT = query::sql::insert_values_head<schema_t>();
    return std::pair{query::sql::terminated<TEXT.size()>(TEXT), TEXT.table_slot()};
  }();

 public:
  static constexpr std::string_view      VALUE{PARTS.first.data(), PARTS.first.size() - 1};
  static constexpr query::sql::TableSlot TABLE_SLOT = PARTS.second;  ///< Where `VALUE` names the table (see `query::sql::on_table`).
};

/** @brief Where the `sent`-th inserted column is in a `row_t`: an `insert_row_type` holds only those columns, a `row_type` every one. */
template <schema::IsSchema schema_t, schema::InsertRowOf<schema_t> row_t, std::size_t sent>
inline constexpr std::size_t POSITION = std::same_as<row_t, typename schema_t::insert_row_type> ? sent : schema_t::INSERTABLE_INDICES.at(sent);

/** @brief The field of the `sent`-th inserted column. */
template <schema::IsSchema schema_t, schema::InsertRowOf<schema_t> row_t, std::size_t sent>
using sent_field = std::tuple_element_t<POSITION<schema_t, row_t, sent>, row_t>;

/** @return The array literal of the `sent`-th inserted column's value in every row (`NULL` for a NULL), for the `unnest` INSERT. */
template <schema::IsSchema schema_t, schema::InsertRowOf<schema_t> row_t, std::size_t sent>
[[nodiscard]] std::string column_array(std::span<const row_t> rows) {
  using field_t = sent_field<schema_t, row_t, sent>;
  std::string literal = "{";
  bool        first = true;

  for ( const row_t& row : rows ) {
    if ( ! first ) {
      literal += ',';
    }
    first = false;

    if ( const auto* value = schema::value_if_present(std::get<POSITION<schema_t, row_t, sent>>(row)); value != nullptr ) {
      postgres::detail::append_array_element(literal, format_field<typename field_t::value_type>(*value));

    } else {
      literal += "NULL";
    }
  }

  literal += '}';

  return literal;
}

template <schema::IsSchema schema_t, schema::InsertRowOf<schema_t> row_t, std::size_t... sent>
[[nodiscard]] InsertStatement build_unnest(std::span<const row_t> rows, const schema::TableName* table, std::index_sequence<sent...> /*columns*/) {
  return InsertStatement{.sql = sql_on<UnnestSql<schema_t>>(table), .values = {column_array<schema_t, row_t, sent>(rows)...}};
}

template <schema::IsSchema schema_t, schema::InsertRowOf<schema_t> row_t, std::size_t sent>
void append_one_value(const row_t& row, InsertStatement& statement) {
  using field_t = sent_field<schema_t, row_t, sent>;
  if constexpr ( sent != 0 ) {
    statement.sql += ", ";
  }

  if ( const auto* value = schema::value_if_present(std::get<POSITION<schema_t, row_t, sent>>(row)); value != nullptr ) {
    statement.values.push_back(format_field<typename field_t::value_type>(*value));
    statement.sql += "$" + std::to_string(statement.values.size());

  } else {
    statement.sql += "NULL";  // written into the statement: it takes the column's type, and needs no parameter
  }
}

template <schema::IsSchema schema_t, schema::InsertRowOf<schema_t> row_t, std::size_t... sent>
void append_row_values(const row_t& row, InsertStatement& statement, std::index_sequence<sent...> /*columns*/) {
  (append_one_value<schema_t, row_t, sent>(row, statement), ...);
}

/** @return One multi-row `INSERT ... VALUES` for `rows`, which must fit in `MAX_STATEMENT_PARAMETERS`. */
template <schema::IsSchema schema_t, schema::InsertRowOf<schema_t> row_t>
[[nodiscard]] InsertStatement build_values(std::span<const row_t> rows, const schema::TableName* table) {
  InsertStatement statement{.sql = sql_on<ValuesHeadSql<schema_t>>(table), .values = {}};

  for ( std::size_t row = 0; row < rows.size(); ++row ) {
    if ( row != 0 ) {
      statement.sql += ", ";
    }

    statement.sql += '(';
    append_row_values<schema_t, row_t>(rows[row], statement, std::make_index_sequence<schema_t::INSERTABLE_FIELD_COUNT>{});
    statement.sql += ')';
  }

  return statement;
}

}  // namespace insert_detail

/** @brief `CREATE TABLE` for `schema_t` (see `query::sql::create_table_statement`), as a static string. */
template <schema::IsSchema schema_t>
class CreateTableSql {
  static constexpr auto PARTS = [] {
    constexpr auto TEXT = query::sql::create_table_statement<schema_t>();
    return std::pair{query::sql::terminated<TEXT.size()>(TEXT), TEXT.table_slot()};
  }();

 public:
  static constexpr std::string_view      VALUE{PARTS.first.data(), PARTS.first.size() - 1};
  static constexpr query::sql::TableSlot TABLE_SLOT = PARTS.second;  ///< Where `VALUE` names the table (see `query::sql::on_table`).
};

/** @brief ` RETURNING <every column>` for `schema_t`, as a static string: what `insert_returning` adds to each INSERT. */
template <schema::IsSchema schema_t>
class ReturningSql {
  static constexpr auto CHARS = [] {
    constexpr auto TEXT = query::sql::returning_every_column<schema_t>();
    return query::sql::terminated<TEXT.size()>(TEXT);
  }();

 public:
  static constexpr std::string_view VALUE{CHARS.data(), CHARS.size() - 1};
};

/** @brief `DROP TABLE IF EXISTS` for `schema_t`, as a static string. */
template <schema::IsSchema schema_t>
class DropTableSql {
  static constexpr auto PARTS = [] {
    constexpr auto TEXT = query::sql::drop_table_statement<schema_t>();
    return std::pair{query::sql::terminated<TEXT.size()>(TEXT), TEXT.table_slot()};
  }();

 public:
  static constexpr std::string_view      VALUE{PARTS.first.data(), PARTS.first.size() - 1};
  static constexpr query::sql::TableSlot TABLE_SLOT = PARTS.second;  ///< Where `VALUE` names the table (see `query::sql::on_table`).
};

/**
 * @brief Builds the statements that insert `rows`, leaving out generated columns (the database fills those in): from a
 * `row_type`, their values are not sent, and an `insert_row_type` has none.
 *
 * Normally one statement, `INSERT ... SELECT * FROM unnest(...)`, with one array argument per column: any number of rows,
 * inserted atomically. A schema with an array-typed column can't use `unnest`; its rows go in multi-row `VALUES`
 * statements of at most `MAX_STATEMENT_PARAMETERS` arguments each, which `StatementRunner::insert` runs in a transaction.
 *
 * @param table The table to insert into, if not the schema's own.
 * @pre `rows` isn't empty.
 */
template <TextSchema schema_t, schema::InsertRowOf<schema_t> row_t>
[[nodiscard]] std::vector<InsertStatement> build_insert(std::span<const row_t> rows, const schema::TableName* table = nullptr) {
  static_assert(schema_t::INSERTABLE_FIELD_COUNT > 0, "insert: the schema has no column that is not GENERATED");

  if constexpr ( insert_detail::USES_UNNEST<schema_t> ) {
    static_assert(
        insert_detail::every_insertable_has_sql_type<schema_t>(std::make_index_sequence<schema_t::FIELD_COUNT>{}),
        "insert: every inserted column's type needs a SQL type name (it is sent as an array of that type): give it "
        "one (ColumnType::SQL_TYPE, or a query::sql::SqlType specialization)"
    );

    return {insert_detail::build_unnest<schema_t, row_t>(rows, table, std::make_index_sequence<schema_t::INSERTABLE_FIELD_COUNT>{})};

  } else {
    constexpr std::size_t        ROWS_PER_STATEMENT = MAX_STATEMENT_PARAMETERS / schema_t::INSERTABLE_FIELD_COUNT;
    std::vector<InsertStatement> statements;
    for ( std::size_t first = 0; first < rows.size(); first += ROWS_PER_STATEMENT ) {
      statements.push_back(
          insert_detail::build_values<schema_t, row_t>(rows.subspan(first, std::min(ROWS_PER_STATEMENT, rows.size() - first)), table)
      );
    }

    return statements;
  }
}

// ---- what a backend sends: a query or a raw statement, bound ------------------------------------------------------

}  // namespace schemacht::postgres::detail

namespace schemacht::postgres::detail {

/** @brief A statement as it goes to the database: its text, the text of each argument, and the columns it must return (by name, in order). */
struct WireStatement {
  std::string_view              sql;
  std::vector<std::string>      params;
  std::vector<std::string_view> column_names;
};

/**
 * @return `statement` as it goes to the database: a `CompiledQuery` (with the names of the columns it returns, from its schema), or a
 * `BoundRawStatement` (which is about no table). The text views point at static storage, or into `statement` for a query aimed at
 * another table: `statement` must outlive the result.
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
      throw std::runtime_error(
          "the result has " + std::to_string(column_count) + " columns, and the statement returns " + std::to_string(column_names.size())
      );
    }
    return;
  }

  for ( std::size_t column = 0; const std::string_view expected : column_names ) {
    if ( const std::string_view name = result.column_name(column++); name != expected ) {  // `column` is now its 1-based position
      throw std::runtime_error(
          "the statement returned the column '" + std::string(name) + "' at position " + std::to_string(column) + ", where its fields have '" +
          std::string(expected) + "' (name the select list's columns with AS)"
      );
    }
  }
}

}  // namespace schemacht::postgres::detail
