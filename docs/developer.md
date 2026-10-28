# Working on schemacht

The user guide is [guide.md](guide.md); the headers document each type and call.

## Layout

```
include/schemacht/
  schemacht.hpp          umbrella: everything below
  schema.hpp             Field, KeyRole, Schema, get, Table, TableName
  query.hpp              On, col, arg, select, update, delete_where, Prepared, like, RawStatement (includes schema.hpp)
  backend.hpp            FieldCodec, row reading, by_key_values: what a backend needs, without libpq's headers
  postgres.hpp           Database, Transaction, AsyncClient, statements, the table check
  json.hpp, uuid.hpp     json::Json (jsonb) and json::contains; Uuid
  column_type.hpp        ColumnType helpers for types of your own: WrappedColumn, EnumColumn, ...
  util/                  CTString, identifier checks, MoveOnlyFunction, futures and completions, hex
  async/                 ThreadPool
  schema/                Field, Schema, Table, the ColumnType primary template
  query/                 predicates, Query/Update/Delete, Prepared, CompiledQuery, sql.hpp (the SQL writer), RawStatement
  postgres/              codecs (field_codec, binary_form, numeric, chrono_codec), row_reader, key_lookup, statements,
                         statement_runner, database, transaction, async_client, table_differences, session_settings,
                         and their concepts/
  json/                  the Json column type and its query operation
  concepts/, */concepts/ the concepts, beside the headers that use them
src/                     the compiled part: the libpq client, the thread pool, the codecs that need no templates
tests/
  compile/               STATIC_REQUIREs: the SQL text each statement lowers to, schema facts, the calls' return types
  compile_fail/          cases that must not compile, each checked for its message (cases.cpp, one #if per case)
  unit/                  no database: codecs, row reading, binding, the thread pool, utilities
  integration/           need SCHEMACHT_TEST_DB
  fault/                 allocation failures, with operator new replaced (its own executable)
  support/               test_db() and exec(), shared schemas and operations, a contains helper
examples/                five programs, run as tests against SCHEMACHT_TEST_DB
cmake/                   the installed package's config
```

## Layers

Each directory depends only on the ones before it:

```
util  ←  schema  ←  query  ←  postgres  ←  json, uuid.hpp, column_type.hpp
util  ←  async   ←  src/postgres/async_client.cpp
```

- `util` includes nothing else of the library. `schema` uses `util` only. `query` uses `schema` and `util`, and never
  `postgres`: writing a statement touches no database and no libpq. `postgres` uses all three. The type layers sit on
  `postgres`'s codecs.
- `async/thread_pool.hpp` is used only from `src/postgres/async_client.cpp`, not from any header.
- `src/postgres/async_client.cpp` is the one file that includes `libpq-fe.h`. `postgres/async_client.hpp` forward-declares
  `pg_result` and holds its state behind a pointer, so a program that includes `schemacht.hpp` compiles without libpq's
  headers and links libpq through the library.
- Two places bend the layering: `postgres/table_differences.hpp` includes the top-level `column_type.hpp` for
  `EnumColumn` (which includes `postgres/` headers, a loop between directories, not between files), and a few concept
  headers include full headers (`query/concepts/command.hpp` includes `query/predicate.hpp`).

## How it fits together

### Schema

`schema::Field<T, "name", KeyRole, generated>` (`schema/field.hpp`) is one column. `T` in a `std::optional` makes the
column nullable: `data_type` is what a row holds, `value_type` the type inside. `schema::Schema<"table", Field...>`
(`schema/schema.hpp`) is a table: `row_type` is a `std::tuple` of the fields, `insert_row_type` the tuple without the
generated ones, and it answers `field_index<"name">()`, `column_names()`, `PRIMARY_KEY_FIELD_COUNT` and the like at
compile time. `schema::get<"name">(row)` finds the field by name. `schema::Table<Schema>` (`schema/table.hpp`) is a
run-time table name tagged with its schema, checked once by `util::is_identifier` and kept quoted.

### Statements as compile-time values

Everything in `query/` is a structural type, so a whole statement can be a template argument:

- `query::On<S>::col<"x">()` is a `ColumnRef<S, index>`; its operators return a `Predicate<S, capacity>`
  (`query/predicate.hpp`): a flat `std::array<Node, capacity>` with children before parents, so `&&`, `||` and `!`
  copy node arrays and the capacity is a template parameter that grows as tests are combined. A `Node` for an
  extension's test points to a static `Operation` (`detail::BoundOperation<Op>::VALUE`); an `OrderTerm` with a
  ranking points to a static `Ranking`.
