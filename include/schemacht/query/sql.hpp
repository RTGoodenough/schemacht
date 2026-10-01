#pragma once

#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "schemacht/query/predicate.hpp"
#include "schemacht/query/query.hpp"
#include "schemacht/query/sql_type.hpp"
#include "schemacht/schema/schema.hpp"
#include "schemacht/util/identifier.hpp"

/**
 * Lowering a `Query` to a PostgreSQL statement, as written, entirely at
 * compile time. Every value that isn't a literal is a `$n` placeholder, so the
 * text is a constant: no string building, and no per-call formatting of values.
 */
namespace schemacht::query::sql {

/** @brief A fixed-capacity string that can be built in a constant expression. */
template <std::size_t capacity>
class Text {
 public:
  constexpr void push(char chr) { _chars.at(_size++) = chr; }

  constexpr void append(std::string_view part) {
    for ( const char chr : part ) {
      push(chr);
    }
  }

  constexpr void append_unsigned(std::uint64_t value) { append_digits<DECIMAL_BASE, DECIMAL_DIGITS>(value); }

  constexpr void append_signed(std::int64_t value) {
    if ( value < 0 ) {
      push('-');
      append_unsigned(std::uint64_t{0} - std::bit_cast<std::uint64_t>(value));
    } else {
      append_unsigned(static_cast<std::uint64_t>(value));
    }
  }

  /** @brief Appends `name` double-quoted, after checking it with `util::is_identifier`. */
  constexpr void append_identifier(std::string_view name) {
    if ( ! util::is_identifier(name) ) {
      throw std::invalid_argument("invalid SQL identifier");
    }
    push('"');
    append(name);
    push('"');
  }

  /** @brief Appends a SQL function name, after checking it is only letters, digits and underscores. */
  constexpr void append_function(std::string_view name) {
    if ( name.empty() || (name.front() >= '0' && name.front() <= '9') ) {
      throw std::invalid_argument("invalid SQL function name");
    }
    for ( const char chr : name ) {
      const bool valid = (chr >= 'a' && chr <= 'z') || (chr >= 'A' && chr <= 'Z') || (chr >= '0' && chr <= '9') || chr == '_';
      if ( ! valid ) {
        throw std::invalid_argument("invalid SQL function name");
      }
    }
    append(name);
  }

  /**
   * @brief Appends a SQL operator such as `<->`, after checking it is only operator characters and that PostgreSQL would
   * read it as one operator: `--` and slash-star start a comment, which would swallow the rest of the statement, and an
   * operator of several characters may end in `+` or `-` only if it also has one of `~ ! @ # % ^ & | ?`.
   */
  constexpr void append_operator(std::string_view name) {
    constexpr std::string_view OPERATOR_CHARS = "<>-|#&~=@+*/%^!?";
    constexpr std::string_view UNSIGNED_CHARS = "~!@#%^&|?";  // any of these lets an operator end in + or -
    if ( name.empty() ) {
      throw std::invalid_argument("invalid SQL operator");
    }
    for ( const char chr : name ) {
      if ( ! OPERATOR_CHARS.contains(chr) ) {
        throw std::invalid_argument("invalid SQL operator");
      }
    }
    if ( name.contains("--") || name.contains("/*") ) {
      throw std::invalid_argument("invalid SQL operator: it would start a comment");
    }
    const bool ends_in_sign = name.back() == '+' || name.back() == '-';
    if ( name.size() > 1 && ends_in_sign && name.find_first_of(UNSIGNED_CHARS) == std::string_view::npos ) {
      throw std::invalid_argument("invalid SQL operator: PostgreSQL would read the trailing sign as a separate operator");
    }
    append(name);
  }

  [[nodiscard]] constexpr std::size_t size() const { return _size; }
  [[nodiscard]] constexpr char        at(std::size_t index) const { return _chars.at(index); }

 private:
  static constexpr std::uint64_t DECIMAL_BASE = 10;
  static constexpr std::size_t   DECIMAL_DIGITS = 20;  // the most digits a uint64_t has

  /** @brief Appends `value` in `base`, most significant digit first; at least one digit, so 0 is "0". */
  template <std::uint64_t base, std::size_t max_digits>
  constexpr void append_digits(std::uint64_t value) {
    constexpr std::string_view   DIGITS = "0123456789abcdef";
    std::array<char, max_digits> digits{};
    std::size_t                  count = 0;
    for ( bool more = true; more; more = value != 0 ) {
      digits.at(count++) = DIGITS.at(value % base);
      value /= base;
    }
    while ( count > 0 ) {
      push(digits.at(--count));
    }
  }

  std::array<char, capacity> _chars{};
  std::size_t                _size = 0;
};

namespace detail {

template <schema::IsSchema schema_t>
[[nodiscard]] constexpr std::string_view column_type_name(std::size_t column) {
  return schema_t::visit_column(column, []<std::size_t idx> { return type_name<typename schema_t::template field_at<idx>::value_type>(); });
}

constexpr std::string_view comparison_symbol(Comparison comparison) {
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
  throw std::logic_error("unknown comparison");
}

/** @brief Writes a predicate tree as a SQL boolean expression, as it was written. */
template <schema::IsSchema schema_t, std::size_t predicate_capacity, std::size_t text_capacity>
class Emitter {
 public:
  constexpr Emitter(const Predicate<schema_t, predicate_capacity>* predicate, Text<text_capacity>* text) : _predicate(predicate), _text(text) {}

