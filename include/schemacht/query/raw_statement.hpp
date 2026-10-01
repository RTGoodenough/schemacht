#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "schemacht/schema/field.hpp"
#include "schemacht/util/compile_time.hpp"

namespace schemacht::query {

/** @brief The C++ types of a `Raw_Statement`'s arguments, in the order of its `$1`, `$2`, ... */
template <typename... argument_ts>
struct RawArguments {
  using types = std::tuple<argument_ts...>;
};

/** @brief What is wrong with a raw statement's placeholders, if anything; see `check_placeholders`. */
enum class PlaceholderProblem : std::uint8_t {
  None,
  Unused,        ///< A declared argument has no `$n` in the statement.
  Beyond,        ///< A `$n` names an argument that was not declared (or is `$0`).
  DollarQuote,   ///< `$$ ... $$` or `$tag$ ... $tag$`: text the scanner cannot see into, so it cannot tell what is a placeholder.
  Unterminated,  ///< A string, quoted name or comment that never ends.
};

/** @brief The result of `check_placeholders`: the problem, and the placeholder number it concerns (0 if none). */
struct PlaceholderCheck {
  PlaceholderProblem problem = PlaceholderProblem::None;
  std::size_t        number = 0;

  [[nodiscard]] constexpr bool operator==(const PlaceholderCheck&) const = default;
};

inline constexpr std::size_t DECIMAL_BASE = 10;

/** @brief The most arguments a raw statement may declare. */
inline constexpr std::size_t MAX_RAW_ARGUMENTS = 128;

namespace detail {

[[nodiscard]] constexpr bool is_name_char(char chr) {
  return (chr >= 'a' && chr <= 'z') || (chr >= 'A' && chr <= 'Z') || (chr >= '0' && chr <= '9') || chr == '_' || chr == '$';
}

[[nodiscard]] constexpr bool is_digit(char chr) { return chr >= '0' && chr <= '9'; }

/** @brief What `SQL_Scanner::skip_non_code` found at the scanner's position. */
enum class Skipped : std::uint8_t {
  Nothing,       ///< Code: nothing was skipped.
  Text,          ///< A string, quoted name or comment, now skipped.
  Unterminated,  ///< One that never ends.
};

/**
 * @brief Walks over SQL text, skipping what is not code: quoted strings, quoted names and comments.
 *
 * What is left is where a `$n` is a placeholder. A `$` that follows a name character is part of a name (`a$1`), not a
 * placeholder.
 */
class SQL_Scanner {
 public:
  explicit constexpr SQL_Scanner(std::string_view sql) : _sql(sql) {}

  constexpr SQL_Scanner(const SQL_Scanner&) = default;
  constexpr SQL_Scanner(SQL_Scanner&&) noexcept = default;
  constexpr SQL_Scanner& operator=(const SQL_Scanner&) = default;
  constexpr SQL_Scanner& operator=(SQL_Scanner&&) noexcept = default;
  constexpr ~SQL_Scanner() = default;

  [[nodiscard]] constexpr bool done() const { return _at >= _sql.size(); }
  [[nodiscard]] constexpr char peek(std::size_t ahead = 0) const { return _at + ahead < _sql.size() ? _sql.at(_at + ahead) : '\0'; }
  constexpr void               advance(std::size_t count = 1) { _at += count; }
  [[nodiscard]] constexpr bool after_name_char() const { return _at > 0 && is_name_char(_sql.at(_at - 1)); }

  /** @brief Skips the string, quoted name or comment that starts here, if one does. */
  constexpr Skipped skip_non_code() {
    const char chr = peek();
    bool       ended = true;
    if ( chr == '\'' ) {
      ended = skip_quoted('\'', escape_strings());
    } else if ( chr == '"' ) {
      ended = skip_quoted('"', false);
    } else if ( chr == '-' && peek(1) == '-' ) {
      while ( ! done() && peek() != '\n' ) {
        advance();
      }
    } else if ( chr == '/' && peek(1) == '*' ) {
      ended = skip_block_comment();
    } else {
      return Skipped::Nothing;
    }
    return ended ? Skipped::Text : Skipped::Unterminated;
  }

 private:
  /** @brief Whether the quote here opens an `E'...'` string, in which a backslash escapes the next character. */
  [[nodiscard]] constexpr bool escape_strings() const {
    return _at > 0 && (_sql.at(_at - 1) == 'e' || _sql.at(_at - 1) == 'E') && (_at < 2 || ! is_name_char(_sql.at(_at - 2)));
  }

  /** @return Whether the quoted text ended (false: the statement ran out first). */
  constexpr bool skip_quoted(char quote, bool backslash_escapes) {
    advance();
    while ( ! done() ) {
      if ( (backslash_escapes && peek() == '\\') || (peek() == quote && peek(1) == quote) ) {
        advance(2);  // a backslash escapes the next character (in an E'' string), and a doubled quote is one quote inside the text
      } else if ( peek() == quote ) {
        advance();
        return true;
      } else {
        advance();
      }
    }
    return false;
  }

