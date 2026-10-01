#pragma once

/**
 * The Postgres backend: `Database`, a pool of connections (an `Async_Client`) that every schema reads and writes through, running queries and raw
 * statements with `std::future`s; and `Connection` (libpqxx, for setup and scripts), `SessionSettings` and `Query_Error`. Includes backend.hpp.
 *
 * Brings in libpqxx's headers (a large part of what schemacht costs to compile), so a file that only needs schemas, queries or
 * the backend interfaces is quicker to compile with just those.
 */

#include "schemacht/backend.hpp"                    // IWYU pragma: export
#include "schemacht/postgres/async_client.hpp"      // IWYU pragma: export
#include "schemacht/postgres/connection.hpp"        // IWYU pragma: export
#include "schemacht/postgres/database.hpp"          // IWYU pragma: export
#include "schemacht/postgres/session_settings.hpp"  // IWYU pragma: export
