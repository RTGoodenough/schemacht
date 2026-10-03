#pragma once

#include <string_view>
#include <utility>

#include "schemacht/schema/concepts/schema.hpp"
#include "schemacht/schema/table_name.hpp"

namespace schemacht::schema {

/**
 * @brief A table of `schema_t`'s shape whose name is chosen while the program runs: one per tenant or per layer, a dated copy,
 * a name from configuration or from another language's bindings. A schema's own name is its default table; a statement written
 * against it runs on this one instead once aimed at it (`on`), its columns checked while compiling as ever.
 *
 * Only `schema_t`'s statements can be aimed at it: a query, update, delete or insert of another schema is a compile error, and
 * a raw statement names its own tables. It is only a name: it runs nothing itself, so one serves a database and every
 * transaction alike.
 */
template <IsSchema schema_t>
class Table {
 public:
  using schema_type = schema_t;

  explicit Table(TableName name) : _name(std::move(name)) {}

  /** @throws std::invalid_argument if `name` is not a valid table name (see `TableName`). */
  explicit Table(std::string_view name) : _name(name) {}

  /**
   * @brief A table in a schema other than the search path's: `"schema_name"."name"`.
   * @throws std::invalid_argument if either part is not a valid name.
   */
  Table(std::string_view schema_name, std::string_view name) : _name(schema_name, name) {}

  [[nodiscard]] const TableName& name() const noexcept { return _name; }

  [[nodiscard]] bool operator==(const Table&) const = default;

 private:
  TableName _name;
};

}  // namespace schemacht::schema