- `Query` (`query/query.hpp`), `Update` and `Delete` (`query/command.hpp`) add the select list, order, limit, sets and
  returning. A builder call that cannot be right (`limit(-1)`, a column set twice) throws, which inside a constant
  expression is a compile error naming the message.
- `Prepared<STATEMENT>` (`query/prepared.hpp`) lowers it: `collect_slots` types each `arg<N>()` from the column it is
  used with (and refuses gaps and conflicting uses), `SQL` is written by `query/sql.hpp`, and `bind(args...)` checks each
  argument against its slot and returns a `TypedQuery` over a `CompiledQuery<S>` (`query/compiled_query.hpp`): a view of
  the static SQL, the `TableSlot`, the projected columns and a `std::vector<Param<S>>`. `Param` is a `std::variant` of
  the schema's value types, their vectors, `std::int64_t` (a limit), `double` (an operation's number) and
  `ArgumentText` (an `argument_type`, already formatted), so the code that sends a statement is compiled once per
  schema, not once per statement.
- `query/sql.hpp` is consteval: `sql::Text<capacity>` is a fixed-capacity string that remembers where it wrote the
  table name (`append_table`, `table_slot()`; `on_table` is the one run-time function in the file, used by `.on(table)`),
  `detail::Emitter` writes a predicate tree with three precedence levels, and `select_statement`, `update_statement`,
  `delete_statement`, `create_table_statement`, `insert_unnest_statement`, `upsert_statement`,
  `table_differences_statement` and `drop_table_statement` write each kind. Names and operators were validated where
  they were declared; `append_identifier`, `append_function` and `append_operator` check again and, on a miss, call a
  deliberately non-constexpr function (`detail::sql_identifier_not_validated()`), which makes the compile error name it.
- `RawStatement<"SQL", RawArguments<...>, Field...>` (`query/raw_statement.hpp`) scans its text for `$n` while compiling
  (`check_placeholders`) and binds to a `BoundRawStatement`; the result's column names are checked when it runs.

### Codecs and rows

`ColumnType<T>` (`schema/column_type.hpp`, primary template undefined) is the user-facing specialization point;
`postgres::FieldCodec<T>` (`postgres/field_codec.hpp`) is the codec for built-in types and forwards to `ColumnType` for
the rest: `parse` (text), `format`, and optionally `parse_binary` and `binary_accepts`. `query::sql::SqlType<T>` gives
a type's SQL name (`query/sql_type.hpp`). `BinaryReader` (`postgres/binary_form.hpp`) reads PostgreSQL's big-endian
binary fields; `postgres/numeric.hpp` handles `numeric`; `postgres/chrono_codec.hpp` the dates and times. Rows are read
by free functions in `postgres/row_reader.hpp`: `read_row`, `read_rows` (one row at a time to a handler) and
`read_chunk` (a vector), each field through `parse_column` or `parse_binary_column`, with the column's name in any
error. The concepts `TextRow` and `BinaryReadableRow` say whether a row type can be read in each form; a row with a
type that has no binary form is read as text whatever was asked.

### Running statements

- `postgres/statements.hpp` turns statement values into what is sent. `SchemaStatement<S, result>` holds the SQL text,
  the `TableSlot`, the parameter texts and, if a codec refused a value while it was made, the error; only
  `detail::SchemaStatementMaker` constructs one, through `insert_statement`, `insert_returning_statement`,
  `upsert_statement`, `create_table_statement`, `drop_table_statement` and `table_differences_statement`.
  `UncheckedSql` holds run-time text and bound arguments. `detail::wire(statement)` gives the `WireStatement` (SQL,
  parameters, column names) for any `SendableStatement`; `detail::wire_caught` (in `statement_runner.hpp`) is
  `noexcept` and catches whatever wiring throws into a `Wired{statement, error}`.
