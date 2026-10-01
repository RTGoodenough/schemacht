#pragma once

/**
 * What the backends share, without a database: `Field_Codec`, the text and binary form of each column type, and the query that looks rows up by key values.
 *
 * The Postgres backends are in postgres.hpp, apart from this because they bring in libpqxx and libpq's headers. Includes
 * schema.hpp and query.hpp.
 */

#include "schemacht/postgres/field_codec.hpp"  // IWYU pragma: export
#include "schemacht/postgres/key_lookup.hpp"   // IWYU pragma: export
#include "schemacht/query.hpp"                 // IWYU pragma: export
#include "schemacht/schema.hpp"                // IWYU pragma: export
