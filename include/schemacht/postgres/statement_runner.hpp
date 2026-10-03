#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "schemacht/postgres/async_client.hpp"
#include "schemacht/postgres/concepts/statement_runner.hpp"  // IWYU pragma: export
#include "schemacht/postgres/key_lookup.hpp"
#include "schemacht/postgres/row_reader.hpp"
#include "schemacht/postgres/statements.hpp"
#include "schemacht/schema/schema.hpp"
#include "schemacht/util/compile_time.hpp"
#include "schemacht/util/futures.hpp"

namespace schemacht::postgres {

namespace detail {

/** @return A future that is already complete with `value`. */
template <typename value_t>
[[nodiscard]] std::future<value_t> ready_future(value_t value) {
  std::promise<value_t> promise;
  promise.set_value(std::move(value));
  return promise.get_future();
}

/**
 * @brief Waits for `count` statements that were queued at once, whose completions may run on several workers at the same
 * time and in any order: calls `done` once, after the last of them, with the error of the earliest statement that failed
 * (null if none did). After a failure in a transaction, PostgreSQL fails every later statement too (`25P02`), which is not
 * the error worth reporting.
 */
template <StreamCallback done_t>
class Countdown {
 public:
  Countdown(std::size_t count, done_t done) : _remaining(count), _done(std::move(done)) {}

  /** @brief Statement `index` finished, with `error` (null on success). */
  void finish(std::size_t index, const std::exception_ptr& error) {
    std::unique_lock lock(_mutex);
    if ( error && index < _failed_at ) {
      _failed_at = index;
      _error = error;
    }

    if ( --_remaining != 0 ) {
      return;
    }

    lock.unlock();
    _done(_error);
  }

 private:
  std::mutex         _mutex;
  std::size_t        _remaining;
  std::size_t        _failed_at = SIZE_MAX;
  std::exception_ptr _error;
  done_t             _done;
};

/** @brief The results of statements whose rows a caller reads (`RETURNING`): each statement's, in statement order. */
struct KeptResults {
  ResultFormat             format = ResultFormat::Text;  ///< The form the statements are asked to return their rows in.
  std::vector<QueryResult> results;
};

/** @brief Where `send_all` keeps the results; null when nobody reads them. */
using Results = std::shared_ptr<KeptResults>;

/**
 * @brief Queues every one of `statements` at once, on `lease`'s connection or (if it is null) on the pool. On a lease, nothing
 * queued later can come between them. Calls `done` once, after the last completion, with the earliest error.
 *
 * With `results`, each statement's result is kept there, at its index and in its `format`, for `done` to read: each
 * completion only stores its own, so nothing is read before every statement has finished.
 * @pre `statements` isn't empty; on the pool, it is one statement (several go through `run_atomically`).
 */
template <StreamCallback done_t>
void send_all(
    AsyncClient& client, const AsyncClient::Lease* lease, std::vector<InsertStatement> statements, done_t done, const Results& results = {}
) {
  const auto         countdown = std::make_shared<Countdown<done_t>>(statements.size(), std::move(done));
  const ResultFormat format = results ? results->format : ResultFormat::Text;
  if ( results ) {
    results->results.resize(statements.size());
  }

  for ( std::size_t index = 0; index < statements.size(); ++index ) {
    AsyncClient::Completion completion = [countdown, index, results](QueryResult result, const std::exception_ptr& error) {
      if ( results ) {
        results->results.at(index) = std::move(result);  // its own slot: the countdown's lock orders it before `done`
      }

      countdown->finish(index, error);
    };

    InsertStatement& statement = statements.at(index);
    if ( lease != nullptr ) {
      client.run(*lease, std::move(statement.sql), std::move(statement.values), std::move(completion), format);
    } else {
      client.run(std::move(statement.sql), std::move(statement.values), std::move(completion), format);
    }
  }
}

/**
 * @brief Runs `statements` (inserts) on a connection of their own, in one transaction: `BEGIN`, every one of them and
 * `COMMIT`, queued at once. Calls `done` once with the earliest error: all of the rows are written, or none is (after a
 * failed insert, PostgreSQL answers the `COMMIT` with `ROLLBACK`, and the insert's error is the earlier one). With `results`,
 * as `send_all`: the `BEGIN` and `COMMIT` keep theirs too, which have no rows.
 */
template <StreamCallback done_t>
void run_atomically(AsyncClient& client, std::vector<InsertStatement> statements, done_t done, const Results& results = {}) {
  client.lease([client = &client, statements = std::move(statements), done = std::move(done),
                results](const AsyncClient::Lease& lease, const std::exception_ptr& error) mutable {
    if ( error ) {
      done(error);
      return;
    }

    statements.insert(statements.begin(), InsertStatement{.sql = "BEGIN", .values = {}});
    statements.push_back(InsertStatement{.sql = "COMMIT", .values = {}});

    send_all(*client, &lease, std::move(statements), std::move(done), results);
  });  // the lease goes back once the COMMIT is done
}

/**
 * @return Every row of `results`, read as `row_t`s, in order: in binary form or as text, as each result is.
 * @throws as `read_rows` does, for a value that does not parse.
 */
template <TextRow row_t>
[[nodiscard]] std::vector<row_t> read_all(const std::vector<QueryResult>& results) {
  std::vector<row_t> rows;
  auto               keep = [&rows](row_t&& row) { rows.push_back(std::move(row)); };
  for ( const QueryResult& result : results ) {
    read_rows<row_t>(result, keep);
  }

  return rows;
}

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
 * Every call returns a `std::future` at once (or takes a completion, `..._into`) and the work finishes in the background.
 *
 * @note Errors, including a statement that can't be started, arrive through the future, never as a throw from the call itself.
 *
 * @warning Row handlers and completions run on the pool's worker threads. They must not block on another of this database's
 * futures (`get()`), which could leave no worker free to complete it; chain with `execute_into` / `stream_into` instead.
 */
class StatementRunner {
 public:
  /**
   * @brief Runs `statement` in the background and calls `done` exactly once, on a worker thread, with every row (as `execute`
   * returns them) and the error (null on success).
   *
   * The primitive `execute` is built on: for chaining a statement's result into another asynchronous step (another `execute_into`
   * or `stream_into`, say) without a future in between that nothing ever waits on. Most callers want `execute`.
   * @tparam statement_t A query (`Prepared::bind`) of any schema, or a raw statement (`RawStatement::bind`).
   * @param read How to read the result; the default if not given.
   */
  template <RunnableStatement statement_t, detail::ExecuteCallback<typename statement_t::result_type> done_t>
  void execute_into(const statement_t& statement, done_t done, const std::optional<ReadOptions>& read = std::nullopt) {
    using result_t = statement_t::result_type;
    const auto rows = std::make_shared<std::vector<result_t>>();

    stream_into(
        statement, [rows](result_t&& row) { rows->push_back(std::move(row)); },
        [rows, done = std::move(done)](const std::exception_ptr& error) mutable { done(error ? std::vector<result_t>{} : std::move(*rows), error); },
        read
    );
  }

