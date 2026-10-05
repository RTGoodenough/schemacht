#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

#include "schemacht/util/identifier.hpp"

namespace schemacht::schema {

/**
 * @brief A table's name chosen while the program runs, for running a schema's statements on a table other than its own (see
 * `Table`): from configuration, a user, or another language's bindings.
 *
 * Checked once, when made, by the rule a `Schema`'s own name follows (`util::is_identifier`): lowercase letters, digits and
 * underscores, not starting with a digit, at most 63 bytes. So it is written into a statement between double quotes, with
 * nothing to escape, and no later use of it can fail.
 */
class TableName {
 public:
  /** @throws std::invalid_argument if `name` is not a valid table name. */
  explicit TableName(std::string_view name) : _name(checked(name)), _quoted(quote(name)) {}

  /**
   * @brief A table in a schema other than the search path's: `"schema_name"."name"`.
   * @throws std::invalid_argument if either part is not a valid name.
   */
  TableName(std::string_view schema_name, std::string_view name)
      : _schema_name(checked(schema_name)), _name(checked(name)), _quoted(quote(schema_name) + '.' + quote(name)) {}

  [[nodiscard]] const std::string& name() const noexcept { return _name; }

  /** @return The schema the table is in, or empty if it is found through the search path. */
  [[nodiscard]] const std::string& schema_name() const noexcept { return _schema_name; }

  /** @return The name as a statement writes it: `"name"`, or `"schema_name"."name"`. */
  [[nodiscard]] const std::string& quoted() const noexcept { return _quoted; }

  [[nodiscard]] bool operator==(const TableName&) const = default;

 private:
  std::string _schema_name;
  std::string _name;
  std::string _quoted;

  [[nodiscard]] static std::string checked(std::string_view part) {
    if ( ! util::is_identifier(part) ) {
      throw std::invalid_argument(
          "'" + std::string(part) +
          "' is not a valid table or schema name: lowercase letters, digits and underscores, not starting with a digit, at most 63 bytes"
      );
    }

    return std::string(part);
  }

  [[nodiscard]] static std::string quote(std::string_view part) { return '"' + std::string(part) + '"'; }
};

}  // namespace schemacht::schema
