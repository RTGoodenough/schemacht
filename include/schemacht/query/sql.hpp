#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "schemacht/query/command.hpp"
#include "schemacht/query/predicate.hpp"
#include "schemacht/query/query.hpp"
#include "schemacht/query/sql_type.hpp"
#include "schemacht/schema/field.hpp"
#include "schemacht/schema/schema.hpp"
#include "schemacht/schema/table_name.hpp"
#include "schemacht/util/identifier.hpp"

/**
 * @file
 * Lowering a `Query` to a PostgreSQL statement, entirely at compile time. Every value that isn't a literal is a `$n`
 * placeholder, so the text is a constant.
 *
 * Everything here but `on_table` is `consteval`: it can only run while compiling, so a mistake is a compile error, never a
 * runtime exception. Names and operators are validated where they are declared (`schema::Field`, `schema::Schema`,
 * `query::detail::BoundOperation`); the checks below only guard that invariant.
 */
namespace schemacht::query::sql {

namespace detail {

// Not constexpr on purpose: reaching a call in a `consteval` function is a compile error that names the function.
inline void sql_identifier_not_validated() {}
inline void sql_spelling_not_validated() {}
inline void sql_statement_capacity_exceeded() {}
inline void sql_statement_names_its_table_twice() {}  // one slot per statement until statements can name several tables
inline void sql_statement_names_no_table() {}

}  // namespace detail

/** @brief Where a statement names its table: the quoted name's first character, and its length (see `on_table`). */
struct TableSlot {
  std::size_t at = 0;
  std::size_t size = 0;
};

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

  /** @brief Appends the statement's table, as `append_identifier` does, and remembers where it is (`table_slot`). */
  consteval void append_table(std::string_view name) {
    if ( _table.size != 0 ) {
      detail::sql_statement_names_its_table_twice();
    }

    _table.at = _size;
    append_identifier(name);
    _table.size = _size - _table.at;
  }