  constexpr Emitter(const Emitter&) = default;
  constexpr Emitter(Emitter&&) noexcept = default;
  constexpr Emitter& operator=(const Emitter&) = default;
  constexpr Emitter& operator=(Emitter&&) noexcept = default;
  constexpr ~Emitter() = default;

  constexpr void node(std::size_t index) { write(index, ANY_LEVEL); }

 private:
  // How tightly an expression binds, by the operator around it: an `OR` inside an `AND` needs parentheses, and nothing
  // else does (`AND` and `OR` are associative, and `NOT` parenthesizes what it negates).
  static constexpr std::size_t ANY_LEVEL = 0;
  static constexpr std::size_t OR_LEVEL = 1;
  static constexpr std::size_t AND_LEVEL = 2;

  /** @param context The level of the junction this expression is an operand of. */
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- a node index and a precedence level, in that order at every call.
  constexpr void write(std::size_t index, std::size_t context) {
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

  constexpr void junction(const Node& current, std::string_view word, std::size_t level, std::size_t context) {
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
  constexpr void call(const Node& current) {
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
  constexpr void argument(const Node& current, std::string_view type) {
    _text->append(current.operation->argument_prefix);
    operand(current.operand);
    cast(type);
    _text->append(current.operation->argument_suffix);
  }

  constexpr void cast(std::string_view type) {
    if ( ! type.empty() ) {
      _text->append("::");
      _text->append(type);
    }
  }

  constexpr void cast_array(std::string_view type) {
    if ( ! type.empty() ) {
      cast(type);
      _text->append("[]");
    }
  }

  constexpr void operand(const Operand& value) {
    if ( value.kind == OperandKind::Arg ) {
      _text->push('$');
      _text->append_unsigned(value.slot + 1);
      return;
    }
    switch ( value.constant.kind ) {
      case ConstantKind::Bool:
        _text->append(value.constant.bits != 0 ? "TRUE" : "FALSE");
        return;
      case ConstantKind::Signed:
        _text->append_signed(std::bit_cast<std::int64_t>(value.constant.bits));
        return;
      case ConstantKind::Unsigned:
        _text->append_unsigned(value.constant.bits);
        return;
    }
  }

  const Predicate<schema_t, predicate_capacity>* _predicate;
  Text<text_capacity>*                           _text;
};

}  // namespace detail

/** @brief Room for the longest statement `select_statement` can produce; overflowing it is a compile error. */
inline constexpr std::size_t STATEMENT_CAPACITY = 4096;

/**
 * @brief Writes `SELECT <the first count of columns> FROM <schema_t's table>`, the shared start of every statement. A column whose type has a
 * `Select_Expression` is written as that function of it (`ST_AsBinary("rast") AS "rast"`).
 */
template <schema::IsSchema schema_t, std::size_t text_capacity, std::size_t columns_capacity>
constexpr void append_select_from(Text<text_capacity>& text, const std::array<std::size_t, columns_capacity>& columns, std::size_t count) {
  text.append("SELECT ");
  for ( std::size_t i = 0; i < count; ++i ) {
    if ( i != 0 ) {
      text.append(", ");
    }
    const std::string_view name = schema_t::column_names().at(columns.at(i));
    schema_t::visit_column(columns.at(i), [&]<std::size_t idx> {
      constexpr std::string_view FUNCTION = Select_Expression<typename schema_t::template field_at<idx>::value_type>::FUNCTION;
      if constexpr ( FUNCTION.empty() ) {
        text.append_identifier(name);
      } else {
        text.append(FUNCTION);  // the type reads the column through a function of it: `FUNCTION("col") AS "col"`
        text.push('(');
        text.append_identifier(name);
        text.append(") AS ");
        text.append_identifier(name);
      }
    });
  }
  text.append(" FROM ");
  text.append_identifier(schema_t::TABLE_NAME);
}

/**
 * @return `SELECT <columns> FROM <table> [WHERE <predicate>] [ORDER BY <terms>] [LIMIT <count>]`,
 * with each argument as `$n` (`arg<N>()` is `$N+1`).
 */
template <schema::IsSchema schema_t, std::size_t predicate_capacity, std::size_t order_capacity>
[[nodiscard]] constexpr Text<STATEMENT_CAPACITY> select_statement(const Query<schema_t, predicate_capacity, order_capacity>& query) {
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
      text.append_unsigned(query.row_limit.count);
      break;
    case LimitKind::Arg:
      text.append(" LIMIT $");
      text.append_unsigned(query.row_limit.slot + 1);
      break;
  }
  return text;
}

/** @return `text`'s contents as a null-terminated array of exactly `length + 1` characters. */
template <std::size_t length, std::size_t capacity>
[[nodiscard]] constexpr std::array<char, length + 1> terminated(const Text<capacity>& text) {
  std::array<char, length + 1> result{};
  for ( std::size_t i = 0; i < length; ++i ) {
    result.at(i) = text.at(i);
  }
  return result;
}

}  // namespace schemacht::query::sql
