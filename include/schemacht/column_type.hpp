#pragma once

/**
 * What adding a column type of your own takes (json/json.hpp is a complete example):
 *
 * - Specialize `postgres::FieldCodec<T>` with `parse` and `format`: the type's text form.
 * - Add `parse_binary` to it (and optionally `binary_accepts`) so results can be read in PostgreSQL's binary form, the
 *   default; without it, rows with the type are read as text.
 * - Specialize `query::sql::SqlType<T>` with its SQL type name, so list arguments and `Database::insert` can cast to it.
 * - Give the type `operator<=>` if queries should order by it.
 */

#include "schemacht/postgres/field_codec.hpp"  // IWYU pragma: export
#include "schemacht/query/sql_type.hpp"        // IWYU pragma: export
#include "schemacht/schema/field.hpp"          // IWYU pragma: export
