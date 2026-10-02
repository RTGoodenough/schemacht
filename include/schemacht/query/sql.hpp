#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "schemacht/query/command.hpp"
#include "schemacht/query/predicate.hpp"
#include "schemacht/query/query.hpp"
#include "schemacht/query/sql_type.hpp"
#include "schemacht/schema/schema.hpp"
#include "schemacht/util/identifier.hpp"

/**
 * Lowering a `Query` to a PostgreSQL statement, entirely at compile time. Every value that isn't a literal is a `$n`
 * placeholder, so the text is a constant.
 *
 * Everything here is `consteval`: it can only run while compiling, so a mistake is a compile error, never a runtime
 * exception. Names and operators are validated where they are declared (`schema::Field`, `schema::Schema`,
 * `query::detail::BoundOperation`); the checks below only guard that invariant.
 */
namespace schemacht::query::sql {

namespace detail {

// Not constexpr on purpose: reaching a call in a `consteval` function is a compile error that names the function.
inline void sql_identifier_not_validated() {}
inline void sql_spelling_not_validated() {}
inline void sql_statement_capacity_exceeded() {}

}  // namespace detail

/** @brief A fixed-capacity string built while compiling. */
template <std::size_t capacity>
class Text {
 public:
  consteval void push(char chr) {
    if ( _size == capacity ) {
      detail::sql_statement_capacity_exceeded();
    }
    _chars.at(_size++) = chr;
  }

  consteval void append(std::string_view part) {
    for ( const char chr : part ) {
      push(chr);
    }
  }

  consteval void append_unsigned(std::uint64_t value) {
    std::array<char, DECIMAL_DIGITS> digits{};
    std::size_t                      count = 0;
    for ( bool more = true; more; more = value != 0 ) {  // at least one digit, so 0 is "0"
      digits.at(count++) = static_cast<char>('0' + (value % DECIMAL_BASE));
      value /= DECIMAL_BASE;
    }
    while ( count > 0 ) {
      push(digits.at(--count));
    }
  }

  consteval void append_signed(std::int64_t value) {
    if ( value < 0 ) {
      push('-');
      append_unsigned(std::uint64_t{0} - std::bit_cast<std::uint64_t>(value));
    } else {
      append_unsigned(static_cast<std::uint64_t>(value));
    }
  }

  /** @brief Appends `name` double-quoted. `name` is a table or column name, already checked by `util::is_identifier`. */
  consteval void append_identifier(std::string_view name) {
    if ( ! util::is_identifier(name) ) {
      detail::sql_identifier_not_validated();
    }
    push('"');
    append(name);
    push('"');
  }

  /** @brief Appends a SQL function name, already checked by `util::is_sql_function_name`. */
  consteval void append_function(std::string_view name) {
    if ( ! util::is_sql_function_name(name) ) {
      detail::sql_spelling_not_validated();
    }
    append(name);
  }

  /** @brief Appends a SQL operator such as `<->`, already checked by `util::is_sql_operator`. */
  consteval void append_operator(std::string_view name) {
    if ( ! util::is_sql_operator(name) ) {
      detail::sql_spelling_not_validated();
    }
    append(name);
  }

  /** @brief Appends `names`, each double-quoted, separated by `, `. */
  template <std::size_t count>
  consteval void append_identifier_list(const std::array<std::string_view, count>& names) {
    for ( std::size_t i = 0; i < count; ++i ) {
      if ( i != 0 ) {
        append(", ");
      }
      append_identifier(names.at(i));
    }
  }

  [[nodiscard]] consteval std::size_t size() const { return _size; }
  [[nodiscard]] consteval char        at(std::size_t index) const { return _chars.at(index); }

 private:
  static constexpr std::uint64_t DECIMAL_BASE = 10;
  static constexpr std::size_t   DECIMAL_DIGITS = 20;  // the most digits a uint64_t has