  /**
   * @return A future for every row satisfying `statement`, each a `statement_t::result_type` (a tuple of the returned columns' `Field`s;
   * the table's `row_type` unless the query projects). It holds the error instead if the statement failed (`QueryError`), or its result
   * was not the columns it names (`std::runtime_error`).
   * @tparam statement_t A query (`Prepared::bind`) of any schema, or a raw statement (`RawStatement::bind`).
   * @param read How to read the result; the default if not given.
   */
  template <RunnableStatement statement_t>
  [[nodiscard]] std::future<std::vector<typename statement_t::result_type>> execute(
      const statement_t& statement, const std::optional<ReadOptions>& read = std::nullopt
  ) {
    using result_t = statement_t::result_type;
    const auto                         promise = std::make_shared<std::promise<std::vector<result_t>>>();
    std::future<std::vector<result_t>> future = promise->get_future();

    execute_into(
        statement, [promise](std::vector<result_t> rows, const std::exception_ptr& error) { util::settle(*promise, error, std::move(rows)); }, read
    );

    return future;
  }

  /**
   * @brief Runs `statement` in the background, calls `on_row` with each row (a `statement_t::result_type`, as `execute` returns
   * them) in order, and calls `done` exactly once, on a worker thread, when the stream ends, with the error (null on success).
   *
   * `on_row` is called one row at a time, never from two threads at once, on a thread of the pool's (not the caller's), and
   * returns `void`, or a `bool` where `false` stops: no more rows are delivered and the query is cancelled (in a transaction,
   * where a cancel would abort it, the rest of the rows are read and dropped instead). If `on_row` throws, that is the stream's
   * error; rows delivered before an error (the database's or `on_row`'s) stay delivered.
   *
   * The primitive `stream` is built on: for chaining into another asynchronous step without a future in between. Most
   * callers want `stream`.
   */
  template <RunnableStatement statement_t, RowCallback<typename statement_t::result_type> handler_t, detail::StreamCallback done_t>
  void stream_into(const statement_t& statement, handler_t on_row, done_t done, const std::optional<ReadOptions>& read = std::nullopt) {
    using result_t = statement_t::result_type;
    ReadOptions options = read.value_or(_read);
    options.format = BinaryReadableRow<result_t> ? options.format : ResultFormat::Text;

    detail::WireStatement wired;
    try {
      wired = detail::wire(statement);  // formats the arguments, which a codec may refuse
    } catch ( ... ) {
      _client->post([done = std::move(done), error = std::current_exception()] mutable { done(error); });
      return;
    }

    send_stream(
        std::string(wired.sql), std::move(wired.params), options,
        [column_names = std::move(wired.column_names), on_row = std::move(on_row)](QueryResult chunk) mutable {
          detail::check_result_columns(column_names, chunk);
          return read_rows<result_t>(chunk, on_row);  // false: `on_row` wants no more rows, stop the query
        },
        std::move(done)
    );
  }

