# Using schemacht

The headers under `include/schemacht/` document every type and call; the programs under `examples/` are complete, and
the longer snippets below name the file they come from.

The namespaces are `schemacht::schema` (tables), `schemacht::query` (statements) and `schemacht::postgres` (running
them). The examples alias them as `sch`, `q` and `pg`, and so does this guide.

## Describe a table

A column is a `sch::Field`: its C++ type, its name, and optionally its key role and whether the database fills it in.
A table is a `sch::Schema` of fields, with its name:

```cpp
using Id = sch::Field<std::int64_t, "id", sch::KeyRole::Primary, true>;  // true: the database numbers it
using Name = sch::Field<std::string, "name">;
using Email = sch::Field<std::optional<std::string>, "email">;  // a std::optional column can be NULL
using Score = sch::Field<double, "score">;
using People = sch::Schema<"schemacht_example_people", Id, Name, Email, Score>;
```

- A `std::optional<T>` column can be NULL. Everything that names the column's type elsewhere (comparisons, arguments,
  the SQL type) uses the `T` inside.
- `KeyRole::Primary` marks a column as the primary key, or part of it: several `Primary` fields form one composite key.
  `KeyRole::Unique` marks a column as unique on its own; several `Unique` fields are each unique separately. A unique
  constraint across several columns is not described; create it with SQL.
- The fourth argument, `generated`, marks a column the database fills in: an identity key, a column with a `DEFAULT`
  the program never writes, or a computed one. It is read like any other and never inserted or updated.
- Names are checked while compiling: lowercase letters, digits and underscores, not starting with a digit, at most 63
  bytes; two fields of one schema cannot share a name.

A row is `People::row_type`, a `std::tuple` of the fields, read by name:

```cpp
People::make_row(1, "ann", "ann@example.com", 91.5);        // every column
People::make_insert_row("ann", "ann@example.com", 91.5);    // without the generated ones: an insert_row_type
sch::get<"name">(person);                                   // a reference to the column's value
```

`make_row` and `make_insert_row` take the values in the schema's order, each converting to its column's type without
narrowing (an `int` for a `std::int64_t` column, a literal for a `std::string`, `std::nullopt` for a NULL).

## Write statements

Statements are values that exist at compile time, written with the schema's columns. `q::On<Schema>::col<"name">()`
names a column; `q::arg<N>()` stands for the N-th value given when the statement runs:

```cpp
using Col = q::On<People>;

constexpr auto AT_LEAST = q::select(Col::col<"score">() >= q::arg<0>()).order_by(Col::col<"score">().desc()).limit(10);
constexpr auto SET_SCORE = q::update<People>().set(Col::col<"score">(), q::arg<0>()).where(Col::col<"id">() == q::arg<1>());
```

Tests on a column:

- `==`, `!=`, `<`, `<=`, `>`, `>=` against an `arg<N>()`, or against a literal for `bool` and integer columns (other
  types are compared with arguments only; a literal that does not fit the column is a compile error);
