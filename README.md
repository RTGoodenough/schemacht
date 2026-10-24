# AI Disclosure

The code and docs were written with the help of an AI assistant (Claude).

# schemacht

schemacht is a C++20 library for PostgreSQL. A table is described once, as a C++ type. Queries, updates and deletes are
written in C++ against that type and turned into SQL while compiling, so a misspelled column or an argument of the
wrong type is a compile error. A `Database` runs the statements on a pool of non-blocking libpq connections and returns
typed rows through `std::future`s or completions.

## What it does

- **Schemas as types:** `sch::Schema<"people", Field...>`; a row is a tuple of its fields, read with `sch::get<"name">`.
- **Statements checked while compiling:** `select`, `update`, `delete_where` with comparisons, `in`, `IS NULL`, `LIKE`,
  `AND`/`OR`/`NOT`, `order_by`, `limit`, `project` and `returning`; `q::Prepared<Q>::SQL` is the SQL text.
- **Inserts:** `insert` any number of rows in one statement, `insert_returning` for what the database filled in, `upsert`
  matched on the primary key or a unique column, with each column overwritten, merged by a SQL function or operator, or
  kept.
- **Transactions:** `begin` with an isolation level or read-only; a transaction dropped without a commit is rolled back.
- **Streaming:** `stream` a large result a row at a time, or `stream_chunks` a chunk at a time, with backpressure.
- **Futures or completions:** every call returns a `std::future`, or calls a completion given last; every error arrives
  there, exactly once.
- **SQL the builder cannot write:** `q::RawStatement` with its `$n` placeholders checked while compiling, and
  `pg::unchecked_sql` for text built while the program runs.
- **Column types:** the built-in ones (numbers, text, bytes, arrays, `std::chrono`, `Uuid`, `json::Json`) and types of
  your own through a `ColumnType` specialization.
- **An extension's operators and functions** (pg_trgm, PostGIS, ...) in queries, as operation types.
- **Tables named while the program runs** (`sch::Table<Schema>`), and a check of a table against its schema
  (`table_differences`).

## Requirements

- A C++20 compiler: tested with g++ 13 and clang++ 18 (the headers compile as C++23 too).
- libpq 17 or newer (`libpq-dev` from the [PostgreSQL apt repository](https://apt.postgresql.org)).
- CMake 3.21 or newer.
- Optional: Doxygen and Graphviz, for the API reference.

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

Linking `schemacht::schemacht` also asks for C++20 (or later) on your target, and links libpq and threads.

## A first program

[examples/first_program.cpp](examples/first_program.cpp), run with a libpq connection string as its argument:

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
using People = sch::Schema<"schemacht_example_people", Id, Name, Email, Score>;
using Col = q::On<People>;

// Statements, written in C++ and turned into SQL while compiling; arg<N>() is a value given when one runs.
constexpr auto AT_LEAST = q::select(Col::col<"score">() >= q::arg<0>()).order_by(Col::col<"score">().desc()).limit(10);
constexpr auto SET_SCORE = q::update<People>().set(Col::col<"score">(), q::arg<0>()).where(Col::col<"id">() == q::arg<1>());

int main(int argc, char** argv) {
  pg::Database database(argc > 1 ? argv[1] : "");  // a pool of connections
  database.drop_table<People>().get();             // if it is there from a run before
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

  database.drop_table<People>().get();
}
```

`q::Prepared<AT_LEAST>::SQL` is the statement as a compile-time string:
`SELECT "id", "name", "email", "score" FROM "schemacht_example_people" WHERE "score" >= $1 ORDER BY "score" DESC LIMIT 10`.

## Examples

Each example is a program under [examples/](examples/), run with a connection string as its argument. Each makes its
own tables and drops them afterwards.

| Example | Shows |
|---|---|
| `first_program` | the program above |
| `basic` | a schema, a table made from it, queries with arguments, a raw statement, a table named while the program runs |
| `custom_types` | column types of your own: a wrapped integer, enums, a converted type, a `numeric` written by hand |
| `transfers` | identity keys, `insert_returning`, update and delete with `returning`, a unique column, an upsert, transactions, chained completions |
| `extensions` | pg_trgm's operators and a function of the application's own, jsonb's operators, an operation with an argument type of its own, a test of an expression of a column |

## Building and testing

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build
```

The tests and examples are built when schemacht is the top-level project (`SCHEMACHT_BUILD_TESTS`,
`SCHEMACHT_BUILD_EXAMPLES`). The tests that need a database, and the examples, skip themselves unless
`SCHEMACHT_TEST_DB` holds a libpq connection string:

```sh
SCHEMACHT_TEST_DB="host=localhost port=5432 dbname=schemacht_test user=test" ctest --test-dir build -j8
```

Point it at a throwaway database. The tests and examples create and drop objects there: tables named
`schemacht_it_*` and `schemacht_example_*`, the schemas `schemacht_it_schema`, `schemacht_it_upsert`,
`schemacht_it_check` and `schemacht_it_check_domains` (with domains in it), the view `schemacht_it_check_view`, the
sequence `schemacht_it_check_missing`, the functions `similar_above`, `schemacht_it_keep_longer` and
`schemacht_it_upsert.add_totals`, and the type `schemacht_example_mood`. One integration test and the `extensions`
example install `pg_trgm` if they can, and leave it installed; without it, that part is skipped.

Besides the unit and integration tests there are compile-fail tests (each compiles one case that must fail with a given
message) and a fault test (`schemacht_fault_tests`, which fails each memory allocation in turn and checks that every
completion still runs exactly once, or never when the call threw).

## Documentation

- [docs/guide.md](docs/guide.md): using schemacht, from describing a table to an extension's operators and a table's
  check against its schema.
- [docs/developer.md](docs/developer.md): how it is put together, the path of a call, threads and the error guarantee,
  how to add to it, how to build, test and lint.
- The API reference is generated from the headers with Doxygen: `cmake --build build --target docs`, then open
  `build/docs/html/index.html`. The headers carry the same comments.

## Status and license

Version 0.1: the API may still change between minor versions. MIT license, see [LICENSE](LICENSE).
