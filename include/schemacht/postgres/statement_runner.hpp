#pragma once

#include <cstdint>
#include <exception>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "schemacht/postgres/async_client.hpp"
#include "schemacht/postgres/key_lookup.hpp"
#include "schemacht/postgres/row_reader.hpp"
#include "schemacht/postgres/statements.hpp"
#include "schemacht/schema/schema.hpp"
#include "schemacht/util/compile_time.hpp"
#include "schemacht/util/futures.hpp"

namespace schemacht::postgres {

namespace detail {

/**
 * @brief A statement as it goes to the database, or why it could not be made (a codec refused a value): that goes to the
 * completion. (`std::expected`, which is what this is, is not there for clang with libstdc++ 13.)
 */
struct Wired {
  WireStatement      statement;
  std::exception_ptr error;
};

/** @return `statement` as it goes to the database (its arguments moved, if it is a temporary); or, if it could not be, why. */
template <SendableStatementRef statement_t>
[[nodiscard]] Wired wire_caught(statement_t&& statement) noexcept {
  try {
    return {.statement = wire(std::forward<statement_t>(statement)), .error = nullptr};
  } catch ( ... ) {
    Wired failed;
    failed.error = std::current_exception();
    return failed;
  }
}

/** @brief The rows `execute` gives for `statement_t` (given by reference or as a temporary). */
template <SendableStatementRef statement_t>
using rows_of = std::vector<typename std::remove_cvref_t<statement_t>::result_type>;

}  // namespace detail

/**
 * @brief How a result is read: in chunks of how many rows, how many chunks may wait for the handler, and in which form. A row
 * with a column that has no binary form (see `HasBinaryForm`) is always read as text.
 */
using ReadOptions = AsyncClient::StreamOptions;

/**
 * @brief What a `Database` and a `Transaction` both run: queries, raw statements, commands, inserts and table changes, on the
 * pool (a `Database`) or on the one connection a transaction holds (a `Transaction`).
 *
 * Every call starts the work and returns at once; it finishes in the background. Each takes, last, how to be told: by default
 * a `std::future` of the outcome, which the call returns (`db.execute(query).get()`), or else a completion, called once with
 * the outcome and the error (null on success) on a worker thread, and the call returns nothing. A completion chains the next
 * step without a future in between: `db.execute(query, [&](std::vector<Row> rows, std::exception_ptr error) { ... })`.
 *
 * The schema-shaped calls (`create_table`, `insert`, `select_by_key`, ...) use the schema's own table. To run a statement on
 * another of its shape, named while the program runs (a `schema::Table`), aim it there: `db.execute(query.on(archive))`,
 * `db.execute(insert_statement<Orders>(rows).on(archive))`.
 *
 * @note Errors: every error from making and running a statement (the server's, a lost connection, a value a codec refuses, a
 * result that doesn't parse, even no memory while turning it into what is sent) arrives through the future, or the completion,
 * exactly once and on a worker thread, never during the call. A call throws only if it can't start at all, and then its
 * completion is never called: `std::bad_alloc` with no memory to hand the statement on (or to make the future), or whatever
 * moving a completion or row handler of yours throws. A call either started, or threw.
 *
 * @warning Row handlers and completions run on the pool's worker threads. They must not block on another of this database's
 * futures (`get()`), which could leave no worker free to complete it: chain the next call with a completion instead, catching
 * what that call throws.
 */
class StatementRunner {
 public:
  /**
   * @brief Runs `statement` for every row it returns, each a `statement_t::result_type` (a tuple of the returned columns'
   * `Field`s; the table's `row_type` unless the query projects).
   *
   * The error is a `QueryError` if the statement failed, or a `std::runtime_error` if its result was not the columns it names.
   * @tparam statement_t A query (`Prepared::bind`) of any schema, an insert that gives back its rows
   *         (`insert_returning_statement`), or a raw statement (`RawStatement::bind`).
   * @param read How to read the result.
   * @param done none given (`util::UseFuture`): returns a `std::future` of the rows. Else called with the rows and the error.
   */
  template <RunnableStatementRef statement_t, util::CompletionFor<detail::rows_of<statement_t>> done_t = util::UseFuture>
  auto execute(statement_t&& statement, const ReadOptions& read, done_t done = {}) {
    using result_t = std::remove_cvref_t<statement_t>::result_type;
    return util::complete<std::vector<result_t>>(std::move(done), [&](auto done) {
      send_for_rows<result_t>(detail::wire_caught(std::forward<statement_t>(statement)), read, std::move(done));
    });
  }

  /** @brief As above, read as this runner reads results by default. */
  template <RunnableStatementRef statement_t, util::CompletionFor<detail::rows_of<statement_t>> done_t = util::UseFuture>
  auto execute(statement_t&& statement, done_t done = {}) {
    return execute(std::forward<statement_t>(statement), _read, std::move(done));
  }