  /** @return Where `append_table` wrote the table: what `on_table` replaces to run the statement on another table. */
  [[nodiscard]] consteval TableSlot table_slot() const {
    if ( _table.size == 0 ) {
      detail::sql_statement_names_no_table();
    }

    return _table;
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
  TableSlot                  _table{};
};

/**
 * @return `sql` naming `table` where it names its own, at `slot`: the same statement, on a table chosen while the program
 * runs. The one function here that runs then; `table` was checked when it was made, so nothing is escaped.
 */
[[nodiscard]] inline std::string on_table(std::string_view sql, TableSlot slot, const schema::TableName& table) {
  std::string text;
  text.reserve(sql.size() - slot.size + table.quoted().size());
  text.append(sql.substr(0, slot.at));
  text.append(table.quoted());
  text.append(sql.substr(slot.at + slot.size));

  return text;
}

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

/** @return Whether each field is an identity key: `GENERATED`, the one primary key column, and of a signed integer type. */
template <schema::IsSchema schema_t, std::size_t... idx>
consteval std::array<bool, schema_t::FIELD_COUNT> identity_flags(std::index_sequence<idx...> /*indices*/) {
  return {
      (schema_t::PRIMARY_KEY_FIELD_COUNT == 1 && schema_t::template field_at<idx>::GENERATED &&
       schema_t::template field_at<idx>::KEY_ROLE == schema::KeyRole::Primary &&
       std::signed_integral<typename schema_t::template field_at<idx>::value_type>)...
  };
}

/** @return Whether each column's SQL type is an array (`text[]`, say): a `std::vector`, or a column type of your own around one. */
template <schema::IsSchema schema_t, std::size_t... idx>
consteval std::array<bool, schema_t::FIELD_COUNT> array_flags(std::index_sequence<idx...> /*indices*/) {
  return {type_name<typename schema_t::template field_at<idx>::value_type>().ends_with("[]")...};
}

/** @return Whether each column's type is one of your own (a `ColumnType`, or a `SqlType` specialization), not one schemacht names. */
template <schema::IsSchema schema_t, std::size_t... idx>
consteval std::array<bool, schema_t::FIELD_COUNT> own_type_flags(std::index_sequence<idx...> /*indices*/) {
  return {builtin_type_name<typename schema_t::template field_at<idx>::value_type>().empty()...};
}

template <schema::IsSchema schema_t, std::size_t... idx>
consteval std::array<bool, schema_t::FIELD_COUNT> nullable_flags(std::index_sequence<idx...> /*indices*/) {
  return {schema_t::template field_at<idx>::NULLABLE...};
}

/** @brief Whether each column of `schema_t` is itself an array: its SQL type ends in `[]`. */
template <schema::IsSchema schema_t>
inline constexpr std::array<bool, schema_t::FIELD_COUNT> COLUMN_ARRAY = array_flags<schema_t>(std::make_index_sequence<schema_t::FIELD_COUNT>{});

/**
 * @brief Whether each column of `schema_t` is of a type of your own. Its values in an array argument go as a `text[]` and are
 * cast to the type one by one: an array literal is split on its element type's own delimiter, which schemacht knows only for
 * the types it names (PostGIS's `geometry` uses `:`, not `,`).
 */
template <schema::IsSchema schema_t>
inline constexpr std::array<bool, schema_t::FIELD_COUNT> COLUMN_OWN_TYPE =
    own_type_flags<schema_t>(std::make_index_sequence<schema_t::FIELD_COUNT>{});

/**
 * @brief Whether the column `column` of `schema_t` goes in an `unnest` insert as a `text[]`, cast back in the select list: an
 * array (`unnest` would flatten an array of arrays), or a type of your own (`COLUMN_OWN_TYPE`).
 */
template <schema::IsSchema schema_t>
[[nodiscard]] consteval bool sent_as_text(std::size_t column) {
  return COLUMN_ARRAY<schema_t>.at(column) || COLUMN_OWN_TYPE<schema_t>.at(column);
}

/** @brief Whether each column of `schema_t` can be NULL (its field is a `std::optional`). */
template <schema::IsSchema schema_t>
inline constexpr std::array<bool, schema_t::FIELD_COUNT> COLUMN_NULLABLE =
    nullable_flags<schema_t>(std::make_index_sequence<schema_t::FIELD_COUNT>{});

/** @brief Whether each column of `schema_t` is `GENERATED` (computed by the database, never inserted). */
template <schema::IsSchema schema_t>
inline constexpr std::array<bool, schema_t::FIELD_COUNT> COLUMN_GENERATED =
    generated_flags<schema_t>(std::make_index_sequence<schema_t::FIELD_COUNT>{});

/** @brief Whether each column of `schema_t` is an identity key, which the database numbers (`GENERATED ALWAYS AS IDENTITY`). */
template <schema::IsSchema schema_t>
inline constexpr std::array<bool, schema_t::FIELD_COUNT> COLUMN_IDENTITY =
    identity_flags<schema_t>(std::make_index_sequence<schema_t::FIELD_COUNT>{});

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

/** @brief The longest SQL type name a value can be cast to (`append_cast_type`). */
inline constexpr std::size_t MAX_CAST_TYPE_LENGTH = 128;

/** @return Whether `left` and `right` are the same apart from the case of ASCII letters, as SQL reads an unquoted name. */
[[nodiscard]] constexpr bool same_name(std::string_view left, std::string_view right) {
  return std::ranges::equal(left, right, [](char lhs, char rhs) {
    const auto lower = [](char chr) { return chr >= 'A' && chr <= 'Z' ? static_cast<char>(chr - 'A' + 'a') : chr; };
    return lower(lhs) == lower(rhs);
  });
}

/**
 * @return The unlimited form of `type` if it is a character or bit string type of limited length (`varchar`, `char`, `bit` and
 * their other spellings, without the length), else empty: `bpchar` for the blank-padded ones and `char` (which alone means a
 * length of one), `varchar` for the varying ones, `varbit` for the bit strings.
 */
[[nodiscard]] consteval std::string_view unlimited_string_type(std::string_view type) {
  constexpr std::string_view                NATIONAL = "national ";  // `national character varying` is `character varying`
  constexpr std::array<std::string_view, 4> PADDED{"char", "character", "nchar", "bpchar"};
  constexpr std::array<std::string_view, 4> VARYING{"varchar", "character varying", "char varying", "nchar varying"};
  constexpr std::array<std::string_view, 3> BITS{"bit", "bit varying", "varbit"};

  constexpr std::string_view CHAR = "char";
  if ( type.size() >= NATIONAL.size() + CHAR.size() && same_name(type.substr(0, NATIONAL.size()), NATIONAL) &&
       same_name(type.substr(NATIONAL.size(), CHAR.size()), CHAR) ) {  // `national char...` only: there is no `national varchar` or `national bit`
    type.remove_prefix(NATIONAL.size());
  }

  const auto named = [type](std::string_view name) { return same_name(type, name); };
  if ( std::ranges::any_of(PADDED, named) ) {
    return "bpchar";
  }

  if ( std::ranges::any_of(VARYING, named) ) {
    return "varchar";
  }

  if ( std::ranges::any_of(BITS, named) ) {
    return "varbit";
  }

  return {};
}

/**
 * @brief Appends `type` as an argument is cast to it (`$1::<type>`). A character or bit string type of limited length is cast to
 * its unlimited form (`varchar(3)` to `varchar`, `char(2)` to `bpchar`; see `unlimited_string_type`): a cast cuts such a value to
 * fit, where storing it fails, so a value too long fails where it is stored (or matches nothing it is compared with) instead of
 * being cut. Any other type is cast to as it is written: its modifier may be more than a size (`geometry(Point,4326)`'s is a
 * geometry type and an SRID, which the cast checks and applies), and a number's precision rounds alike either way. An array's
 * `[]` stays.
 */
template <std::size_t text_capacity>
consteval void append_cast_type(Text<text_capacity>& text, std::string_view type) {
  if ( type.size() > MAX_CAST_TYPE_LENGTH ) {
    sql_statement_capacity_exceeded();
  }

  std::array<char, MAX_CAST_TYPE_LENGTH> chars{};  // `type` without its modifier: `varchar (3)[]` is `varchar[]`
  std::size_t                            size = 0;
  std::size_t                            depth = 0;
  for ( const char chr : type ) {
    if ( chr == '(' ) {
      while ( size > 0 && chars.at(size - 1) == ' ' ) {
        --size;
      }
      ++depth;

    } else if ( chr == ')' ) {
      --depth;

    } else if ( depth == 0 ) {
      chars.at(size++) = chr;
    }
  }

  const std::string_view unmodified(chars.data(), size);
  const std::size_t      dims = unmodified.find("[]");
  const std::string_view unlimited = unlimited_string_type(unmodified.substr(0, dims));
  if ( unlimited.empty() ) {
    text.append(type);
    return;
  }

  text.append(unlimited);
  text.append(dims == std::string_view::npos ? std::string_view() : unmodified.substr(dims));
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

  consteval void node(std::size_t index) { write(index, ANY_LEVEL); }

 private:
  // How tightly an expression binds, by the operator around it: an `OR` inside an `AND` needs parentheses, and nothing
  // else does (`AND` and `OR` are associative, and `NOT` parenthesizes what it negates).
  static constexpr std::size_t ANY_LEVEL = 0;
  static constexpr std::size_t OR_LEVEL = 1;
  static constexpr std::size_t AND_LEVEL = 2;

  const Predicate<schema_t, predicate_capacity>* _predicate;
  Text<text_capacity>*                           _text;

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
        list_cast(current.column);
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

  /**
   * @brief `function("column", $n::type[, $m::double precision])`, or `"column" operator $n::type`: `type` is the column's, or
   * the operation's `argument_type`'s. The column is written inside the operation's `COLUMN_PREFIX` and `COLUMN_SUFFIX`, and
   * the argument inside its `ARGUMENT_PREFIX` and `ARGUMENT_SUFFIX`, if it has them.
   */
  consteval void call(const Node& current) {
    const ArgumentForm*    own = current.operation->argument;
    const std::string_view type = own != nullptr ? own->sql_type : column_type_name<schema_t>(current.column);

    if ( current.operation->infix ) {
      tested_column(current);
      _text->push(' ');
      _text->append_operator(current.operation->name);
      _text->push(' ');
      argument(current, type);
      return;
    }

    _text->append_function(current.operation->name);
    _text->push('(');
    tested_column(current);
    _text->append(", ");
    argument(current, type);

    if ( current.operation->takes_real ) {
      _text->append(", ");
      operand(current.extra);
      cast(type_name<double>());
    }
    _text->push(')');
  }

  /** @brief The call's column as the operation writes it: `"column"`, inside the SQL the operation puts around it (if any). */
  consteval void tested_column(const Node& current) {
    _text->append(current.operation->column_prefix);
    _text->append_identifier(schema_t::column_names().at(current.column));
    _text->append(current.operation->column_suffix);
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
      append_cast_type(*_text, type);
    }
  }