- `StatementRunner` (`postgres/statement_runner.hpp`) is the base of `Database` and `Transaction`: `execute`, `stream`,
  `stream_chunks`, the conveniences (`insert`, `upsert`, `create_table`, `select_by_key`, ...), each taking an optional
  completion last through `util::complete` (`util/futures.hpp`), which gives a `std::future` for `util::UseFuture` and
  hands any other completion on. Three private senders do the work: `send_for_rows` (collects a stream into a vector),
  `send_stream` and `send_command`. The concepts in `postgres/concepts/statements.hpp` (`RunnableStatement`,
  `CommandStatement`, `SendableStatement`) say what each accepts.
- `Database` (`postgres/database.hpp`) owns the `AsyncClient` (a private base holding it, so the pool exists before the
  `StatementRunner` base) and adds `begin`, which leases a connection and runs `BEGIN` on it. `Transaction`
  (`postgres/transaction.hpp`) is a `StatementRunner` on a lease: `commit` and `rollback` go through `end`, the
  destructor through `abandon`.
- `AsyncClient` (`postgres/async_client.hpp`, `src/postgres/async_client.cpp`, pimpl) is the libpq client: a pool of
  non-blocking connections, one reactor thread, a `ThreadPool` of workers. `run` sends a statement for one result,
  `stream` for chunked rows, `lease` takes a connection, `post` runs a task on the workers. Its callbacks
  (`Completion`, `ChunkHandler`, `StreamCompletion`, `LeaseCompletion`) are `util::MoveOnlyFunction`s, so they can
  capture promises and other completions.

## The path of one call

`database.execute(q::Prepared<Q>::bind(args...))`:

1. **Compile time.** `Prepared<Q>` turns a bare predicate into a `Query`, checks it with `static_assert`s, types the
   slots and writes `SQL`.
2. **`bind`**, on the caller's thread: a `static_assert` per argument, `make_param<slot>` for each, a `TypedQuery`.
3. **`StatementRunner::execute`** calls `util::complete`, which makes the promise and future (this is where
   `std::bad_alloc` can reach the caller), then `detail::wire_caught`: the SQL is copied, each `Param` is formatted by
   `FieldCodec::format` through `TextBinder`, the column names come from the schema, any exception is caught into
   `Wired::error`.
4. **`send_for_rows`** makes the vector the rows collect into and calls `send_stream`: a query for rows always runs as a
   stream. `send_stream` posts the wiring error to a worker if there is one; otherwise it wraps the row reader in a
   `ChunkHandler` that first checks the result's column names, and calls `AsyncClient::stream`.
5. **`AsyncClient::Impl::stream`** checks the options, builds an `Operation` and `enqueue`s it under `_mutex`
   (refused if the client is stopping or the lease is lost), then wakes the reactor by writing to a pipe.
6. **The reactor** (`reactor_loop`): `start_queued` pairs the operation with an idle connection, `begin` calls
   `PQsendQueryParams` (binary results if asked) and `PQsetChunkedRowsMode`; then `poll`, `advance` (`PQflush`,
   `PQconsumeInput`) and `read_results`. Each `PGRES_TUPLES_CHUNK` goes to `enqueue_chunk`, onto `Stream::ready`, and
   `deliver` is posted to the workers if no delivery is running. The null result ends the statement: `finish_stream`
   returns the connection to the pool.
7. **A worker** runs `deliver`: chunks are taken one at a time and handed to `on_chunk`, which reads each row
   (`read_rows` → `read_row` → `FieldCodec`) into the vector; an exception is caught into `handler_error` and stops the
   stream; after each chunk the reactor is woken, since there is room for another.
8. **`complete_if_done`** posts the stream's completion once everything delivered is handled: `send_for_rows`'
   completion calls the `Settler`, which sets the promise's value or exception, and the caller's `get()` returns.

A statement that returns no rows (`execute(command)`) goes `send_command` → `AsyncClient::run` in text form → the
reactor's `finish` posts the completion with the result → `affected_rows()`.

**`stream_chunks`** is the same path with `read_chunk` as the reader. Backpressure: `read_results` stops reading a
connection whose stream has `max_buffered_chunks` chunks waiting (`is_full`, `paused`), so the server waits;
`serve_stream` resumes it when a chunk has been handled, and reads at once since libpq may already hold results. A
handler that returns `false` or throws sets `stop`; the reactor then cancels the statement (`PQcancelCreate`, driven
non-blocking by the reactor; the connection stays busy until the cancel is done) or, on a lease, reads the rest and
drops it. The cancel's own `57014` error is not reported.

