#pragma once

/**
 * Everything: schemas, queries, the Postgres backend and `json::Json`.
 *
 * The pieces, to include separately when compile time matters: schema.hpp, query.hpp, backend.hpp (no libpq),
 * postgres.hpp and json.hpp. column_type.hpp is for adding a column type of your own.
 */

#include "schemacht/backend.hpp"   // IWYU pragma: export
#include "schemacht/json.hpp"      // IWYU pragma: export
#include "schemacht/postgres.hpp"  // IWYU pragma: export
#include "schemacht/query.hpp"     // IWYU pragma: export
#include "schemacht/schema.hpp"    // IWYU pragma: export