  /** @return Whether the comment ended (false: the statement ran out first). */
  constexpr bool skip_block_comment() {
    std::size_t depth = 0;  // block comments nest in PostgreSQL
    while ( ! done() ) {
      if ( peek() == '/' && peek(1) == '*' ) {
        ++depth;
        advance(2);
      } else if ( peek() == '*' && peek(1) == '/' ) {
        --depth;
        advance(2);
        if ( depth == 0 ) {
          return true;
        }
      } else {
        advance();
      }
    }
    return false;
  }

  std::string_view _sql;
  std::size_t      _at = 0;
};

}  // namespace detail

/**
 * @brief Checks the `$n` placeholders in `sql` against `argument_count` declared arguments: every argument is used, and
 * none is named that was not declared.
 *
 * The statement is a constant, so a mismatch is a mistake in the source, not something the data can cause; it is caught
 * when the statement is compiled instead of when the database rejects it. Strings, quoted names and comments are skipped
 * (`'costs $5'` has no placeholder). Dollar-quoted text is refused, because the scanner cannot tell what is inside it.
 */
[[nodiscard]] constexpr PlaceholderCheck check_placeholders(std::string_view sql, std::size_t argument_count) {
  std::array<bool, MAX_RAW_ARGUMENTS + 1> used{};
  detail::SQL_Scanner                     scanner(sql);
  while ( ! scanner.done() ) {
    const detail::Skipped skipped = scanner.skip_non_code();
    if ( skipped == detail::Skipped::Unterminated ) {
      return {.problem = PlaceholderProblem::Unterminated};
    }
    if ( skipped == detail::Skipped::Text ) {
      continue;
    }
    if ( scanner.peek() != '$' || scanner.after_name_char() ) {
      scanner.advance();
      continue;
    }
    scanner.advance();
    if ( ! detail::is_digit(scanner.peek()) ) {
      return {.problem = PlaceholderProblem::DollarQuote};
    }
    std::size_t number = 0;
    while ( detail::is_digit(scanner.peek()) ) {
      number = (number * DECIMAL_BASE) + static_cast<std::size_t>(scanner.peek() - '0');
      scanner.advance();
      if ( number > MAX_RAW_ARGUMENTS ) {
        return {.problem = PlaceholderProblem::Beyond, .number = number};
      }
    }
    if ( number == 0 || number > argument_count ) {
      return {.problem = PlaceholderProblem::Beyond, .number = number};
    }
    used.at(number) = true;
  }
  for ( std::size_t number = 1; number <= argument_count; ++number ) {
    if ( ! used.at(number) ) {
      return {.problem = PlaceholderProblem::Unused, .number = number};
    }
  }
  return {};
}

template <util::CTString sql_text, typename arguments_t, schema::IsField... field_ts>
class Raw_Statement;

/**
 * @brief A `Raw_Statement` with its arguments bound: what a backend runs, as it runs a `Typed_Query`.
 *
 * Build one with `Raw_Statement::bind(...)`. It holds the arguments as the C++ types the statement declares; the backend writes them as text.
 *
 * @tparam statement_t The `Raw_Statement` it is a use of.
 */
template <typename statement_t>
class Bound_Raw_Statement {
 public:
  using statement_type = statement_t;

  /** @brief One result row: a `std::tuple` of the statement's `Field`s, as a `Typed_Query`'s `result_type`. */
  using result_type = statement_t::row_type;

  using arguments_type = statement_t::argument_types;

  explicit Bound_Raw_Statement(arguments_type arguments) : _arguments(std::move(arguments)) {}

  Bound_Raw_Statement(const Bound_Raw_Statement&) = default;
  Bound_Raw_Statement(Bound_Raw_Statement&&) noexcept = default;
  Bound_Raw_Statement& operator=(const Bound_Raw_Statement&) = default;
  Bound_Raw_Statement& operator=(Bound_Raw_Statement&&) noexcept = default;
  ~Bound_Raw_Statement() = default;

  /** @return The statement, with `$n` placeholders for `arguments()`. */
  [[nodiscard]] static constexpr std::string_view sql() noexcept { return statement_t::SQL; }

  /** @return The columns' names, in order: what the statement's select list must call them. */
  [[nodiscard]] static constexpr std::span<const std::string_view> column_names() noexcept { return statement_t::COLUMN_NAMES; }

  /** @return The arguments, in placeholder order: element 0 is `$1`. */
  [[nodiscard]] const arguments_type& arguments() const noexcept { return _arguments; }

