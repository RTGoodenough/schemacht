#pragma once

/**
 * Schemas: `Field`, `KeyRole` and `Schema`, and reading a row's values by column name (`get`).
 *
 * @code
 * using Id = schemacht::schema::Field<std::int64_t, "id", schemacht::schema::KeyRole::Primary>;
 * using Name = schemacht::schema::Field<std::string, "name">;
 * using PeopleSchema = schemacht::schema::Schema<"people", Id, Name>;
 * @endcode
 *
 * Part of schemacht.hpp; include this one alone to describe a table without the rest.
 */

#include "schemacht/schema/field.hpp"   // IWYU pragma: export
#include "schemacht/schema/schema.hpp"  // IWYU pragma: export
