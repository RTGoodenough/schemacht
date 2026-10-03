#pragma once

#include <concepts>

#include "schemacht/schema/table_name.hpp"

namespace schemacht::postgres {

/**
 * @brief Whether `statement_t` is a query, update or delete written against `schema_t`, which a `Table<schema_t>` can run on its
 * own table (`on`). A raw statement names its tables itself, so it is not one.
 */
template <typename statement_t, typename schema_t>
concept StatementOn = requires(const statement_t& statement, const schema::TableName& table) {
  requires std::same_as<typename statement_t::schema_type, schema_t>;
  { statement.on(table) } -> std::same_as<statement_t>;
};

}  // namespace schemacht::postgres
