#pragma once

#include <concepts>
#include <cstddef>
#include <string>
#include <string_view>

#include "schemacht/query/predicate.hpp"
#include "schemacht/schema/schema.hpp"

/**
 * Text tests for queries. Each is one operation, written as the SQL operator a database answers from an index; none has
 * .
 */
namespace schemacht::query {

/**
 * @return `text` with every `%`, `_` and `\` escaped, so that as a `LIKE` pattern it matches only itself. Build a prefix
 * test as `escape_like(prefix) + "%"`.
 */
[[nodiscard]] inline std::string escape_like(std::string_view text) {
  std::string escaped;
  escaped.reserve(text.size());
  for ( const char chr : text ) {
    if ( chr == '%' || chr == '_' || chr == '\\' ) {
      escaped.push_back('\\');
    }
    escaped.push_back(chr);
  }
  return escaped;
}

/** @brief The text matches a `LIKE` pattern: `"column" ~~ pattern` (`~~` is the operator `LIKE` is written as). */
class Like {
 public:
  static constexpr std::string_view OPERATOR = "~~";
};

/**
 * @brief Rows whose text column matches the `LIKE` pattern bound to `arg<N>()`:
 *
 * @code
 * constexpr auto NAMED = query::select(query::like(Roads::col<"name">(), query::arg<0>()));
 * roads.execute(query::Prepared<NAMED>::bind(query::escape_like("Main") + "%"));   // starts with "Main"
 * @endcode
 *
 * `%` matches any run of characters, `_` any one character, and a backslash makes the next character literal.
 * Written as the operator, so a `btree (name text_pattern_ops)` index answers a pattern with a fixed start (`Main%`), and a
 * trigram index (`pg_trgm`) any pattern. A leading `%` cannot use a btree. Negate with `!` for `NOT LIKE`.
 */
template <schema::IsSchema schema_t, std::size_t column_idx, std::size_t slot_idx>
  requires std::same_as<typename Column_Ref<schema_t, column_idx>::data_type, std::string>
[[nodiscard]] constexpr auto like(const Column_Ref<schema_t, column_idx>& column, Arg_Ref<slot_idx> arg) {
  return column.template apply<Like>(arg);
}

}  // namespace schemacht::query