- `col.in(arg<N>())`: the value is one of a `std::vector` of them;
- `col.is_null()` and `col.is_not_null()`, for a `std::optional` column;
- `q::like(col, arg<N>())` for a text column, written as the `~~` operator;
- `&&`, `||` and `!` combine tests; `q::match_all<Schema>()` and `q::match_none<Schema>()` are the two constants;
- `col.apply<Op>(arg<N>())` for an operator or function of an extension (see [An extension's operators](#an-extensions-operators-and-functions)).

A query is `q::select(test)`, or `q::select<Schema>()` for every row, then:

- `.order_by(col.asc(), col.desc(), ...)`, or `col.ranked_by<Op>(arg<N>())` for a ranking such as "the nearest first";
- `.limit(10)` or `.limit(arg<N>())`;
- `.project(col, ...)` to fetch only some columns; each result row is then a tuple of just those fields.

An update is `q::update<Schema>()` with `.set(col, arg<N>())` or `.set(col, literal)`, `.set_null(col)`, and `.where(test)`
or `.all_rows()`; one without either does not compile. A delete is `q::delete_where(test)` or `q::delete_all<Schema>()`;
`delete_where(match_all())` does not compile. Both give the count of rows changed, or the rows themselves with
`.returning()` (every column) or `.returning(col, ...)`.

`q::Prepared<STATEMENT>` is the statement lowered to SQL. `q::Prepared<AT_LEAST>::SQL` is a compile-time string:

```
SELECT "id", "name", "email", "score" FROM "schemacht_example_people" WHERE "score" >= $1 ORDER BY "score" DESC LIMIT 10
```

`q::Prepared<AT_LEAST>::bind(80.0)` supplies the arguments, each of its column's type (or `std::int64_t` for a limit,
a `std::vector` of the column's type for `in`), converting without narrowing. The wrong number of arguments, a wrong
type, or one `arg<N>()` used for two columns of different types are compile errors, with messages that say so. Values
reach the database as `$n` parameters, never spliced into the text.

## Run them

A `pg::Database` is a pool of connections plus the threads that complete the calls:

```cpp
pg::Database database("host=localhost dbname=app user=app");
```

`pg::Database::Options` sets the pool: `pool.connections` (4 by default; also the most statements in flight at once),
`pool.workers` (2, the threads that run completions), `pool.session` (settings applied to every connection: JIT off and
ISO dates by default), `pool.on_notice` (a callback for the server's notices, which are otherwise dropped), and `read`,
how results are read unless a call says otherwise (see [Stream large results](#stream-large-results)).

Every call starts the work and returns at once. By default it returns a `std::future` of the outcome:

```cpp
for ( const People::row_type& person : database.execute(q::Prepared<AT_LEAST>::bind(80.0)).get() ) {
  std::cout << sch::get<"name">(person) << ": " << sch::get<"score">(person) << '\n';
}
```

Given a completion as its last argument instead, the call returns nothing and the completion runs once, on a worker
thread, with the outcome and the error (null on success). This is how one step starts the next without blocking a
thread; from `examples/transfers.cpp`:

```cpp
std::uint64_t record_closing_balances(pg::Database& database) {
  std::promise<std::uint64_t> recorded;
  std::future<std::uint64_t>  done = recorded.get_future();

  database.execute(
      q::Prepared<ALL_ACCOUNTS>::bind(), [&database, &recorded](const std::vector<Accounts::row_type>& accounts, const std::exception_ptr& error) {
        if ( error ) {
          recorded.set_exception(error);
          return;
        }

        try {
          std::vector<ClosingBalances::row_type> snapshot;
          snapshot.reserve(accounts.size());
          for ( const Accounts::row_type& account : accounts ) {
            snapshot.push_back(ClosingBalances::make_row(sch::get<"id">(account), sch::get<"balance">(account)));
          }

          // From a worker, so no `.get()` here: the insert takes a completion too.
          database.insert<ClosingBalances>(snapshot, [&recorded](std::uint64_t inserted, const std::exception_ptr& insert_error) {
            schemacht::util::settle(recorded, insert_error, inserted);
          });
        } catch ( ... ) {  // no memory to start the insert: a worker drops what a completion throws, and the wait below would never end
          recorded.set_exception(std::current_exception());
        }
      }
  );

  return done.get();  // the one wait, on the caller's thread
}
```

Two rules follow from where completions run:

- A completion or row handler must not wait on another of the database's futures with `get()`: with every worker
  waiting, none is free to complete what they wait for. Chain the next call with a completion instead.
- What a completion throws is dropped by the worker, so a completion that starts another call catches what that call
  throws (the `try` above).

Errors: every error from making and running a statement arrives through the future or the completion, exactly once,
and never during the call. The server's errors are a `pg::QueryError`, whose `sqlstate()` is the five-character code
(`23505` for a unique violation, say). A lost connection, a result whose columns are not the ones the statement names,
or a NULL in a column the schema says cannot be one is a `std::runtime_error`; a value that does not parse is a
`std::invalid_argument`, naming the column. A call itself throws only if it cannot start at all: `std::bad_alloc` with
no memory to hand the statement on, and then its completion never runs.

The calls, on a `Database` or a `Transaction` alike:

| Call | Gives |
|---|---|
| `execute(query)` | a `std::vector` of the result rows |
| `execute(command)` | the count of rows an update, delete, insert or DDL statement affected |
| `stream(query, on_row)`, `stream_chunks(query, on_chunk)` | each row, or each chunk of rows, as it arrives |
| `insert<S>(rows)`, `insert_returning<S>(rows)` | the count written, or the rows as the database stored them |
| `upsert<S, Rules...>(rows)` | the count written or updated |
| `select_by_key<S>(keys)`, `select_by_key_values<S, "col">(values)` | the rows whose key, or column, is one of the values |
| `create_table<S>()`, `drop_table<S>()` | the table from the schema, and `DROP TABLE IF EXISTS` |
| `table_differences<S>()` | how the table differs from the schema (see [Check a table](#check-a-table-against-its-schema)) |

Each takes the completion last; `execute(query)`, `stream` and `stream_chunks` take a `pg::ReadOptions` before it.

## Insert rows

`insert` writes any number of rows in one statement: all of them or none. A schema with generated columns takes rows
without them (`make_insert_row`), and `insert_returning` gives back the rows as stored, generated values included:

```cpp
const auto added = database
                       .insert_returning<People>({
                           People::make_insert_row("ann", "ann@example.com", 91.5),
                           People::make_insert_row("bob", std::nullopt, 78.0),
                       })
                       .get();
```

The rows may come back in any order; find each by a value you gave.

`upsert` inserts rows and, for a primary key already in the table, updates that row instead (`ON CONFLICT ... DO
UPDATE`). By default every column but the key is overwritten; a `pg::MergeWith<"col", Op>` merges the value already
there with the new one by a SQL operator or function, and a `pg::Keep<"col">` leaves it as it is. From
`examples/transfers.cpp`:

```cpp
struct Add {
  static constexpr std::string_view OPERATOR = "+";
};

transaction
    .upsert<Activity, pg::MergeWith<"transfers", Add>, pg::MergeWith<"moved", Add>, pg::Keep<"first_transfer">>(
        {Activity::make_row(from, 1, amount, transfer_id), Activity::make_row(to, 1, amount, transfer_id)}
    )
    .get();
```

A function is called as `FUNCTION("current"."col", excluded."col")`, the value already there first; it may be named
with its schema (`stats.add_counts`). An upsert needs a primary key the rows carry (not a generated one), at least one
column to update, and rules that name no key, generated or repeated column; each of these is a compile error otherwise.
The conflict target is the primary key only.

## Use transactions

```cpp
pg::Transaction transaction = database.begin({.isolation = pg::Isolation::Serializable}).get();
...
transaction.commit().get();
```

`begin` takes a `pg::TransactionOptions`: `isolation` (`ReadCommitted`, the default; `RepeatableRead`; `Serializable`)
and `read_only`. The transaction holds one of the pool's connections until it ends; it runs the same calls as the
database, in the order they are made. A transaction dropped without `commit` or `rollback` is rolled back.

- A statement the server rejects fails the transaction: PostgreSQL refuses every later statement in it (SQLSTATE
  `25P02`), and `commit` reports that it rolled back (a `QueryError` with `25P02`).
- An error before a statement is sent (a value its codec refuses) is that call's alone; the transaction stays open.
- Under `Serializable`, a commit that fails with `40001` is to be run again.
- Once ended, every statement on it fails with `std::logic_error`; `is_open()` says.
- A transaction is used from one thread at a time, and must not outlive its database.

## Stream large results

`stream` calls a handler with each row as it arrives, so nothing holds the whole result; `stream_chunks` hands over
each chunk of rows as a `std::vector`. The handler returns `void`, or a `bool` where `false` stops the stream. From
`tests/integration/database_test.cpp`:

```cpp
database.stream_chunks(Series::bind(100000000), [&seen](const std::vector<Series::row_type>& rows) { return (seen += rows.size()) < 250; }).get();
```

The rows are read from the database in chunks of `ReadOptions::chunk_rows` (1000 by default), and at most
`max_buffered_chunks` (4) are held before the client stops reading and the server waits, so a slow handler holds about
`chunk_rows × (max_buffered_chunks + 1)` rows in memory. `ReadOptions::format` chooses PostgreSQL's binary form (the
default; less to send and parse) or text. The defaults are the database's `Options::read`; `read_options()` gives them,
to change one for a call:

```cpp
auto read = database.read_options();
read.chunk_rows = 40;
```

The handler runs on a worker thread, one call at a time. If it throws, that is the stream's error, and the rows it had
stay delivered. A stream stopped early is cancelled on the server; in a transaction, where a cancel would fail the
transaction, the remaining rows are read and dropped instead. The future, or the completion, completes once the stream
has ended either way.

## Run SQL the builder cannot write

A `q::RawStatement` is SQL text with typed arguments and typed result columns; from `examples/basic.cpp`:

```cpp
using Headcount = schemacht::schema::Field<std::int64_t, "headcount">;
using Average = schemacht::schema::Field<std::optional<double>, "average">;  // NULL when no row matches
using PayrollSummary = schemacht::query::RawStatement<
    "SELECT count(*) AS headcount, avg(salary) AS average FROM schemacht_example_employees WHERE salary >= $1", schemacht::query::RawArguments<float>,
    Headcount, Average>;

const PayrollSummary::row_type summary = database.execute(PayrollSummary::bind(50000.0F)).get().at(0);
```

Its `$n` placeholders are checked against the argument list while compiling; the result's columns are checked by name
when it runs (a column returned under another name is an error, not the wrong value). Where PostgreSQL cannot tell a
parameter's type from its context, cast it in the text (`CAST($1 AS jsonb)`). A raw statement with no fields is a command
(`execute` gives the count). An argument cannot be NULL.

For text that can only be written while the program runs, `pg::unchecked_sql(text, arguments...)` runs it with the
arguments bound to `$1`, `$2`, ..., checking nothing:

```cpp
database.execute(pg::unchecked_sql("CREATE INDEX ON " + table + " (body)")).get();
database.execute(pg::unchecked_sql("UPDATE " + table + " SET hits = $1 WHERE id = ANY($2)", std::int64_t{7}, std::vector<std::int64_t>{1, 3})).get();
```

It is one statement that returns no rows. A value goes as an argument, never into the text; a name spliced into the
text is quoted first (`sch::TableName::quoted()`).

## Column types

The built-in column types and the SQL types they are written as:

| C++ | PostgreSQL |
|---|---|
| `bool` | `boolean` |
| `std::int8_t`/`std::int16_t`, `std::int32_t`, `std::int64_t` | `smallint`, `integer`, `bigint` |
| `std::uint8_t`, `std::uint16_t`, `std::uint32_t` | the next wider signed type: `smallint`, `integer`, `bigint` |
| `float`, `double` | `real`, `double precision` |
| `std::string` | `text` |
| `std::vector<std::byte>` | `bytea` |
| `std::vector<T>` of any of these | `T[]` |
| `std::chrono::sys_time<D>` (`system_clock::time_point`) | `timestamptz` |
| `std::chrono::local_time<D>` | `timestamp` |
| `std::chrono::year_month_day` | `date` |
| `schemacht::Uuid` | `uuid` |
| `schemacht::json::Json` | `jsonb` |

A 64-bit unsigned column does not compile (`bigint` is signed). Times are kept to the microsecond; a time written is
truncated to one. A `std::vector` of `std::vector` compiles as an array of arrays, but reading or writing one fails at
run time.

A type of your own becomes a column type by specializing `schemacht::ColumnType<T>` once, next to the type, with the
SQL type's name and the text form; a binary form is optional. `include/schemacht/column_type.hpp` has helpers that write
the specialization, from `examples/custom_types.cpp`:

```cpp
template <>
struct schemacht::ColumnType<shop::OrderId> : schemacht::WrappedColumn<&shop::OrderId::value> {};

template <>
struct schemacht::ColumnType<shop::Mood> : schemacht::EnumColumn<shop::Mood, "schemacht_example_mood", "sad", "ok", "happy"> {};

template <>
struct schemacht::ColumnType<shop::Priority> : schemacht::EnumIntegerColumn<shop::Priority> {};

template <>
struct schemacht::ColumnType<std::filesystem::path>
    : schemacht::ConvertedColumn<
          std::filesystem::path, std::string, [](const std::filesystem::path& path) { return path.string(); },
          [](const std::string& text) { return std::filesystem::path(text); }> {};
```

- `WrappedColumn<&T::member>`: a type over one built-in value (a strong id over a `bigint`).
- `EnumColumn<E, "sql_type", "label"...>`: a C++ enum as a PostgreSQL enum type, or as `text`, by its labels.
- `EnumIntegerColumn<E>`: a C++ enum as its number.
- `ConvertedColumn<T, Through, to, from>`: a type stored as another type through two functions.
- `ScaledNumericColumn<&T::member, places, "numeric(p,s)">`: an integer count of hundredths (say) as a `numeric`.

The same file shows one written by hand (`Money`): `SQL_TYPE`, `parse`, `format`, and optionally `parse_binary` and
`binary_accepts`. A type without a binary form is read as text whatever the query asks for. Give a type `operator<=>`
if queries compare or order by it. A type with array literals that PostgreSQL does not split on commas (PostGIS's
`geometry`) works as a column and as a list argument, but not yet as an array column.

## An extension's operators and functions

An operation is a type that names its SQL; `col.apply<Op>(arg<N>())` tests the column with it, and
`col.ranked_by<Op>(arg<N>())` sorts by it. From `examples/extensions.cpp`:

```cpp
/** @brief `"column" % $1::text`: similar enough, by the `pg_trgm.similarity_threshold` setting. */
struct Similar {
  static constexpr std::string_view OPERATOR = "%";
};

/** @brief `"column" <-> $1::text`: 1 minus the similarity, so the most similar sort first. */
struct Distance {
  static constexpr std::string_view OPERATOR = "<->";
};

/** @brief `similar_above("column", $1::text, $2::double precision)`: more similar than the number (`CreateSimilarAbove`). */
struct SimilarAbove {
  static constexpr std::string_view FUNCTION = "similar_above";
  static constexpr bool             TAKES_NUMBER = true;
};

/** @brief `"column" ? $1::text`: the document has the key. The column is `jsonb`; the argument is not. */
struct HasKey {
  static constexpr std::string_view OPERATOR = "?";
  using argument_type = JsonKey;
};

/** @brief `lower("column") = lower($1::text)`: the same text, whatever the case of either. */
struct SameIgnoringCase {
  static constexpr std::string_view OPERATOR = "=";
  static constexpr std::string_view COLUMN_PREFIX = "lower(";
  static constexpr std::string_view COLUMN_SUFFIX = ")";
  static constexpr std::string_view ARGUMENT_PREFIX = "lower(";
  static constexpr std::string_view ARGUMENT_SUFFIX = ")";
};
```

```cpp
constexpr auto NAMED_LIKE = q::select(StreetColumns::col<"name">().apply<trgm::Similar>(q::arg<0>()))
                                .order_by(StreetColumns::col<"name">().ranked_by<trgm::Distance>(q::arg<0>()))
                                .limit(3)
                                .project(StreetColumns::col<"name">());
```

The members:

- `OPERATOR` (written `"column" OP $n`) or `FUNCTION` (`FUNCTION("column", $n)`), one of the two. A function may be
  named with its schema. The spelling is checked while compiling.
- `TAKES_NUMBER = true`: the function takes a `double` as well, `apply<Op>(arg<0>(), arg<1>())`.
- `argument_type`: the argument's type when it is not the column's (a key for `jsonb`'s `?`, a polygon against a line
  column). It must be a column type of your own; `bind` then takes exactly that type, and the argument is cast to its
  SQL type.
- `ARGUMENT_PREFIX`/`ARGUMENT_SUFFIX` and `COLUMN_PREFIX`/`COLUMN_SUFFIX`: SQL written around the argument or the column.
  They test an expression of the column, such as `lower("name")`, `ST_ConvexHull("rast")` or a cast to `geography`,
  which is what an index on that expression answers. The text is written as it is, so an expression written with an
  operator brings its own parentheses. Only the test changes: select lists and `ORDER BY` name the column itself.

A ranking has `OPERATOR` and optionally `argument_type`, and nothing around its sides. A merge operation for `upsert`
has `OPERATOR` or `FUNCTION` and nothing else. `json::contains` (`@>`) and `q::like` (`~~`) are operations the library
ships.

## Tables named while the program runs

A schema's name is its default table. A `sch::Table<Schema>` names another table of the same shape, checked once when
it is made, and a statement of the schema runs there once aimed at it with `.on(table)`. From `examples/basic.cpp`:

```cpp
const std::string                              archive_name = std::string(EmployeeSchema::TABLE_NAME) + "_archive";
const schemacht::schema::Table<EmployeeSchema> archive(archive_name);
const std::vector                              archived{EmployeeSchema::make_row(3, "johnny cash", 48000.0F)};

database.execute(schemacht::postgres::drop_table_statement<EmployeeSchema>().on(archive)).get();
database.execute(schemacht::postgres::create_table_statement<EmployeeSchema>().on(archive)).get();
database.execute(schemacht::postgres::insert_statement<EmployeeSchema>(archived).on(archive)).get();

std::cout << "In " << archive_name << ":\n";
for ( const EmployeeSchema::row_type& employee : database.execute(NameLookup::bind("john%").on(archive)).get() ) {
  std::cout << "  " << schemacht::schema::get<"name">(employee) << '\n';
}
```

`Table(schema_name, name)` names a table in a PostgreSQL schema (`"gis"."roads"`). Aiming another schema's statement at
a table is a compile error, and a raw statement names its own tables. The schema-shaped calls (`insert<S>`,
`create_table<S>`, ...) have statement forms for this: `insert_statement<S>(rows)`, `upsert_statement<S, Rules...>(rows)`,
`create_table_statement<S>()`, `drop_table_statement<S>()`, `table_differences_statement<S>()`.

## Check a table against its schema

`table_differences<S>()` asks the database how the table differs from the schema and gives each difference as a row:
what is wrong (`pg::TableProblem`), the column, what the schema expects and what the table has. `pg::describe` puts one
in words. From `tests/integration/table_differences_test.cpp`:

```cpp
for ( const pg::TableDifference& difference : runner.execute(pg::table_differences_statement<schema_t>().on(table)).get() ) {
```

It reports a missing table or column, a type the database does not have (an extension not installed), another type
(as the database spells it, modifiers included: `geometry(Point,4326)`), `NOT NULL` where the field is optional or the
reverse, a generated field with nothing to fill it in, a column the database always computes that an insert would set,
another primary key, and a required column the schema lacks. Views are compared on columns and types only; `UNIQUE`
constraints and indexes are not compared. It needs a PostgreSQL 17 server.

## Limits

- A statement's text fits 4096 bytes; an identifier is at most 63 bytes, lowercase.
- No joins, aggregates, `DISTINCT` or `OFFSET` in the builder: write a `RawStatement`.
- Literals in tests are `bool` and integers only; everything else is an argument.
- A column of arrays of arrays compiles but fails at run time; so does an array column of a type whose array literal
  is not comma-separated.
- One `Unique` field is one constraint; a unique constraint across columns is written in SQL, and `upsert` matches on
  the primary key only.
- `argument_type` must be a column type of your own, not a built-in one.
- A `RawStatement` or `unchecked_sql` argument cannot be NULL; `unchecked_sql` returns no rows.
- The table check needs PostgreSQL 17; the library needs libpq 17.
