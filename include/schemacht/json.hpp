#pragma once

/**
 * `json::Json`, the `jsonb` column type, and `json::contains`, the `@>` test on it. Includes query.hpp (and through it schema.hpp),
 * which is what a query with `json::contains` is written with.
 */

#include "schemacht/json/json.hpp"   // IWYU pragma: export
#include "schemacht/json/query.hpp"  // IWYU pragma: export
#include "schemacht/query.hpp"       // IWYU pragma: export