  std::array<char, capacity> _chars{};
  std::size_t                _size = 0;
};

namespace detail {

template <schema::IsSchema schema_t, std::size_t... idx>
consteval std::array<std::string_view, schema_t::FIELD_COUNT> type_names(std::index_sequence<idx...> /*indices*/) {
  return {type_name<typename schema_t::template field_at<idx>::value_type>()...};
}

template <schema::IsSchema schema_t, std::size_t... idx>
consteval std::array<std::string_view, schema_t::FIELD_COUNT> select_functions(std::index_sequence<idx...> /*indices*/) {
  return {SelectExpression<typename schema_t::template field_at<idx>::value_type>::FUNCTION...};
}

template <schema::IsSchema schema_t, std::size_t... idx>
consteval std::array<bool, schema_t::FIELD_COUNT> generated_flags(std::index_sequence<idx...> /*indices*/) {
  return {schema_t::template field_at<idx>::GENERATED...};
}

template <schema::IsSchema schema_t, std::size_t... idx>
consteval std::array<bool, schema_t::FIELD_COUNT> nullable_flags(std::index_sequence<idx...> /*indices*/) {
  return {schema_t::template field_at<idx>::NULLABLE...};
}

/** @brief Whether each column of `schema_t` can be NULL (its field is a `std::optional`). */
template <schema::IsSchema schema_t>
inline constexpr std::array<bool, schema_t::FIELD_COUNT> COLUMN_NULLABLE =
    nullable_flags<schema_t>(std::make_index_sequence<schema_t::FIELD_COUNT>{});

/** @brief Whether each column of `schema_t` is `GENERATED` (computed by the database, never inserted). */
template <schema::IsSchema schema_t>
inline constexpr std::array<bool, schema_t::FIELD_COUNT> COLUMN_GENERATED =
    generated_flags<schema_t>(std::make_index_sequence<schema_t::FIELD_COUNT>{});

/** @brief The SQL type name of each column of `schema_t` (empty where it has none), in declaration order. */
template <schema::IsSchema schema_t>
inline constexpr std::array<std::string_view, schema_t::FIELD_COUNT> COLUMN_TYPE_NAMES =
    type_names<schema_t>(std::make_index_sequence<schema_t::FIELD_COUNT>{});

/** @brief The `SelectExpression::FUNCTION` of each column of `schema_t` (empty where it is selected as it is). */
template <schema::IsSchema schema_t>
inline constexpr std::array<std::string_view, schema_t::FIELD_COUNT> COLUMN_SELECT_FUNCTIONS =
    select_functions<schema_t>(std::make_index_sequence<schema_t::FIELD_COUNT>{});

template <schema::IsSchema schema_t>
[[nodiscard]] consteval std::string_view column_type_name(std::size_t column) {
  return COLUMN_TYPE_NAMES<schema_t>.at(column);
}

consteval std::string_view comparison_symbol(Comparison comparison) {
  switch ( comparison ) {
    case Comparison::Equal:
      return "=";
    case Comparison::NotEqual:
      return "<>";
    case Comparison::Less:
      return "<";
    case Comparison::LessEqual:
      return "<=";
    case Comparison::Greater:
      return ">";
    case Comparison::GreaterEqual:
      return ">=";
  }
  return "";  // unreachable: every Comparison is handled above
}

/** @brief Writes an operand: `$n` for a bound argument (`arg<N>()` is `$N+1`), or the literal itself. */
template <std::size_t text_capacity>
consteval void append_operand(Text<text_capacity>& text, const Operand& value) {
  if ( value.kind == OperandKind::Arg ) {
    text.push('$');
    text.append_unsigned(value.slot + 1);
    return;
  }
  switch ( value.constant.kind ) {
    case ConstantKind::Bool:
      text.append(value.constant.bits != 0 ? "TRUE" : "FALSE");
      return;
    case ConstantKind::Signed:
      text.append_signed(std::bit_cast<std::int64_t>(value.constant.bits));
      return;
    case ConstantKind::Unsigned:
      text.append_unsigned(value.constant.bits);
      return;
  }
}

/** @brief Writes a predicate tree as a SQL boolean expression, as it was written. */
template <schema::IsSchema schema_t, std::size_t predicate_capacity, std::size_t text_capacity>
class Emitter {
 public:
  consteval Emitter(const Predicate<schema_t, predicate_capacity>* predicate, Text<text_capacity>* text) : _predicate(predicate), _text(text) {}

  Emitter(const Emitter&) = delete;
  Emitter(Emitter&&) = delete;
  Emitter& operator=(const Emitter&) = delete;
  Emitter& operator=(Emitter&&) = delete;
  constexpr ~Emitter() = default;

  consteval void node(std::size_t index) { write(index, ANY_LEVEL); }

 private:
  // How tightly an expression binds, by the operator around it: an `OR` inside an `AND` needs parentheses, and nothing
  // else does (`AND` and `OR` are associative, and `NOT` parenthesizes what it negates).
  static constexpr std::size_t ANY_LEVEL = 0;
  static constexpr std::size_t OR_LEVEL = 1;
  static constexpr std::size_t AND_LEVEL = 2;