  /**
   * @brief Runs `command`, a statement that returns no rows (an update, a delete, an insert, a `CREATE TABLE`, or a raw statement
   * with no columns), for the count of rows it affected: those an `UPDATE` changed, say, or 0 for a `CREATE TABLE`.
   * @param done none given (`util::UseFuture`): returns a `std::future` of the count. Else called with the count and the error.
   */
  template <CommandStatementRef statement_t, util::CompletionFor<std::uint64_t> done_t = util::UseFuture>
  auto execute(statement_t&& command, done_t done = {}) {
    return util::complete<std::uint64_t>(std::move(done), [&](auto done) {
      send_command(detail::wire_caught(std::forward<statement_t>(command)), std::move(done));
    });
  }

  /**
   * @brief Runs `statement` and calls `on_row` with each row it returns (a `statement_t::result_type`, as `execute` gives them),
   * in order, as they arrive: nothing holds the whole result.
   *
   * `on_row` is called one row at a time, never from two threads at once, on a thread of the pool's (not the caller's), and
   * returns `void`, or a `bool` where `false` stops: no more rows are delivered and the query is cancelled (in a transaction,
   * where a cancel would abort it, the rest of the rows are read and dropped instead). If `on_row` throws, that is the stream's
   * error; rows delivered before an error (the database's or `on_row`'s) stay delivered.
   * @param done none given (`util::UseFuture`): returns a `std::future<void>` that completes once the stream ends (whether it ran to
   *        the end or `on_row` stopped it). Else called with the error once it ends.
   */
  template <
      RunnableStatement statement_t, RowCallback<typename statement_t::result_type> handler_t, util::CompletionFor<void> done_t = util::UseFuture>
  auto stream(const statement_t& statement, handler_t on_row, const ReadOptions& read, done_t done = {}) {
    return util::complete<void>(std::move(done), [&](auto done) {
      send_rows<typename statement_t::result_type>(detail::wire_caught(statement), read, std::move(on_row), std::move(done));
    });
  }

  /** @brief As above, read as this runner reads results by default. */
  template <
      RunnableStatement statement_t, RowCallback<typename statement_t::result_type> handler_t, util::CompletionFor<void> done_t = util::UseFuture>
  auto stream(const statement_t& statement, handler_t on_row, done_t done = {}) {
    return stream(statement, std::move(on_row), _read, std::move(done));
  }

  // ---- conveniences: each one statement, run by `execute` on the schema's own table ------------------------------------------

  /** @brief Creates `schema_t`'s table (see `create_table_statement`). */
  template <schema::IsSchema schema_t, util::CompletionFor<std::uint64_t> done_t = util::UseFuture>
  auto create_table(done_t done = {}) {
    return execute(create_table_statement<schema_t>(), std::move(done));
  }

  /** @brief Drops `schema_t`'s table, if it exists (`DROP TABLE IF EXISTS`). */
  template <schema::IsSchema schema_t, util::CompletionFor<std::uint64_t> done_t = util::UseFuture>
  auto drop_table(done_t done = {}) {
    return execute(drop_table_statement<schema_t>(), std::move(done));
  }

  /** @brief Every row of `schema_t` whose `key_col` value is one of `keys`, e.g. `db.select_by_key_values<PlacesSchema, "id">(ids)`. */
  template <TextSchema schema_t, util::CTString key_col, util::CompletionFor<std::vector<typename schema_t::row_type>> done_t = util::UseFuture>
  auto select_by_key_values(std::span<const key_type<schema_t, key_col>> keys, done_t done = {}) {
    return execute(by_key_values<schema_t, key_col>(keys), std::move(done));
  }

  /** @brief Every row of `schema_t` whose primary key is one of `keys`, e.g. `db.select_by_key<Orders>(ids)`. */
  template <TextSchema schema_t, util::CompletionFor<std::vector<typename schema_t::row_type>> done_t = util::UseFuture>
    requires schema::HasSingleColumnKey<schema_t>
  auto select_by_key(std::span<const primary_key_type<schema_t>> keys, done_t done = {}) {
    return execute(by_key<schema_t>(keys), std::move(done));
  }

  /**
   * @brief Writes `rows` of `schema_t` (see `insert_statement`), for the count written: all of them, or none. The rows are copied
   * into the statement before the call returns, so they need only live for the call.
   */
  template <TextSchema schema_t, util::CompletionFor<std::uint64_t> done_t = util::UseFuture>
  auto insert(const std::vector<typename schema_t::row_type>& rows, done_t done = {}) {
    return execute(insert_statement<schema_t>(rows), std::move(done));
  }

  /** @brief As above, for rows without the generated columns (`Schema::make_insert_row`). */
  template <TextSchema schema_t, util::CompletionFor<std::uint64_t> done_t = util::UseFuture>
    requires schema::HasGeneratedColumn<schema_t>
  auto insert(const std::vector<typename schema_t::insert_row_type>& rows, done_t done = {}) {
    return execute(insert_statement<schema_t>(rows), std::move(done));
  }

