# schemacht

schemacht is a C++23 library for PostgreSQL. You describe a table once, as a C++ type. Queries, updates and deletes are
then written in C++ against it and turned into SQL while compiling. A misspelled column, or an argument of the wrong type,
is a compile error rather than a failure at run time. A `Database` runs the statements on a pool of non-blocking libpq
connections and returns typed rows through `std::future`s.

## Requirements

- A C++23 compiler: tested with g++ 13 and clang++ 18.
- libpq 17 or newer (`libpq-dev` from the [PostgreSQL apt repository](https://apt.postgresql.org)).
- CMake 3.21 or newer.

## Adding it to a CMake project

Either build it as part of your project:

```cmake
add_subdirectory(schemacht)
target_link_libraries(app PRIVATE schemacht::schemacht)
```

or install it (`cmake --install <build> --prefix <dir>`) and find it:

```cmake
find_package(schemacht 0.1 REQUIRED)
target_link_libraries(app PRIVATE schemacht::schemacht)
```

Linking `schemacht::schemacht` also sets C++23 for your target, and links libpq and threads.

## Quick start

```cpp
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>

#include "schemacht/schemacht.hpp"

namespace sch = schemacht::schema;
namespace q = schemacht::query;
namespace pg = schemacht::postgres;

// A table, described once: each column's C++ type and name.
using Id = sch::Field<std::int64_t, "id", sch::KeyRole::Primary, true>;  // true: the database numbers it
using Name = sch::Field<std::string, "name">;
using Email = sch::Field<std::optional<std::string>, "email">;  // a std::optional column can be NULL
using Score = sch::Field<double, "score">;
using People = sch::Schema<"people", Id, Name, Email, Score>;
using Col = q::On<People>;

// Statements, written in C++ and turned into SQL while compiling; arg<N>() is a value given when one runs.
constexpr auto AT_LEAST = q::select(Col::col<"score">() >= q::arg<0>()).order_by(Col::col<"score">().desc()).limit(10);
constexpr auto SET_SCORE = q::update<People>().set(Col::col<"score">(), q::arg<0>()).where(Col::col<"id">() == q::arg<1>());

int main() {
  pg::Database database("host=localhost dbname=app user=app");  // a pool of connections
  database.drop_table<People>().get();                          // if it is there from a run before
  database.create_table<People>().get();

  // Every call returns a std::future; insert_returning gives back what the database filled in, such as the ids.
  const auto added = database
                         .insert_returning<People>({
                             People::make_insert_row("ann", "ann@example.com", 91.5),
                             People::make_insert_row("bob", std::nullopt, 78.0),
                         })
                         .get();

  for ( const People::row_type& person : database.execute(q::Prepared<AT_LEAST>::bind(80.0)).get() ) {
    std::cout << sch::get<"name">(person) << ": " << sch::get<"score">(person) << '\n';
  }

  // Changes made together, or not at all.
  pg::Transaction transaction = database.begin().get();
  for ( const People::row_type& person : added ) {
    transaction.execute(q::Prepared<SET_SCORE>::bind(0.0, sch::get<"id">(person))).get();
  }
  transaction.commit().get();
}
```

`q::Prepared<AT_LEAST>::SQL` is the statement as a compile-time string:
`SELECT "id", "name", "email", "score" FROM "people" WHERE "score" >= $1 ORDER BY "score" DESC LIMIT 10`.

## What else it does

- **Queries:**
  - comparisons, `in(...)`, `IS NULL`, `AND`/`OR`/`NOT`, `LIKE`;
  - `order_by`, `limit`, and `project` to fetch only some columns;
  - `stream` to handle a large result a chunk at a time, with backpressure;
  - every call returns a `std::future`, or, given a callback last, calls it instead, to chain the next step:
    `database.execute(query, [](std::vector<Row> rows, std::exception_ptr error) { ... })`;
  - every error from making and running a statement arrives through the future or the callback, which runs exactly once; a
    call throws only if it can't start at all (no memory to hand the statement on, say), and then its callback never runs.
- **Changes:**
  - `update(...)` and `delete_where(...)` give the count of rows changed, or the rows themselves with `.returning(...)`;
  - `insert` writes any number of rows atomically, in one statement: all of them or none;
  - `upsert` does the same, updating the row already there for a key that is (`ON CONFLICT` on the primary key): each
    column overwritten, merged with the value already there by a SQL function or operator you name, or kept,
    `database.upsert<PageViews, pg::MergeWith<"views", Add>, pg::Keep<"first_seen">>(rows)`.
- **Transactions:** `Database::begin` (with an isolation level, or read-only). A transaction that is dropped without a
  commit is rolled back.
- **Raw SQL with types:** `q::RawStatement<"SELECT ...", q::RawArguments<...>, Fields...>` checks its `$n` placeholders
  against its arguments while compiling. For SQL that can only be written while the program runs (DDL on a table named
  then, say), `pg::unchecked_sql(text, arguments...)` runs it with its arguments bound to `$1`, `$2`, ..., checking nothing.
- **Column types:**
  - `bool`, integers, `float` and `double`, `std::string`, bytes (`bytea`), `std::vector` (arrays);
  - dates and times (`std::chrono`), `schemacht::Uuid`, `schemacht::json::Json` (`jsonb`);
  - any type of your own, with a `schemacht::ColumnType` specialization: see
    [include/schemacht/column_type.hpp](include/schemacht/column_type.hpp) and
    [examples/custom_types.cpp](examples/custom_types.cpp).
- **An extension's operators and functions** (pg_trgm, PostGIS, ...): `col.apply<Op>(arg)` for a test, and
  `col.ranked_by<Op>(arg)` to sort by one. An operation's argument can be of a type other than the column's, with
  `using argument_type = T;`, where `T` is a column type of your own (a `ColumnType` specialization): a text key for
  jsonb's `?`, say. See [examples/extensions.cpp](examples/extensions.cpp).
- **Tables from schemas:** `create_table` and `drop_table`, for tests, examples and a first start. They cover the primary
  key, `UNIQUE` columns and an identity key.
- **Tables named while the program runs:** a schema's name is its default table. A `sch::Table<Schema>` names another of
  its shape (checked once, when made, and optionally in a PostgreSQL schema: `"gis"."roads"`), for a name from
  configuration, a user, or another language's bindings. Any statement of the schema runs there once aimed at it:
  `database.execute(query.on(archive))`, `database.execute(pg::insert_statement<Schema>(rows).on(archive))`.

The examples show these together:
- [examples/basic.cpp](examples/basic.cpp): a schema, an insert, queries, a raw statement, and a table named while the program
  runs;
- [examples/custom_types.cpp](examples/custom_types.cpp): column types of your own;
- [examples/transfers.cpp](examples/transfers.cpp): identity keys, `insert_returning`, update and delete with
  `returning`, a unique column, an upsert, and transactions;
- [examples/extensions.cpp](examples/extensions.cpp): pg_trgm's operators and a function of the application's own, jsonb's
  operators, and an operation whose argument is not of the column's type.

## Building and testing

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build
```

The tests and examples are built when schemacht is the top-level project (`SCHEMACHT_BUILD_TESTS`,
`SCHEMACHT_BUILD_EXAMPLES`). The integration tests and the examples need a PostgreSQL database to run against. Point
`SCHEMACHT_TEST_DB` at a throwaway one: they create and drop tables whose names start with `schemacht_it_` or
`schemacht_example_`, and the schemas `schemacht_it_schema` and `schemacht_it_upsert`. One integration test and the extensions example also install
pg_trgm if they can, and leave it installed; they create and drop the functions `similar_above` and
`schemacht_it_keep_longer`. Without
`SCHEMACHT_TEST_DB`, they are skipped.

```sh
SCHEMACHT_TEST_DB="host=localhost port=5432 dbname=schemacht_test user=test" ctest --test-dir build -j8
```

## Status

Version 0.1: the API may still change between minor versions.