  /** @param context The level of the junction this expression is an operand of. */
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- a node index and a precedence level, in that order at every call.
  consteval void write(std::size_t index, std::size_t context) {
    const Node& current = _predicate->nodes.at(index);
    switch ( current.kind ) {
      case NodeKind::True:
        _text->append("TRUE");
        return;
      case NodeKind::False:
        _text->append("FALSE");
        return;
      case NodeKind::Compare:
        _text->append_identifier(schema_t::column_names().at(current.column));
        _text->push(' ');
        _text->append(comparison_symbol(current.comparison));
        _text->push(' ');
        operand(current.operand);
        return;
      case NodeKind::InList:
        _text->append_identifier(schema_t::column_names().at(current.column));
        _text->append(" = ANY(");
        operand(current.operand);
        cast_array(column_type_name<schema_t>(current.column));
        _text->push(')');
        return;
      case NodeKind::Call:
        call(current);
        return;
      case NodeKind::IsNull:
        _text->append_identifier(schema_t::column_names().at(current.column));
        _text->append(" IS NULL");
        return;
      case NodeKind::IsNotNull:
        _text->append_identifier(schema_t::column_names().at(current.column));
        _text->append(" IS NOT NULL");
        return;
      case NodeKind::Not:
        _text->append("NOT (");
        write(current.lhs, ANY_LEVEL);
        _text->push(')');
        return;
      case NodeKind::And:
        junction(current, " AND ", AND_LEVEL, context);
        return;
      case NodeKind::Or:
        junction(current, " OR ", OR_LEVEL, context);
        return;
    }
  }

  consteval void junction(const Node& current, std::string_view word, std::size_t level, std::size_t context) {
    const bool wrap = level < context;
    if ( wrap ) {
      _text->push('(');
    }
    write(current.lhs, level);
    _text->append(word);
    write(current.rhs, level);
    if ( wrap ) {
      _text->push(')');
    }
  }

  /** @brief `function("column", $n::type[, $m::double precision])`, or `"column" operator $n::type` (the argument written inside the operation's `ARGUMENT_PREFIX` and `ARGUMENT_SUFFIX`, if it has them). */
  consteval void call(const Node& current) {
    const std::string_view type = column_type_name<schema_t>(current.column);
    if ( current.operation->infix ) {
      _text->append_identifier(schema_t::column_names().at(current.column));
      _text->push(' ');
      _text->append_operator(current.operation->name);
      _text->push(' ');
      argument(current, type);
      return;
    }
    _text->append_function(current.operation->name);
    _text->push('(');
    _text->append_identifier(schema_t::column_names().at(current.column));
    _text->append(", ");
    argument(current, type);
    if ( current.operation->takes_real ) {
      _text->append(", ");
      operand(current.extra);
      cast(type_name<double>());
    }
    _text->push(')');
  }

  /** @brief The call's argument as the operation writes it: `$n::type`, inside the SQL the operation puts around it (if any). */
  consteval void argument(const Node& current, std::string_view type) {
    _text->append(current.operation->argument_prefix);
    operand(current.operand);
    cast(type);
    _text->append(current.operation->argument_suffix);
  }

  consteval void cast(std::string_view type) {
    if ( ! type.empty() ) {
      _text->append("::");
      _text->append(type);
    }
  }

  consteval void cast_array(std::string_view type) {
    if ( ! type.empty() ) {
      cast(type);
      _text->append("[]");
    }
  }

  consteval void operand(const Operand& value) { append_operand(*_text, value); }