  /** @brief `::<type>[]` on a list argument, or `::text[]::<type>[]` for a type of your own (see `COLUMN_OWN_TYPE`). */
  consteval void list_cast(std::size_t column) {
    const std::string_view type = column_type_name<schema_t>(column);
    if ( type.empty() ) {
      return;
    }

    if ( COLUMN_OWN_TYPE<schema_t>.at(column) ) {
      _text->append("::text[]");
    }
    cast(type);
    _text->append("[]");
  }

  consteval void operand(const Operand& value) { append_operand(*_text, value); }

 public:
  Emitter(const Emitter&) = delete;
  Emitter(Emitter&&) = delete;
  Emitter& operator=(const Emitter&) = delete;
  Emitter& operator=(Emitter&&) = delete;
  constexpr ~Emitter() = default;
};

}  // namespace detail

/** @brief Room for the longest statement `select_statement` can produce; overflowing it is a compile error (`sql_statement_capacity_exceeded`). */
inline constexpr std::size_t STATEMENT_CAPACITY = 4096;

/**
 * @brief Writes the first `count` of `columns`, as a `SELECT` or `RETURNING` lists them. A column whose type has a
 * `SelectExpression` is written as that function of it (`ST_AsBinary("rast") AS "rast"`).
 */
template <schema::IsSchema schema_t, std::size_t text_capacity, std::size_t columns_capacity>
consteval void append_column_list(Text<text_capacity>& text, const std::array<std::size_t, columns_capacity>& columns, std::size_t count) {
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
}