  /**
   * @brief Writes `rows` of `schema_t` and gives back the rows as written (see `insert_returning_statement`), every column of
   * each, generated ones included: the key the database chose, say.
   */
  template <TextSchema schema_t, util::CompletionFor<std::vector<typename schema_t::row_type>> done_t = util::UseFuture>
  auto insert_returning(const std::vector<typename schema_t::row_type>& rows, done_t done = {}) {
    return execute(insert_returning_statement<schema_t>(rows), std::move(done));
  }

  /** @brief As above, for rows without the generated columns (`Schema::make_insert_row`). */
  template <TextSchema schema_t, util::CompletionFor<std::vector<typename schema_t::row_type>> done_t = util::UseFuture>
    requires schema::HasGeneratedColumn<schema_t>
  auto insert_returning(const std::vector<typename schema_t::insert_row_type>& rows, done_t done = {}) {
    return execute(insert_returning_statement<schema_t>(rows), std::move(done));
  }

 protected:
  /**
   * @param on_lease Whether statements go to `lease`'s connection (a transaction's) rather than the pool; once `lease` is gone,
   *                 every statement then fails.
   */
  StatementRunner(AsyncClient& client, AsyncClient::Lease lease, bool on_lease, const ReadOptions& read)
      : _client(&client), _lease(std::move(lease)), _on_lease(on_lease), _read(read) {}

  [[nodiscard]] AsyncClient&              client() const noexcept { return *_client; }
  [[nodiscard]] const AsyncClient::Lease& lease() const noexcept { return _lease; }
  [[nodiscard]] bool                      holds_lease() const noexcept { return static_cast<bool>(_lease); }
  [[nodiscard]] AsyncClient::Lease        take_lease() noexcept { return std::move(_lease); }
  [[nodiscard]] const ReadOptions&        read_options() const noexcept { return _read; }

 private:
  AsyncClient*       _client;
  AsyncClient::Lease _lease;     // a transaction's connection; empty for a database, and once the transaction has ended
  bool               _on_lease;  // a transaction's: statements go to `_lease`, and fail without it
  ReadOptions        _read;

  // ---- sending: the one place a statement goes to the client. One that could not be wired fails there, on a worker ----------

  /** @brief Sends `wired` and calls `done` with every row it returns, read as `result_t`s. */
  template <TextRow result_t, util::CompletionFor<std::vector<result_t>> done_t>
  void send_for_rows(detail::Wired wired, const ReadOptions& read, done_t done) {
    const auto rows = std::make_shared<std::vector<result_t>>();

    send_rows<result_t>(
        std::move(wired), read, [rows](result_t&& row) { rows->push_back(std::move(row)); },
        [rows, done = std::move(done)](const std::exception_ptr& error) mutable { done(error ? std::vector<result_t>{} : std::move(*rows), error); }
    );
  }

  /** @brief Sends `wired`, calls `on_row` with each row it returns, read as a `result_t`, and then `done`. */
  template <TextRow result_t, RowCallback<result_t> handler_t>
  void send_rows(detail::Wired wired, ReadOptions read, handler_t on_row, AsyncClient::StreamCompletion done) {
    if ( wired.error ) {
      _client->post([done = std::move(done), error = wired.error] mutable { done(error); });
      return;
    }

    read.format = BinaryReadableRow<result_t> ? read.format : ResultFormat::Text;
    AsyncClient::ChunkHandler on_chunk = [column_names = std::move(wired.statement.column_names),
                                          on_row = std::move(on_row)](QueryResult chunk) mutable {
      detail::check_result_columns(column_names, chunk);
      return read_rows<result_t>(chunk, on_row);  // false: `on_row` wants no more rows, stop the query
    };

    if ( _on_lease ) {
      _client->stream(_lease, std::move(wired.statement.sql), std::move(wired.statement.params), read, std::move(on_chunk), std::move(done));
    } else {
      _client->stream(std::move(wired.statement.sql), std::move(wired.statement.params), read, std::move(on_chunk), std::move(done));
    }
  }

  /** @brief Sends `wired`, which returns no rows, and calls `done` with the count of rows it affected and the error (null on success). */
  template <util::CompletionFor<std::uint64_t> done_t>
  void send_command(detail::Wired wired, done_t done) {
    AsyncClient::Completion completion = [done = std::move(done)](const QueryResult& result, const std::exception_ptr& error) mutable {
      done(error ? 0 : result.affected_rows(), error);
    };

    if ( wired.error ) {
      _client->post([completion = std::move(completion), error = wired.error] mutable { completion(QueryResult{}, error); });
    } else if ( _on_lease ) {
      _client->run(_lease, std::move(wired.statement.sql), std::move(wired.statement.params), std::move(completion));
    } else {
      _client->run(std::move(wired.statement.sql), std::move(wired.statement.params), std::move(completion));
    }
  }

 public:
  StatementRunner(const StatementRunner&) = delete;
  StatementRunner& operator=(const StatementRunner&) = delete;

 protected:
  StatementRunner(StatementRunner&&) noexcept = default;
  StatementRunner& operator=(StatementRunner&&) noexcept = default;
  ~StatementRunner() = default;
};

}  // namespace schemacht::postgres
