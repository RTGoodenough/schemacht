#pragma once

/**
 * @file
 * Everything: schemas, queries, the Postgres backend, `json::Json` and `Uuid`.
 *
 * The pieces, to include separately when compile time matters: schema.hpp, query.hpp, backend.hpp (no libpq),
 * postgres.hpp, json.hpp and uuid.hpp. column_type.hpp is for adding a column type of your own.
 */

#include "schemacht/backend.hpp"   // IWYU pragma: export
#include "schemacht/json.hpp"      // IWYU pragma: export
#include "schemacht/postgres.hpp"  // IWYU pragma: export
#include "schemacht/query.hpp"     // IWYU pragma: export
#include "schemacht/schema.hpp"    // IWYU pragma: export
#include "schemacht/uuid.hpp"      // IWYU pragma: export

/** @namespace schemacht The library: `ColumnType` and the column types it ships (`Uuid`, `json::Json`), and the namespaces below. */
/** @namespace schemacht::schema Tables as types: `Field`, `Schema`, rows and `get`, and `Table` for a table named while the program runs. */
/** @namespace schemacht::query Statements as compile-time values: `On`, `select`, `update`, `delete_where`, `Prepared`, `RawStatement`. */
/** @namespace schemacht::query::sql The SQL writer: `Text` and the consteval functions that lower a statement to it. */
/** @namespace schemacht::postgres Running statements on PostgreSQL: `Database`, `Transaction`, `AsyncClient`, the codecs and the table check. */
/** @namespace schemacht::async The thread pool the completions run on. */
/** @namespace schemacht::util Compile-time strings, identifier checks, completions and futures, `MoveOnlyFunction`. */
/** @namespace schemacht::json `Json`, the `jsonb` column type, and `contains`. */

/** @example first_program.cpp A schema, a table made from it, rows in, a query with an argument, and a transaction. */
/** @example basic.cpp A schema, queries with arguments, a raw statement, and a table named while the program runs. */
/** @example custom_types.cpp Column types of your own, with each helper of column_type.hpp and one written by hand. */
/** @example transfers.cpp Identity keys, `insert_returning`, `returning` on an update and a delete, an upsert, transactions and chained completions. */
/** @example extensions.cpp An extension's operators and functions, an argument type of its own, and a test of an expression of a column. */
