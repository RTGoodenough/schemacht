#pragma once

/**
 * Writing queries: the predicate builder (`query::On`, `col`, `arg`, `select`), the compile-time statement (`query::Prepared`),
 * `query::like`, and the typed raw statement (`query::RawStatement`) for what the builder cannot say.
 *
 * Nothing here touches a database or a backend: a backend runs a query (see backend.hpp). Includes schema.hpp.
 */

#include "schemacht/query/compiled_query.hpp"  // IWYU pragma: export
#include "schemacht/query/predicate.hpp"       // IWYU pragma: export
#include "schemacht/query/prepared.hpp"        // IWYU pragma: export
#include "schemacht/query/query.hpp"           // IWYU pragma: export
#include "schemacht/query/raw_statement.hpp"   // IWYU pragma: export
#include "schemacht/query/row_sink.hpp"        // IWYU pragma: export
#include "schemacht/query/text.hpp"            // IWYU pragma: export
#include "schemacht/schema.hpp"                // IWYU pragma: export