  /**
   * @brief As `stream_into`, with the completion as the returned future instead of a callback: the future completes normally once
   * the stream ends (whether it ran to the end or `on_row` stopped it), or holds the query's error.
   */
  template <RunnableStatement statement_t, RowCallback<typename statement_t::result_type> handler_t>
  [[nodiscard]] std::future<void> stream(const statement_t& statement, handler_t on_row, const std::optional<ReadOptions>& read = std::nullopt) {
    const auto        promise = std::make_shared<std::promise<void>>();
    std::future<void> future = promise->get_future();

    stream_into(statement, std::move(on_row), [promise](const std::exception_ptr& error) { util::settle(*promise, error); }, read);

    return future;
  }

  /**
   * @brief Runs `command` (a statement that returns no rows: an update, a delete, or a raw statement with no columns such as a
   * `CREATE TABLE`) in the background and calls `done` exactly once, on a worker thread, with the count of rows it affected and
   * the error (null on success). The primitive the command `execute` is built on.
   */
  template <CommandStatement statement_t, detail::CommandCallback done_t>
  void execute_into(const statement_t& command, done_t done) {
    detail::WireStatement wired;
    try {
      wired = detail::wire(command);  // formats the arguments, which a codec may refuse
    } catch ( ... ) {
      _client->post([done = std::move(done), error = std::current_exception()] mutable { done(0, error); });
      return;
    }

    command_into(std::string(wired.sql), std::move(wired.params), std::move(done));
  }

  /**
   * @return A future for the count of rows `command` affected (those an `UPDATE` changed, say; 0 for a `CREATE TABLE`), or holding
   * its error (`QueryError`).
   */
  template <CommandStatement statement_t>
  [[nodiscard]] std::future<std::uint64_t> execute(const statement_t& command) {
    const auto                 promise = std::make_shared<std::promise<std::uint64_t>>();
    std::future<std::uint64_t> future = promise->get_future();

    execute_into(command, [promise](std::uint64_t affected, const std::exception_ptr& error) { util::settle(*promise, error, affected); });

    return future;
  }

  /**
   * @return A future that completes once `schema_t`'s table is created, or holds the error (`QueryError`, e.g. if it exists):
   * `CREATE TABLE` with each column's SQL type, `NOT NULL` unless its field is a `std::optional`, the primary key, and a
   * `UNIQUE` constraint per unique column. A generated key (the one primary key column, a signed integer) is an identity column
   * the database numbers. For tests, examples and a first start; a table with more to it (indexes, other constraints, any other
   * generated column) is created with SQL.
   */
  template <schema::IsSchema schema_t>
  [[nodiscard]] std::future<void> create_table() {
    return run_without_rows(detail::CreateTableSql<schema_t>::VALUE);
  }

  /** @return A future that completes once `schema_t`'s table is dropped, if it existed (`DROP TABLE IF EXISTS`), or holds the error. */
  template <schema::IsSchema schema_t>
  [[nodiscard]] std::future<void> drop_table() {
    return run_without_rows(detail::DropTableSql<schema_t>::VALUE);
  }

  /** @return A future for every row of `schema_t` whose `key_col` value is one of `keys`, e.g. `db.select_by_key_values<PlacesSchema, "id">(ids)`. */
  template <TextSchema schema_t, util::CTString key_col>
  [[nodiscard]] std::future<std::vector<typename schema_t::row_type>> select_by_key_values(std::span<const key_type<schema_t, key_col>> keys) {
    if ( keys.empty() ) {
      return detail::ready_future(std::vector<typename schema_t::row_type>{});
    }

    return execute(by_key_values<schema_t, key_col>(keys));
  }

