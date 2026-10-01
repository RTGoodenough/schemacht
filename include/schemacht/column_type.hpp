#pragma once

/**
 * What adding a column type of your own takes (as json.hpp and spatial.hpp do): specialize `backend::Field_Codec<T>` (its text
 * form) and `query::sql::SQL_Type<T>` (its SQL type), and give the type `operator<=>`. The codec also needs `parse_binary` (and may
 * supply `binary_accepts`) for a query that returns the type through an asynchronous backend, which reads results in PostgreSQL's binary form.
 */

#include "schemacht/postgres/field_codec.hpp"  // IWYU pragma: export
#include "schemacht/query/sql_type.hpp"        // IWYU pragma: export
#include "schemacht/schema/field.hpp"          // IWYU pragma: export