**A transaction:** `Database::begin` shares the completion on the caller's thread, then `lease`s a connection; the
reactor's `grant` posts the lease to a worker, which sends `BEGIN` on it (`start_leased` runs a lease's statements one
at a time); the `BEGIN`'s completion builds the `Transaction` around the lease, or, if `BEGIN` failed, an ended one and
the lease goes back. `commit`/`rollback` → `end`: the `25P02` error is made on the caller's thread, the lease is taken
out of the transaction and `COMMIT` or `ROLLBACK` queued on it (if queuing throws, the lease is put back and the call
throws); when the lease's queue is empty the connection returns to the pool. A transaction dropped open calls
`abandon`, which queues a `ROLLBACK` with a no-op completion; if even that cannot be queued, `return_to_pool` sees the
connection is not idle and resets it (`PQresetStart`, then the session settings again).

## Threads

- **Callers**: any number; `Database` is thread-safe. Its constructor connects blocking (`PQconnectdb`, the session
  `SET`s), then puts every connection in non-blocking mode.
- **One reactor** (`Impl::_reactor`): all socket I/O, cancels and reconnects. Nothing of the user's runs there except
  `on_notice`, which must be quick and thread-safe.
- **Workers** (`ThreadPool`, `Options::workers` of them): every completion, row and chunk handler, lease grant and
  posted task. A task that throws is dropped.

Rules that follow: completions and handlers must not `get()` another future of the same database (every worker could
be waiting); a completion that starts a call catches what it throws; a `Transaction` is used from one thread at a time.

Locks and queues: `Impl::_mutex` guards the operation queue `_queued`, the idle connections, `_stopping` and each
`LeaseState`'s queue; `LeaseState::busy`, `_leases` and the active statements are the reactor's alone; `Stream::mutex`
guards `ready`, `delivering`, `stop`, `finished`, `error`, `completed`; `ThreadPool::_mutex` and its condition variable
guard `_tasks`. The reactor sleeps in `poll` and is woken through a non-blocking pipe. Shutdown (`~Impl`) sets
`_stopping`, wakes and joins the reactor (queued operations are rejected, streams aborted), runs every task still posted
(`ThreadPool::shutdown`), then closes the connections.

## The error guarantee

Stated in `statement_runner.hpp` and `database.hpp`:

1. Every error from making and running a statement (the server's, a lost connection, a value a codec refuses, a result
   that does not parse, no memory while wiring) reaches the completion or future exactly once, on a worker, never
   during the call.
2. A call throws only if it cannot start: `std::bad_alloc` handing the statement on or making the future, or whatever
   moving the caller's completion throws. Then the completion never runs. A call either started or threw.
3. Library code on a worker never loses an error.

Where each is kept:

- `wire_caught` is `noexcept` and catches wiring into `Wired::error`, which the senders post to a worker.
- A statement made ahead of time (`insert_statement`, `unchecked_sql`) keeps a codec's refusal in itself; `wire`
  rethrows it, `wire_caught` catches it.
- `deliver` catches what a handler or a row parse throws into `handler_error`; `complete_if_done` delivers it once.
- `Database::begin` shares its completion on the caller's thread, so the worker that fails to queue `BEGIN` can still
  call it; `Transaction::end` makes the `25P02` error on the caller's thread, so the worker allocates nothing.
- `util::MoveOnlyFunction` keeps a callable of up to three pointers inline, so posting `[stream, error]` from a worker
  or the reactor allocates nothing; a task that captures a completion is larger and is made where a `std::bad_alloc`
  can be reported.
- `tests/fault/allocation_test.cpp` replaces the global `operator new`, fails each allocation in turn on the caller
  (`sweep_caller`, `sweep_transaction`) and on the one worker (`sweep_worker`), and checks every completion ran once, or
  never when the call threw.

What arrives: `QueryError` with a SQLSTATE for the server's errors (and `25P02` for a commit that rolled back);
`std::runtime_error` for a lost connection, shutdown, a lost lease, wrong result columns, a NULL in a required field;
`std::invalid_argument` for a value that does not parse (naming the column) or bad stream options; `std::logic_error`
for a statement on an ended transaction.

## Compile-time machinery

- `util::CTString<n>` (`util/compile_time.hpp`) is the class-type non-type template parameter behind every name:
  `Field<T, "name">`, `Schema<"table", ...>`, `get<"name">`. The `#error` at its top refuses standards before C++20
  with a message that says so, because clang's own error for `Field<T, "name">` in C++17 does not.