 private:
  arguments_type _arguments;
};

/**
 * @brief The escape hatch: a SQL statement written out in full, for what the query builder cannot say.
 *
 * The statement and its types are all in one type, so a use is checked like any other query:
 *
 * @code
 * using Elevation = query::Raw_Statement<
 *     "SELECT ST_Value(rast, 1, ST_SetSRID(ST_MakePoint($1, $2), 4326)) AS metres "
 *     "FROM elevation WHERE ST_Intersects(rast, ST_SetSRID(ST_MakePoint($1, $2), 4326))",
 *     query::RawArguments<double, double>,
 *     schema::Field<std::optional<double>, "metres">>;
 *
 * std::vector<Elevation::row_type> found = database.execute(Elevation::bind(-122.4, 37.8)).get();
 * @endcode
 *
 * - **No injection by construction.** The text is a compile-time constant, and values reach it only as `$n` arguments,
 *   sent apart from the text. Nothing is spliced in.
 * - **Arguments** are the C++ types listed, each written as text by `backend::Field_Codec` (a `std::vector` of one becomes
 *   an array). Parameters are untyped on the wire, so where PostgreSQL cannot tell a `$n`'s type from its context, cast
 *   it in the statement (`$1::geometry`). An argument cannot be NULL yet.
 * - **Columns** are the `Field`s, in the order of the statement's select list and **named as the columns it returns**
 *   (`... AS metres`): a result whose columns differ from the fields is an error, not silently the wrong values. A
 *   column that can be NULL is a `std::optional` field, as everywhere.
 * - **Placeholders are checked at compile time** (`check_placeholders`).
 * - **Run like any query**: `bind` gives a `Bound_Raw_Statement`, which a `postgres::Database` executes like a query (a raw statement is about no table).
 * - **Only Postgres runs one.**
 *
 * @tparam sql_text     The statement, with `$1`, `$2`, ... for the arguments.
 * @tparam arguments_t  `RawArguments<...>`: the type of each argument.
 * @tparam field_ts     The columns of the result: at least one.
 */
template <util::CTString sql_text, typename arguments_t, schema::IsField... field_ts>
class Raw_Statement {
 public:
  static_assert(sizeof...(field_ts) > 0, "a raw statement returns rows: name at least one column");

  using argument_types = arguments_t::types;

  /** @brief One result row: a `std::tuple` of the columns' `Field`s. */
  using row_type = std::tuple<field_ts...>;

  static constexpr std::string_view SQL{sql_text.view()};
  static constexpr std::size_t      ARGUMENT_COUNT = std::tuple_size_v<argument_types>;
  static constexpr std::size_t      COLUMN_COUNT = sizeof...(field_ts);

  /** @brief The columns' names, in order: what the statement's select list must call them. */
  static constexpr std::array<std::string_view, COLUMN_COUNT> COLUMN_NAMES{field_ts::COLUMN_NAME.view()...};

  /**
   * @brief Supplies the arguments, one for each of the declared ones (each must convert to the declared type).
   * @return The statement, ready for a backend's `execute`.
   */
  template <typename... arg_ts>
  [[nodiscard]] static Bound_Raw_Statement<Raw_Statement> bind(const arg_ts&... args) {
    static_assert(sizeof...(arg_ts) == ARGUMENT_COUNT, "wrong number of arguments for this raw statement");
    static_assert(std::is_constructible_v<argument_types, const arg_ts&...>, "an argument does not have the type its raw statement declares");
    return Bound_Raw_Statement<Raw_Statement>(argument_types(args...));
  }

 private:
  static constexpr PlaceholderCheck PLACEHOLDERS = check_placeholders(SQL, ARGUMENT_COUNT);

  static_assert(ARGUMENT_COUNT <= MAX_RAW_ARGUMENTS, "a raw statement takes at most MAX_RAW_ARGUMENTS arguments");
  static_assert(PLACEHOLDERS.problem != PlaceholderProblem::Unused, "a declared argument of the raw statement has no $n placeholder in it");
  static_assert(PLACEHOLDERS.problem != PlaceholderProblem::Beyond, "the raw statement has a $n (or $0) beyond the arguments it declares");
  static_assert(PLACEHOLDERS.problem != PlaceholderProblem::DollarQuote,
                "the raw statement has $ that is not a placeholder: dollar-quoted text is not supported, write the string with quotes");
  static_assert(PLACEHOLDERS.problem != PlaceholderProblem::Unterminated, "the raw statement has a string, quoted name or comment that never ends");
};

namespace detail {

template <typename type_t>
struct is_raw_statement : std::false_type {};

template <util::CTString sql_text, typename arguments_t, schema::IsField... field_ts>
struct is_raw_statement<Raw_Statement<sql_text, arguments_t, field_ts...>> : std::true_type {};

template <typename type_t>
struct is_bound_raw_statement : std::false_type {};

template <typename statement_t>
struct is_bound_raw_statement<Bound_Raw_Statement<statement_t>> : std::true_type {};

}  // namespace detail

/** @brief Detects whether `type_t` is a `Raw_Statement<...>` specialization. */
template <typename type_t>
concept IsRawStatement = detail::is_raw_statement<type_t>::value;

/** @brief Detects whether `type_t` is a `Bound_Raw_Statement<...>`: a raw statement with its arguments. */
template <typename type_t>
concept IsBoundRawStatement = detail::is_bound_raw_statement<type_t>::value;

}  // namespace schemacht::query
