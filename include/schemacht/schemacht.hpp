#pragma once

/**
 * The whole core: schemas, queries, the backends (Postgres) and `json::Json`.
 *
 * This is the one to include when compile time is not the concern. The pieces are schema.hpp, query.hpp, backend.hpp (no
 * database), postgres.hpp and json.hpp; column_type.hpp is for adding a column type. The modules are apart, because they need
 * a library of their own: spatial.hpp and gis.hpp (needs GDAL).
 */

#include "schemacht/backend.hpp"   // IWYU pragma: export
#include "schemacht/json.hpp"      // IWYU pragma: export
#include "schemacht/postgres.hpp"  // IWYU pragma: export
#include "schemacht/query.hpp"     // IWYU pragma: export
#include "schemacht/schema.hpp"    // IWYU pragma: export