  const Predicate<schema_t, predicate_capacity>* _predicate;
  Text<text_capacity>*                           _text;
};

}  // namespace detail

/** @brief Room for the longest statement `select_statement` can produce; overflowing it is a compile error (`sql_statement_capacity_exceeded`). */
inline constexpr std::size_t STATEMENT_CAPACITY = 4096;

/**
 * @brief Writes `SELECT <the first count of columns> FROM <schema_t's table>`. A column whose type has a `SelectExpression`
 * is written as that function of it (`ST_AsBinary("rast") AS "rast"`).
 */
template <schema::IsSchema schema_t, std::size_t text_capacity, std::size_t columns_capacity>
consteval void append_select_from(Text<text_capacity>& text, const std::array<std::size_t, columns_capacity>& columns, std::size_t count) {
  text.append("SELECT ");
  for ( std::size_t i = 0; i < count; ++i ) {
    if ( i != 0 ) {
      text.append(", ");
    }
    const std::string_view name = schema_t::column_names().at(columns.at(i));
    const std::string_view function = detail::COLUMN_SELECT_FUNCTIONS<schema_t>.at(columns.at(i));
    if ( function.empty() ) {
      text.append_identifier(name);
    } else {
      text.append_function(function);
      text.push('(');
      text.append_identifier(name);
      text.append(") AS ");
      text.append_identifier(name);
    }
  }
  text.append(" FROM ");
  text.append_identifier(schema_t::TABLE_NAME);
}

/**
 * @return `SELECT <columns> FROM <table> [WHERE <predicate>] [ORDER BY <terms>] [LIMIT <count>]`,
 * with each argument as `$n` (`arg<N>()` is `$N+1`).
 */
template <schema::IsSchema schema_t, std::size_t predicate_capacity, std::size_t order_capacity>
[[nodiscard]] consteval Text<STATEMENT_CAPACITY> select_statement(const Query<schema_t, predicate_capacity, order_capacity>& query) {
  Text<STATEMENT_CAPACITY> text;
  append_select_from<schema_t>(text, query.result_columns(), query.result_column_count());

  if ( query.predicate.nodes.at(query.predicate.root).kind != NodeKind::True ) {
    text.append(" WHERE ");
    detail::Emitter<schema_t, predicate_capacity, STATEMENT_CAPACITY>(&query.predicate, &text).node(query.predicate.root);
  }

  for ( std::size_t i = 0; i < query.order_count; ++i ) {
    text.append(i == 0 ? " ORDER BY " : ", ");
    const OrderTerm& term = query.order.at(i);
    text.append_identifier(schema_t::column_names().at(term.column));
    if ( term.ranking != nullptr ) {
      text.push(' ');
      text.append_operator(term.ranking->op);
      text.append(" $");
      text.append_unsigned(term.slot + 1);
      if ( const std::string_view type = detail::column_type_name<schema_t>(term.column); ! type.empty() ) {
        text.append("::");
        text.append(type);
      }
    }
    if ( term.descending ) {
      text.append(" DESC");
    }
  }

  switch ( query.row_limit.kind ) {
    case LimitKind::None:
      break;
    case LimitKind::Constant:
      text.append(" LIMIT ");
      text.append_signed(query.row_limit.count);
      break;
    case LimitKind::Arg:
      text.append(" LIMIT $");
      text.append_unsigned(query.row_limit.slot + 1);
      break;
  }
  return text;
}

/** @brief Writes ` WHERE <predicate>`, or nothing for a predicate that matches every row. */
template <schema::IsSchema schema_t, std::size_t text_capacity, std::size_t predicate_capacity>
consteval void append_where(Text<text_capacity>& text, const Predicate<schema_t, predicate_capacity>& predicate) {
  if ( predicate.count != 0 && predicate.nodes.at(predicate.root).kind != NodeKind::True ) {
    text.append(" WHERE ");
    detail::Emitter<schema_t, predicate_capacity, text_capacity>(&predicate, &text).node(predicate.root);
  }
}

/** @return `UPDATE <table> SET <column> = <value>, ... [WHERE <predicate>]`, each value `$n`, a literal, or `NULL`. */
template <schema::IsSchema schema_t, std::size_t set_capacity, std::size_t predicate_capacity>
[[nodiscard]] consteval Text<STATEMENT_CAPACITY> update_statement(const Update<schema_t, set_capacity, predicate_capacity>& update) {
  Text<STATEMENT_CAPACITY> text;
  text.append("UPDATE ");
  text.append_identifier(schema_t::TABLE_NAME);
  for ( std::size_t i = 0; i < update.set_count; ++i ) {
    const SetTerm& term = update.sets.at(i);
    text.append(i == 0 ? " SET " : ", ");
    text.append_identifier(schema_t::column_names().at(term.column));
    text.append(" = ");
    if ( term.is_null ) {
      text.append("NULL");
    } else {
      detail::append_operand(text, term.value);
    }
  }
  append_where(text, update.predicate);
  return text;
}

/** @return `DELETE FROM <table> [WHERE <predicate>]`. */
template <schema::IsSchema schema_t, std::size_t predicate_capacity>
[[nodiscard]] consteval Text<STATEMENT_CAPACITY> delete_statement(const Delete<schema_t, predicate_capacity>& del) {
  Text<STATEMENT_CAPACITY> text;
  text.append("DELETE FROM ");
  text.append_identifier(schema_t::TABLE_NAME);
  append_where(text, del.predicate);
  return text;
}

/**
 * @return `CREATE TABLE <table> (<column> <type> [NOT NULL], ..., PRIMARY KEY (<key columns>), UNIQUE (<column>), ...)`: each
 * column with its type's SQL name, `NOT NULL` unless its field is a `std::optional`, the primary key if the schema has one,
 * and a `UNIQUE` constraint for each `KeyRole::Unique` column on its own.
 */
template <schema::IsSchema schema_t>
[[nodiscard]] consteval Text<STATEMENT_CAPACITY> create_table_statement() {
  static_assert(! std::ranges::any_of(detail::COLUMN_TYPE_NAMES<schema_t>, &std::string_view::empty),
                "create_table: a column's type has no SQL name; give it one (ColumnType::SQL_TYPE, or a query::sql::SqlType specialization)");
  static_assert(! std::ranges::any_of(detail::COLUMN_GENERATED<schema_t>, std::identity{}),
                "create_table can't write a generated column's expression: create a table with one yourself");
  Text<STATEMENT_CAPACITY> text;
  text.append("CREATE TABLE ");
  text.append_identifier(schema_t::TABLE_NAME);
  text.append(" (");
  for ( std::size_t column = 0; column < schema_t::FIELD_COUNT; ++column ) {
    if ( column != 0 ) {
      text.append(", ");
    }
    text.append_identifier(schema_t::column_names().at(column));
    text.push(' ');
    text.append(detail::COLUMN_TYPE_NAMES<schema_t>.at(column));
    if ( ! detail::COLUMN_NULLABLE<schema_t>.at(column) ) {
      text.append(" NOT NULL");
    }
  }
  if constexpr ( schema_t::HAS_PRIMARY_KEY ) {
    text.append(", PRIMARY KEY (");
    text.append_identifier_list(schema_t::primary_key_column_names());
    text.push(')');
  }
  for ( const std::string_view column : schema_t::unique_column_names() ) {  // each unique on its own (see `KeyRole`)
    text.append(", UNIQUE (");
    text.append_identifier(column);
    text.push(')');
  }
  text.push(')');
  return text;
}

/** @return `DROP TABLE IF EXISTS <table>`. */
template <schema::IsSchema schema_t>
[[nodiscard]] consteval Text<STATEMENT_CAPACITY> drop_table_statement() {
  Text<STATEMENT_CAPACITY> text;
  text.append("DROP TABLE IF EXISTS ");
  text.append_identifier(schema_t::TABLE_NAME);
  return text;
}

/** @brief Writes `INSERT INTO <table> (<every insertable column>)`. */
template <schema::IsSchema schema_t, std::size_t text_capacity>
consteval void append_insert_into(Text<text_capacity>& text) {
  text.append("INSERT INTO ");
  text.append_identifier(schema_t::TABLE_NAME);
  text.append(" (");
  text.append_identifier_list(schema_t::insertable_column_names());
  text.push(')');
}

/**
 * @return `INSERT INTO <table> (<columns>) SELECT * FROM unnest($1::<type>[], $2::<type>[], ...)`: one array argument per
 * insertable column, holding that column's value for every row. Every insertable column's type needs an SQL type name (`sql::type_name`).
 */
template <schema::IsSchema schema_t>
[[nodiscard]] consteval Text<STATEMENT_CAPACITY> insert_unnest_statement() {
  Text<STATEMENT_CAPACITY> text;
  append_insert_into<schema_t>(text);
  text.append(" SELECT * FROM unnest(");
  std::uint64_t placeholder = 0;
  for ( std::size_t column = 0; column < schema_t::FIELD_COUNT; ++column ) {
    if ( detail::COLUMN_GENERATED<schema_t>.at(column) ) {
      continue;
    }
    if ( placeholder != 0 ) {
      text.append(", ");
    }
    text.push('$');
    text.append_unsigned(++placeholder);
    text.append("::");
    text.append(detail::COLUMN_TYPE_NAMES<schema_t>.at(column));
    text.append("[]");
  }
  text.push(')');
  return text;
}

/** @return `INSERT INTO <table> (<columns>) VALUES `, to which the rows are appended. */
template <schema::IsSchema schema_t>
[[nodiscard]] consteval Text<STATEMENT_CAPACITY> insert_values_head() {
  Text<STATEMENT_CAPACITY> text;
  append_insert_into<schema_t>(text);
  text.append(" VALUES ");
  return text;
}

/** @return `text`'s contents as a null-terminated array of exactly `length + 1` characters. */
template <std::size_t length, std::size_t capacity>
[[nodiscard]] consteval std::array<char, length + 1> terminated(const Text<capacity>& text) {
  std::array<char, length + 1> result{};
  for ( std::size_t i = 0; i < length; ++i ) {
    result.at(i) = text.at(i);
  }
  return result;
}

}  // namespace schemacht::query::sql