/** @brief Writes `SELECT <the first count of columns> FROM <schema_t's table>`. */
template <schema::IsSchema schema_t, std::size_t text_capacity, std::size_t columns_capacity>
consteval void append_select_from(Text<text_capacity>& text, const std::array<std::size_t, columns_capacity>& columns, std::size_t count) {
  text.append("SELECT ");
  append_column_list<schema_t>(text, columns, count);

  text.append(" FROM ");
  text.append_table(schema_t::TABLE_NAME);
}

/**
 * @return `SELECT <columns> FROM <the table> [WHERE <predicate>] [ORDER BY <terms>] [LIMIT <count>]`,
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
      const ArgumentForm*    own = term.ranking->argument;
      const std::string_view type = own != nullptr ? own->sql_type : detail::column_type_name<schema_t>(term.column);
      if ( ! type.empty() ) {
        text.append("::");
        detail::append_cast_type(text, type);
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

/** @brief Writes ` RETURNING <columns>`, or nothing for a statement that returns no columns. */
template <schema::IsSchema schema_t, std::size_t text_capacity>
consteval void append_returning(Text<text_capacity>& text, const ReturnedColumns<schema_t>& returned) {
  if ( returned.count != 0 ) {
    text.append(" RETURNING ");
    append_column_list<schema_t>(text, returned.columns, returned.count);
  }
}

/** @return `UPDATE <the table> SET <column> = <value>, ... [WHERE <predicate>] [RETURNING <columns>]`, each value `$n`, a literal, or `NULL`. */
template <schema::IsSchema schema_t, std::size_t set_capacity, std::size_t predicate_capacity>
[[nodiscard]] consteval Text<STATEMENT_CAPACITY> update_statement(const Update<schema_t, set_capacity, predicate_capacity>& update) {
  Text<STATEMENT_CAPACITY> text;
  text.append("UPDATE ");
  text.append_table(schema_t::TABLE_NAME);

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
  append_returning(text, update.returned);

  return text;
}

/** @return `DELETE FROM <the table> [WHERE <predicate>] [RETURNING <columns>]`. */
template <schema::IsSchema schema_t, std::size_t predicate_capacity>
[[nodiscard]] consteval Text<STATEMENT_CAPACITY> delete_statement(const Delete<schema_t, predicate_capacity>& del) {
  Text<STATEMENT_CAPACITY> text;
  text.append("DELETE FROM ");
  text.append_table(schema_t::TABLE_NAME);

  append_where(text, del.predicate);
  append_returning(text, del.returned);

  return text;
}

/**
 * @return `CREATE TABLE <the table> (<column> <type> [NOT NULL], ..., PRIMARY KEY (<key columns>), UNIQUE (<column>), ...)`: each
 * column with its type's SQL name, `NOT NULL` unless its field is a `std::optional`, the primary key if the schema has one,
 * and a `UNIQUE` constraint for each `KeyRole::Unique` column on its own. A generated key column (see `COLUMN_IDENTITY`) is
 * numbered by the database: `GENERATED ALWAYS AS IDENTITY`.
 */