  /** @return A future for every row of `schema_t` whose primary key is one of `keys`, e.g. `db.select_by_key<Orders>(ids)`. */
  template <TextSchema schema_t>
    requires schema::HasSingleColumnKey<schema_t>
  [[nodiscard]] std::future<std::vector<typename schema_t::row_type>> select_by_key(std::span<const primary_key_type<schema_t>> keys) {
    return select_by_key_values<schema_t, schema::primary_key_field<schema_t>::COLUMN_NAME>(keys);
  }

  /**
   * @return A future that completes once `rows` of `schema_t` are written, or holds the error. Generated columns (see
   * `Field::GENERATED`) are never sent: the database fills them in. The rows are written atomically: all of them, or none.
   *
   * Normally that is one statement. A schema with an array-typed column takes several for many rows (see `detail::build_insert`),
   * and they run in a transaction: in the caller's, inside a `Transaction`, and otherwise in one of their own, on a connection
   * of their own while they run.
   */
  template <TextSchema schema_t>
  [[nodiscard]] std::future<void> insert(const std::vector<typename schema_t::row_type>& rows) {
    return insert_rows<schema_t, typename schema_t::row_type>(rows);
  }

  /** @brief As above, for a span. */
  template <TextSchema schema_t>
  [[nodiscard]] std::future<void> insert(std::span<const typename schema_t::row_type> rows) {
    return insert_rows<schema_t, typename schema_t::row_type>(rows);
  }

  /** @brief As above, for rows without the generated columns (`Schema::make_insert_row`). */
  template <TextSchema schema_t>
    requires schema::HasGeneratedColumn<schema_t>
  [[nodiscard]] std::future<void> insert(const std::vector<typename schema_t::insert_row_type>& rows) {
    return insert_rows<schema_t, typename schema_t::insert_row_type>(rows);
  }

  /**
   * @return A future for the rows as written, every column of each (`INSERT ... RETURNING`), generated ones included: the key
   * the database chose, say. Otherwise as `insert`. It holds the error instead if the insert failed.
   *
   * The rows come back in the order of `rows` only for one row: PostgreSQL returns one row per row inserted, but does not
   * promise their order, so match several by a column they were given (a unique one), not by position.
   * @param read The form to read them in (`ReadOptions::format`; the rest is for streams); the default if not given.
   */
  template <TextSchema schema_t>
  [[nodiscard]] std::future<std::vector<typename schema_t::row_type>> insert_returning(
      const std::vector<typename schema_t::row_type>& rows, const std::optional<ReadOptions>& read = std::nullopt
  ) {
    return insert_rows_returning<schema_t, typename schema_t::row_type>(rows, read);
  }

  /** @brief As above, for rows without the generated columns (`Schema::make_insert_row`). */
  template <TextSchema schema_t>
    requires schema::HasGeneratedColumn<schema_t>
  [[nodiscard]] std::future<std::vector<typename schema_t::row_type>> insert_returning(
      const std::vector<typename schema_t::insert_row_type>& rows, const std::optional<ReadOptions>& read = std::nullopt
  ) {
    return insert_rows_returning<schema_t, typename schema_t::insert_row_type>(rows, read);
  }

 protected:
  /**
   * @param on_lease Whether statements go to `lease`'s connection (a transaction's) rather than the pool; once `lease` is gone,
   *                 every statement then fails.
   */
  StatementRunner(AsyncClient& client, AsyncClient::Lease lease, bool on_lease, const ReadOptions& read)
      : _client(&client), _lease(std::move(lease)), _on_lease(on_lease), _read(read) {}

  [[nodiscard]] AsyncClient&       client() const noexcept { return *_client; }
  [[nodiscard]] bool               holds_lease() const noexcept { return static_cast<bool>(_lease); }
  [[nodiscard]] AsyncClient::Lease take_lease() noexcept { return std::move(_lease); }
  [[nodiscard]] const ReadOptions& read_options() const noexcept { return _read; }

 private:
  AsyncClient*       _client;
  AsyncClient::Lease _lease;     // a transaction's connection; empty for a database, and once the transaction has ended
  bool               _on_lease;  // a transaction's: statements go to `_lease`, and fail without it
  ReadOptions        _read;