- `util::is_identifier`, `is_sql_function_name` (`schema.name` allowed) and `is_sql_operator` (`util/identifier.hpp`)
  are the validators. They run at the declaration (`Field`, `Schema`, `BoundOperation`, `BoundRanking`) with a
  `static_assert` that says what is wrong, and again inside `sql::Text` as a guard.
- Errors in the builders are thrown (`std::logic_error`, `std::out_of_range`); in a constant expression that is a
  compile error whose output includes the message. `sql.hpp`'s non-constexpr marker functions do the same for the
  writer's invariants.
- "No second error" guards: where a `static_assert` fails, the code that would follow is given a harmless value
  (`KNOWN_COLUMNS`/`CHECKED` in `UpsertSql`, the null `ArgumentForm` after a bad `argument_type`), so one mistake gives
  one message. Keep that when adding checks.
- Capacities: `sql::STATEMENT_CAPACITY` 4096 (`TABLE_CHECK_CAPACITY` four times that), `MAX_CAST_TYPE_LENGTH` 128,
  `MAX_RAW_ARGUMENTS` 128, `MAX_IDENTIFIER_LENGTH` 63, `MAX_NUMERIC_SCALE` 18, `MoveOnlyFunction::INSIDE_SIZE` three
  pointers. Predicate, order and set capacities are template parameters.
- Compile-fail tests: `schemacht_compile_fail(CASE "regex")` in `tests/CMakeLists.txt` compiles
  `tests/compile_fail/cases.cpp` with `-DCASE_<CASE> -fsyntax-only` and passes if the compiler's output matches the
  regex; `compile_fail.baseline` (C++20) and `compile_fail.baseline_cxx23` check the file compiles with no case defined.

## How to

**Add a column type of your own** (outside the library): specialize `schemacht::ColumnType<T>`, with a helper from
`column_type.hpp` where one fits; `examples/custom_types.cpp` shows each. **Add a built-in type:** a `FieldCodec<T>`
specialization in `postgres/field_codec.hpp` (text `parse`/`format`, binary `parse_binary`/`binary_accepts` with the
OID from `binary_form.hpp`), its name in `query/sql_type.hpp`'s `builtin_type_name`, `Param`'s variant if it is a new
alternative, tests in `tests/unit/field_codec_test.cpp` and `column_type_test.cpp`, and a round trip in
`tests/integration/database_test.cpp` (the `Types` table).

**Add a test on a column** (a predicate kind): a `NodeKind` and the `ColumnRef` call in `query/predicate.hpp`, its
branch in `Emitter::node` in `query/sql.hpp`, a slot rule in `prepared.hpp` if it binds an argument, a SQL pin in
`tests/compile/sql_test.cpp`, and a compile-fail case for what it must refuse. **Add a member to operation types**
(as `COLUMN_PREFIX` was added): a concept in `query/concepts/predicate.hpp`, a field of `Operation` filled by
`BoundOperation`, the writer in `Emitter::call`, the concepts that must refuse it (`RankingOn`, `MergeOperation`), pins
and compile-fail cases.

**Add a statement kind:** its writer in `query/sql.hpp` (consteval, returning a `sql::Text`), a `SchemaStatement` maker
in `postgres/statements.hpp` (`SchemaStatementMaker::make`), a convenience on `StatementRunner` if it is schema-shaped,
pins in `tests/unit/statements_test.cpp`, an integration test, and a sweep in the fault test if it has a new path.

**Add an integration test:** use `test_db()` and `exec()` from `tests/support/integration.hpp`; name tables
`schemacht_it_*`, create them with `create_table` or `exec`, drop them at the end, and add anything new that the test
creates (a schema, a function, a type) to README's list, since users point the suite at their own server.

**Add a compile-fail case:** an `#if defined(CASE_NAME)` block in `tests/compile_fail/cases.cpp` and a
`schemacht_compile_fail(NAME "part of the message")` line; the regex must match the library's message, not the
compiler's.

## Building

```sh
cmake -S . -B build
cmake --build build -j
```