template <schema::IsSchema schema_t>
[[nodiscard]] consteval Text<STATEMENT_CAPACITY> create_table_statement() {
  static_assert(
      ! std::ranges::any_of(detail::COLUMN_TYPE_NAMES<schema_t>, &std::string_view::empty),
      "create_table: a column's type has no SQL name; give it one (ColumnType::SQL_TYPE, or a query::sql::SqlType specialization)"
  );
  static_assert(
      detail::COLUMN_GENERATED<schema_t> == detail::COLUMN_IDENTITY<schema_t>,
      "create_table writes a generated column only as an identity key (the one primary key column, of a signed integer type): "
      "create a table with any other generated column yourself"
  );

  Text<STATEMENT_CAPACITY> text;
  text.append("CREATE TABLE ");
  text.append_table(schema_t::TABLE_NAME);
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

    if ( detail::COLUMN_IDENTITY<schema_t>.at(column) ) {
      text.append(" GENERATED ALWAYS AS IDENTITY");
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

/** @brief The longest a table check can be (`table_differences_statement`): it lists every column, so it outgrows other statements. */
inline constexpr std::size_t TABLE_CHECK_CAPACITY = 4 * STATEMENT_CAPACITY;

/** @brief Appends `value` as a SQL string literal: between single quotes, each one inside it doubled. */
template <std::size_t text_capacity>
consteval void append_literal(Text<text_capacity>& text, std::string_view value) {
  text.push('\'');
  for ( const char chr : value ) {
    if ( chr == '\'' ) {
      text.push('\'');
    }
    text.push(chr);
  }
  text.push('\'');
}

/**
 * @return The query that says how the table in the database differs from `schema_t`: a row per difference, of the columns
 * `problem`, `column`, `expected` and `found`, and none if the table is as the schema says (see
 * `postgres::table_differences_statement`, which says what counts as one). It is written from what `create_table_statement` is
 * written from (each column's name, type, `NOT NULL`, key and `GENERATED`), so a table that statement made has no differences.
 *
 * It reads the catalogs, and compares types in the server's own spelling (`format_type`), so `int8` is `bigint` and
 * `geometry(linestring,4326)` is `geometry(LineString,4326)`. That needs `to_regtypemod`: PostgreSQL 17 or newer. What it
 * calls is named in `pg_catalog`, so nothing on the search path can stand in for it. The rows come by kind of problem, as
 * `postgres::TableProblem` lists them, and within a kind in the schema's column order.
 */
template <schema::IsSchema schema_t>
[[nodiscard]] consteval Text<TABLE_CHECK_CAPACITY> table_differences_statement() {
  static_assert(
      ! std::ranges::any_of(detail::COLUMN_TYPE_NAMES<schema_t>, &std::string_view::empty),
      "table_differences: a column's type has no SQL name; give it one (ColumnType::SQL_TYPE, or a query::sql::SqlType specialization)"
  );

  Text<TABLE_CHECK_CAPACITY> text;

  // The table, by name: one that isn't there has no oid, nor has a name that is something else's (an index, a sequence, a type).
  // "is_table" is false for a view, a materialized view and a foreign table, which have columns but no constraints to compare.
  text.append(R"(WITH RECURSIVE "target" AS (SELECT "t"."name", "c"."oid", "c"."relkind" IN ('r', 'p') AS "is_table" FROM (VALUES (')");
  text.append_table(schema_t::TABLE_NAME);
  text.append(
      R"(')) AS "t" ("name") LEFT JOIN "pg_catalog"."pg_class" AS "c" )"
      R"(ON "c"."oid" = pg_catalog.to_regclass("t"."name") AND "c"."relkind" IN ('r', 'p', 'v', 'm', 'f')), )"
  );

  // What the schema says of each column.
  text.append(R"("want" ("ordinal", "name", "type", "not_null", "is_key", "generated") AS (VALUES )");
  for ( std::size_t column = 0; column < schema_t::FIELD_COUNT; ++column ) {
    if ( column != 0 ) {
      text.append(", ");
    }

    text.push('(');
    text.append_unsigned(column + 1);
    text.append(", ");
    append_literal(text, schema_t::column_names().at(column));
    text.append(", ");
    append_literal(text, detail::COLUMN_TYPE_NAMES<schema_t>.at(column));
    text.append(detail::COLUMN_NULLABLE<schema_t>.at(column) ? ", FALSE" : ", TRUE");
    text.append(schema_t::KEY_ROLES.at(column) == schema::KeyRole::Primary ? ", TRUE" : ", FALSE");
    text.append(detail::COLUMN_GENERATED<schema_t>.at(column) ? ", TRUE" : ", FALSE");
    text.push(')');
  }
  text.append("), ");

  // Each column's type, a domain followed down to the type it is over, gathering each domain's NOT NULL and default on the way:
  // "has_default" if the database gives the column a value when an insert leaves it out.
  text.append(
      R"("types" ("attnum", "typid", "typmod", "not_null", "has_default") AS ()"
      R"(SELECT "a"."attnum", "a"."atttypid", "a"."atttypmod", "a"."attnotnull", )"
      R"(("a"."atthasdef" OR "a"."attidentity" <> '' OR "a"."attgenerated" <> '') )"
      R"(FROM "target" JOIN "pg_catalog"."pg_attribute" AS "a" ON "a"."attrelid" = "target"."oid" )"
      R"(WHERE "a"."attnum" > 0 AND NOT "a"."attisdropped" )"
      R"(UNION ALL SELECT "types"."attnum", "d"."typbasetype", "d"."typtypmod", "types"."not_null" OR "d"."typnotnull", )"
      R"("types"."has_default" OR "d"."typdefault" IS NOT NULL )"
      R"(FROM "types" JOIN "pg_catalog"."pg_type" AS "d" ON "d"."oid" = "types"."typid" WHERE "d"."typtype" = 'd'), )"
  );

  // What the table has: "refuses_value" if an insert may not give the column a value.
  text.append(
      R"("have" AS (SELECT "a"."attname"::text AS "name", pg_catalog.format_type("types"."typid", "types"."typmod") AS "type", )"
      R"("types"."not_null", "types"."has_default" AS "fills_in", ("a"."attidentity" = 'a' OR "a"."attgenerated" <> '') AS "refuses_value" )"
      R"(FROM "target" JOIN "pg_catalog"."pg_attribute" AS "a" ON "a"."attrelid" = "target"."oid" )"
      R"(JOIN "types" ON "types"."attnum" = "a"."attnum" )"
      R"(JOIN "pg_catalog"."pg_type" AS "b" ON "b"."oid" = "types"."typid" AND "b"."typtype" <> 'd'), )"
      R"("want_key" AS (SELECT ARRAY(SELECT "name" FROM "want" WHERE "is_key" ORDER BY 1) AS "names"), )"
      R"("have_key" AS (SELECT ARRAY(SELECT "a"."attname"::text FROM "target" )"
      R"(JOIN "pg_catalog"."pg_index" AS "i" ON "i"."indrelid" = "target"."oid" AND "i"."indisprimary" )"
      R"(JOIN "pg_catalog"."pg_attribute" AS "a" ON "a"."attrelid" = "i"."indrelid" )"
      R"(AND "a"."attnum" = ANY (("i"."indkey"::int2[])[0:"i"."indnkeyatts" - 1]) ORDER BY 1) AS "names"), )"  // not its INCLUDE columns
  );

  text.append(
      R"("differences" ("position", "ordinal", "problem", "column", "expected", "found") AS ()"
      R"(SELECT 0, 0, 'missing_table', '', "target"."name", '' FROM "target" WHERE "target"."oid" IS NULL )"
      R"(UNION ALL SELECT 1, "want"."ordinal", 'missing_column', "want"."name", "want"."type", '' )"
      R"(FROM "target", "want" LEFT JOIN "have" ON "have"."name" = "want"."name" )"
      R"(WHERE "target"."oid" IS NOT NULL AND "have"."name" IS NULL )"
      R"(UNION ALL SELECT 2, "want"."ordinal", 'unknown_type', "want"."name", "want"."type", "have"."type" )"
      R"(FROM "want" JOIN "have" ON "have"."name" = "want"."name" WHERE pg_catalog.to_regtype("want"."type") IS NULL )"
      R"(UNION ALL SELECT 3, "want"."ordinal", 'wrong_type', "want"."name", )"
      R"(pg_catalog.format_type(pg_catalog.to_regtype("want"."type"), pg_catalog.to_regtypemod("want"."type")), "have"."type" )"
      R"(FROM "want" JOIN "have" ON "have"."name" = "want"."name" )"
      R"(WHERE "have"."type" <> pg_catalog.format_type(pg_catalog.to_regtype("want"."type"), pg_catalog.to_regtypemod("want"."type")) )"
      R"(UNION ALL SELECT 4, "want"."ordinal", 'nullability', "want"."name", )"
      R"(CASE WHEN "want"."not_null" THEN 'NOT NULL' ELSE 'nullable' END, CASE WHEN "have"."not_null" THEN 'NOT NULL' ELSE 'nullable' END )"
      R"(FROM "target", "want" JOIN "have" ON "have"."name" = "want"."name" )"
      R"(WHERE "target"."is_table" AND "have"."not_null" <> "want"."not_null" )"
      R"(UNION ALL SELECT 5, "want"."ordinal", 'no_default', "want"."name", '', '' )"
      R"(FROM "target", "want" JOIN "have" ON "have"."name" = "want"."name" )"
      R"(WHERE "target"."is_table" AND "want"."generated" AND "have"."not_null" AND NOT "have"."fills_in" )"
      R"(UNION ALL SELECT 5, "want"."ordinal", 'always_generated', "want"."name", '', '' )"
      R"(FROM "want" JOIN "have" ON "have"."name" = "want"."name" WHERE NOT "want"."generated" AND "have"."refuses_value" )"
  );

  if constexpr ( schema_t::HAS_PRIMARY_KEY ) {  // a schema without a key says nothing of the table's
    text.append(
        R"(UNION ALL SELECT 6, 0, 'primary_key', '', pg_catalog.array_to_string("want_key"."names", ', '), )"
        R"(pg_catalog.array_to_string("have_key"."names", ', ') )"
        R"(FROM "target", "want_key", "have_key" WHERE "target"."is_table" AND "want_key"."names" <> "have_key"."names" )"
    );
  }

  text.append(
      R"(UNION ALL SELECT 7, 0, 'extra_required_column', "have"."name", '', "have"."type" FROM "have" )"
      R"(WHERE "have"."not_null" AND NOT "have"."fills_in" AND "have"."name" NOT IN (SELECT "name" FROM "want")) )"
      R"(SELECT "problem", "column", "expected", "found" FROM "differences" ORDER BY "position", "ordinal", "column")"
  );

  return text;
}

/** @return `DROP TABLE IF EXISTS <the table>`. */
template <schema::IsSchema schema_t>
[[nodiscard]] consteval Text<STATEMENT_CAPACITY> drop_table_statement() {
  Text<STATEMENT_CAPACITY> text;
  text.append("DROP TABLE IF EXISTS ");
  text.append_table(schema_t::TABLE_NAME);

  return text;
}

/** @brief What an upsert calls the row already in the table, in its `SET` clause. */
inline constexpr std::string_view UPSERT_CURRENT = "current";

/** @brief Writes `INSERT INTO <the table> (<every insertable column>)`, naming the table `alias` (`<the table> AS <alias>`) unless it is empty. */
template <schema::IsSchema schema_t, std::size_t text_capacity>
consteval void append_insert_into(Text<text_capacity>& text, std::string_view alias) {
  text.append("INSERT INTO ");
  text.append_table(schema_t::TABLE_NAME);
  if ( ! alias.empty() ) {
    text.append(" AS ");
    text.append_identifier(alias);
  }
  text.append(" (");
  text.append_identifier_list(schema_t::insertable_column_names());
  text.push(')');
}

/** @brief Appends `"current"."column"`: `column` of the row already in the table, in an upsert's `SET` clause. */
template <std::size_t text_capacity>
consteval void append_current(Text<text_capacity>& text, std::string_view column) {
  text.append_identifier(UPSERT_CURRENT);
  text.push('.');
  text.append_identifier(column);
}

/** @brief Appends `excluded."column"`: `column` of the row that would have gone in, in an upsert's `SET` clause. */
template <std::size_t text_capacity>
consteval void append_excluded(Text<text_capacity>& text, std::string_view column) {
  text.append("excluded.");
  text.append_identifier(column);
}

/** @brief Appends `insert_unnest_statement`'s INSERT, naming the table `alias` unless it is empty (see `append_insert_into`). */
template <schema::IsSchema schema_t, std::size_t text_capacity>
consteval void append_insert_unnest(Text<text_capacity>& text, std::string_view alias) {
  append_insert_into<schema_t>(text, alias);

  text.append(" SELECT ");
  bool first = true;
  for ( std::size_t column = 0; column < schema_t::FIELD_COUNT; ++column ) {
    if ( detail::COLUMN_GENERATED<schema_t>.at(column) ) {
      continue;
    }

    if ( ! first ) {
      text.append(", ");
    }
    first = false;

    text.append_identifier(schema_t::column_names().at(column));
    if ( detail::sent_as_text<schema_t>(column) ) {
      text.append("::");
      detail::append_cast_type(text, detail::COLUMN_TYPE_NAMES<schema_t>.at(column));
    }
  }

  text.append(" FROM unnest(");
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
    detail::append_cast_type(
        text, detail::sent_as_text<schema_t>(column) ? std::string_view("text") : detail::COLUMN_TYPE_NAMES<schema_t>.at(column)
    );
    text.append("[]");
  }

  text.append(") AS u(");
  text.append_identifier_list(schema_t::insertable_column_names());
  text.push(')');
}

/**
 * @return `INSERT INTO <the table> (<columns>) SELECT <columns> FROM unnest($1::<type>[], ...) AS u(<columns>)`: one array argument per
 * insertable column, holding that column's value for every row, so any number of rows go in one statement. A column that is
 * itself an array, or of a type of your own, goes as a `text[]` of its values' text forms, cast back to its type in the select
 * list (`detail::sent_as_text`). Every insertable column's type needs an SQL type name (`sql::type_name`).
 */
template <schema::IsSchema schema_t>
[[nodiscard]] consteval Text<STATEMENT_CAPACITY> insert_unnest_statement() {
  Text<STATEMENT_CAPACITY> text;
  append_insert_unnest<schema_t>(text, {});

  return text;
}

/**
 * @brief How an upsert sets one column of a row that is already there, by `operation`: `name("current"."column",
 * excluded."column")` for a function, `"current"."column" name excluded."column"` for an operator (`infix`); "current" is the row
 * there, `excluded` the row that would have gone in. Made by `postgres::MergeWith`.
 */
struct Merge {
  std::size_t      column = 0;           ///< Index into the schema's fields.
  const Operation* operation = nullptr;  ///< The function or operator that merges the two values; null to keep the value there.
};

/**
 * @return `insert_unnest_statement`'s INSERT, with the table named `"current"`, ending `ON CONFLICT (<matched columns>) DO UPDATE
 * SET ...`: a row whose matched columns are already there updates that row instead. The matched columns are the primary key,
 * or the one column at `matched` (a `KeyRole::Unique` column, by index into the schema's fields). Each column that is neither
 * matched, nor a primary key column, nor generated is set as its `Merge` in `merges` says (or left out, if it keeps the value
 * there), or else to the value that would have gone in (`"column" = excluded."column"`). The primary key is never set: a row
 * matched on a unique column keeps its key. The schema must have what it matches on and a column to set
 * (`postgres::upsert_statement` checks both).
 */
template <schema::IsSchema schema_t, std::size_t merge_count>
[[nodiscard]] consteval Text<STATEMENT_CAPACITY> upsert_statement(const std::array<Merge, merge_count>& merges, std::optional<std::size_t> matched) {
  Text<STATEMENT_CAPACITY> text;
  append_insert_unnest<schema_t>(text, UPSERT_CURRENT);

  text.append(" ON CONFLICT (");
  if ( matched ) {
    text.append_identifier(schema_t::column_names().at(*matched));
  } else {
    text.append_identifier_list(schema_t::primary_key_column_names());
  }
  text.append(") DO UPDATE SET ");

  bool first = true;
  for ( std::size_t column = 0; column < schema_t::FIELD_COUNT; ++column ) {
    if ( detail::COLUMN_GENERATED<schema_t>.at(column) || schema_t::KEY_ROLES.at(column) == schema::KeyRole::Primary || column == matched ) {
      continue;
    }

    const auto merge = std::ranges::find(merges, column, &Merge::column);
    if ( merge != merges.end() && merge->operation == nullptr ) {
      continue;  // kept as it is
    }

    if ( ! first ) {
      text.append(", ");
    }
    first = false;

    const std::string_view name = schema_t::column_names().at(column);
    text.append_identifier(name);
    text.append(" = ");

    if ( merge == merges.end() ) {
      append_excluded(text, name);

    } else if ( merge->operation->infix ) {
      append_current(text, name);
      text.push(' ');
      text.append_operator(merge->operation->name);
      text.push(' ');
      append_excluded(text, name);

    } else {
      text.append_function(merge->operation->name);
      text.push('(');
      append_current(text, name);
      text.append(", ");
      append_excluded(text, name);
      text.push(')');
    }
  }

  return text;
}

/** @return ` RETURNING <every column>`, which an INSERT ends with to give back the rows as written. */
template <schema::IsSchema schema_t>
[[nodiscard]] consteval Text<STATEMENT_CAPACITY> returning_every_column() {
  Text<STATEMENT_CAPACITY> text;
  append_returning(text, ReturnedColumns<schema_t>::every_column());

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