  /** @brief Sends a statement to the pool, or to the transaction's connection (where, once it has ended, `done` gets `std::logic_error`). */
  void send(std::string sql, std::vector<std::string> params, AsyncClient::Completion done) {
    if ( _on_lease ) {
      _client->run(_lease, std::move(sql), std::move(params), std::move(done));
    } else {
      _client->run(std::move(sql), std::move(params), std::move(done));
    }
  }

  /** @brief As `send`, for a stream. */
  void send_stream(
      std::string sql, std::vector<std::string> params, const ReadOptions& options, AsyncClient::ChunkHandler on_chunk,
      AsyncClient::StreamCompletion done
  ) {
    if ( _on_lease ) {
      _client->stream(_lease, std::move(sql), std::move(params), options, std::move(on_chunk), std::move(done));
    } else {
      _client->stream(std::move(sql), std::move(params), options, std::move(on_chunk), std::move(done));
    }
  }

  /**
   * @brief Runs the statements of an insert, all at once, so that nothing comes between them: in the transaction, on one; else
   * one statement on the pool, or several in a transaction of their own. `results`, if given, gets each statement's result.
   */
  template <detail::StreamCallback done_t>
  void run_inserts(std::vector<detail::InsertStatement> statements, done_t done, const detail::Results& results = {}) {
    if ( statements.size() > 1 && ! _on_lease ) {
      detail::run_atomically(*_client, std::move(statements), std::move(done), results);
    } else {
      detail::send_all(*_client, _on_lease ? &_lease : nullptr, std::move(statements), std::move(done), results);
    }
  }

  /** @brief What every `insert` runs. */
  template <TextSchema schema_t, schema::InsertRowOf<schema_t> row_t>
  [[nodiscard]] std::future<void> insert_rows(std::span<const row_t> rows) {
    const auto        promise = std::make_shared<std::promise<void>>();
    std::future<void> future = promise->get_future();

    try {
      if ( rows.empty() ) {
        promise->set_value();
        return future;
      }

      run_inserts(detail::build_insert<schema_t, row_t>(rows), [promise](const std::exception_ptr& error) { util::settle(*promise, error); });
    } catch ( ... ) {
      promise->set_exception(std::current_exception());
    }

    return future;
  }

  /** @brief What every `insert_returning` runs: the inserts, each ending in `RETURNING`, read once the last has finished. */
  template <TextSchema schema_t, schema::InsertRowOf<schema_t> row_t>
  [[nodiscard]] std::future<std::vector<typename schema_t::row_type>> insert_rows_returning(
      std::span<const row_t> rows, const std::optional<ReadOptions>& read
  ) {
    using written_t = schema_t::row_type;
    const auto                          promise = std::make_shared<std::promise<std::vector<written_t>>>();
    std::future<std::vector<written_t>> future = promise->get_future();

    try {
      if ( rows.empty() ) {
        promise->set_value({});
        return future;
      }

      std::vector<detail::InsertStatement> statements = detail::build_insert<schema_t, row_t>(rows);
      for ( detail::InsertStatement& statement : statements ) {
        statement.sql += detail::ReturningSql<schema_t>::VALUE;
      }

      const auto results = std::make_shared<detail::KeptResults>();
      results->format = BinaryReadableRow<written_t> ? read.value_or(_read).format : ResultFormat::Text;

      run_inserts(
          std::move(statements),
          [promise, results](const std::exception_ptr& error) {
            if ( error ) {
              promise->set_exception(error);
              return;
            }

            try {
              promise->set_value(detail::read_all<written_t>(results->results));
            } catch ( ... ) {
              promise->set_exception(std::current_exception());
            }
          },
          results
      );
    } catch ( ... ) {
      promise->set_exception(std::current_exception());
    }

    return future;
  }

  /** @brief Runs `sql`, which returns no rows, and calls `done` with the count of rows it affected and the error (null on success). */
  template <detail::CommandCallback done_t>
  void command_into(std::string sql, std::vector<std::string> params, done_t done) {
    send(std::move(sql), std::move(params), [done = std::move(done)](const QueryResult& result, const std::exception_ptr& error) mutable {
      done(error ? 0 : result.affected_rows(), error);
    });
  }

  /** @return A future that completes once `sql` (a statement without arguments or rows) has run, or holds its error. */
  [[nodiscard]] std::future<void> run_without_rows(std::string_view sql) {
    const auto        promise = std::make_shared<std::promise<void>>();
    std::future<void> future = promise->get_future();

    command_into(std::string(sql), {}, [promise](std::uint64_t /*affected*/, const std::exception_ptr& error) { util::settle(*promise, error); });

    return future;
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