Options: `SCHEMACHT_INSTALL`, `SCHEMACHT_BUILD_TESTS`, `SCHEMACHT_BUILD_EXAMPLES` and `SCHEMACHT_WARNINGS` default to on
when schemacht is the top-level project; `SCHEMACHT_BUILD_DOCS` is `AUTO` (the `docs` target exists if Doxygen is found),
`ON` or `OFF`; `SCHEMACHT_CLANG_TIDY` (off) runs clang-tidy on each file as it compiles. With `SCHEMACHT_WARNINGS` every
target of schemacht's own compiles with the warning set in `CMakeLists.txt` (`-Wall -Wextra -Wpedantic -Wshadow
-Wconversion ...`, plus each compiler's own) as errors, through the target property `COMPILE_WARNING_AS_ERROR`, so
`cmake --build build --compile-no-warning-as-error` turns the errors back into warnings; the flags are private to those
targets and never reach a project that links schemacht. The `docs` target fails on any Doxygen warning. The
library is C++20 (`cxx_std_20` on the target); the headers compile as C++23 too, which `compile_fail.baseline_cxx23`
checks. libpq 17 or newer is required at configure time (`SCHEMACHT_MIN_LIBPQ`) and checked again when an `AsyncClient`
starts. Catch2 is fetched with FetchContent unless a 3.x is installed. `cmake --install build --prefix <dir>` puts the
static library under `lib/`, the headers under `include/schemacht/` and the package under `lib/cmake/schemacht/`.

Keeping a second build directory for the other compiler (`-DCMAKE_CXX_COMPILER=clang++`) catches what one compiler
accepts and the other does not; both g++ 13 and clang++ 18 are used.

## Testing

```sh
SCHEMACHT_TEST_DB="host=localhost port=5432 dbname=schemacht_test user=test" ctest --test-dir build -j8
```

- `schemacht_tests` holds the compile-time checks, the unit tests and the integration tests; the last skip themselves
  without `SCHEMACHT_TEST_DB`. `schemacht_fault_tests` is separate because it replaces `operator new`. The compile-fail
  cases and the examples are further ctests.
- Point `SCHEMACHT_TEST_DB` at a throwaway server: the tests and examples create and drop tables, schemas, a view, a
  sequence, functions and a type (README lists them), and one test installs `pg_trgm` if it can.
- The integration tests run in parallel (`-j8`): each uses tables of its own, so a new one must too.
- The examples are run by `tests/run_example.cmake`, which fails on a non-zero exit or anything on stderr (an error, or
  a notice that should have been dropped).
- Threads: the client's tests pass under ThreadSanitizer (`-fsanitize=thread`); on a kernel with a large address
  space randomisation range, run the binary with `setarch "$(uname -m)" -R`.
- The fault test on its own: `ctest --test-dir build -R "failed allocation"`.

## Linting and style

- `clang-format` with the repository's `.clang-format` (Google-based, 150 columns, `AlignAfterOpenBracket: BlockIndent`:
  captures and arguments wrap among themselves and the closing bracket gets its own line, aligned declarations).
- `clang-tidy` with the repository's `.clang-tidy` (nearly every check on, warnings as errors; `tests/` and `examples/`
  have their own relaxed configs; a check switched off has its reason beside it). A `NOLINT` carries the reason after
  `--`. Headers are checked through the sources that include them, either as the build runs (`-DSCHEMACHT_CLANG_TIDY=ON`)
  or by hand:

  ```sh
  clang-tidy -p build --header-filter='include/schemacht/.*' tests/unit/util_test.cpp
  ```

  A clang-tidy that reports `'stddef.h' file not found` has no builtin headers of its own; point it at a compiler's:
  `--extra-arg=-resource-dir=$(clang++ -print-resource-dir)`. Its findings before that are not to be trusted.

Conventions in force:

- Template parameters are constrained by named concepts, kept in a `concepts/` directory beside the header they belong
  to (`query/concepts/predicate.hpp` for `query/predicate.hpp`), which includes it with an IWYU export pragma.
- Helpers an extension author needs are public, not in `detail`.
- Class order: public constructors and methods, then `protected`, then `private` data and methods, then the defaulted
  or deleted special members last; all five are declared.
- A blank line between the steps of a function, and before each `} else` of a chain with more than two branches.
- Doc comments: `@brief` on types, `@return` alone on functions where the return says it all, `///<` on members, names
  in backticks; a file-level comment starts with Doxygen's file command, so that it documents the file and not the
  namespace that follows it.
- Decisions settled only for now are recorded with what is still needed, not left as silent limits.
