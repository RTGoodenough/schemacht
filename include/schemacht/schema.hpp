#pragma once

/**
 * Schemas: `Field`, `KeyRole` and `Schema`, reading a row's values by column name (`get`), and `Table` (with its `TableName`)
 * for a table of a schema's shape named while the program runs.
 *
 * Part of schemacht.hpp; include this one alone to describe a table without the rest.
 */

#include "schemacht/schema/field.hpp"       // IWYU pragma: export
#include "schemacht/schema/schema.hpp"      // IWYU pragma: export
#include "schemacht/schema/table.hpp"       // IWYU pragma: export
#include "schemacht/schema/table_name.hpp"  // IWYU pragma: export
