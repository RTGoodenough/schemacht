#pragma once

/**
 * @file
 * The Postgres backend: `Database`, a pool of connections (an `AsyncClient`) that runs queries and raw statements and
 * returns `std::future`s, `Transaction`, plus `SessionSettings` and `QueryError`. Includes backend.hpp.
 */

#include "schemacht/backend.hpp"                     // IWYU pragma: export
#include "schemacht/postgres/async_client.hpp"       // IWYU pragma: export
#include "schemacht/postgres/database.hpp"           // IWYU pragma: export
#include "schemacht/postgres/session_settings.hpp"   // IWYU pragma: export
#include "schemacht/postgres/statement_runner.hpp"   // IWYU pragma: export
#include "schemacht/postgres/statements.hpp"         // IWYU pragma: export
#include "schemacht/postgres/table_differences.hpp"  // IWYU pragma: export
#include "schemacht/postgres/transaction.hpp"        // IWYU pragma: export
