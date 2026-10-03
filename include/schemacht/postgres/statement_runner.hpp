#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "schemacht/postgres/async_client.hpp"
#include "schemacht/postgres/concepts/statement_runner.hpp"  // IWYU pragma: export
#include "schemacht/postgres/key_lookup.hpp"
#include "schemacht/postgres/row_reader.hpp"
#include "schemacht/postgres/statements.hpp"
#include "schemacht/schema/schema.hpp"
#include "schemacht/schema/table_name.hpp"
#include "schemacht/util/compile_time.hpp"
#include "schemacht/util/futures.hpp"

namespace schemacht::postgres {

namespace detail {

/** @return A future that is already complete with `value`. */
template <util::PromiseValue value_t>
[[nodiscard]] std::future<value_t> ready_future(value_t value) {
  std::promise<value_t> promise;
  promise.set_value(std::move(value));
  return promise.get_future();
}

/** @return A future that is already complete. */
[[nodiscard]] inline std::future<void> ready_future() {
  std::promise<void> promise;
  promise.set_value();
  return promise.get_future();
}

/** @brief `StatementRunner::guarded`'s report for a completion that gets the error alone (a `StreamCallback`). */
struct FailWithError {
  template <StreamCallback done_t>
  void operator()(done_t& done, const std::exception_ptr& error) const {
    done(error);
  }
};

/** @brief `StatementRunner::guarded`'s report for a completion that gets rows (an `ExecuteCallback`): none. */
template <TextRow result_t>
struct FailWithNoRows {
  template <ExecuteCallback<result_t> done_t>
  void operator()(done_t& done, const std::exception_ptr& error) const {
    done(std::vector<result_t>{}, error);
  }
};

/** @brief `StatementRunner::guarded`'s report for a completion that gets a count of rows (a `CommandCallback`): 0. */
struct FailWithNoCount {
  template <CommandCallback done_t>
  void operator()(done_t& done, const std::exception_ptr& error) const {
    done(std::uint64_t{0}, error);
  }
};

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
  requires std::copy_constructible<done_t>  // a copy is kept, to report a failure to queue them (a `util::OnceCallback`)
void run_atomically(AsyncClient& client, std::vector<InsertStatement> statements, done_t done, const Results& results = {}) {
  client.lease([client = &client, statements = std::move(statements), done = std::move(done),
                results](const AsyncClient::Lease& lease, const std::exception_ptr& error) mutable {
    if ( error ) {
      done(error);
      return;
    }

    try {
      statements.insert(statements.begin(), InsertStatement{.sql = "BEGIN", .values = {}});
      statements.push_back(InsertStatement{.sql = "COMMIT", .values = {}});

      send_all(*client, &lease, std::move(statements), done, results);
    } catch ( ... ) {  // no memory to queue them all: with no COMMIT queued, nothing is written once the lease goes back
      done(std::current_exception());
    }
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

template <TextSchema schema_t>
class Table;  // postgres/table.hpp

/**
 * @brief What a `Database` and a `Transaction` both run: queries, raw statements, commands, inserts and table changes, on the
 * pool (a `Database`) or on the one connection a transaction holds (a `Transaction`).
 *
 * Every call returns a `std::future` at once (or takes a completion, `..._into`) and the work finishes in the background.
 *
 * The schema-shaped calls (`create_table`, `insert`, `select_by_key`, ...) use the schema's own table; a `Table` runs them
 * on another, named while the program runs.
 *
 * @note No error escapes a call. One that takes a completion is `noexcept` and calls it exactly once, on a worker thread,
 * with any error: the server's, an argument a codec refuses, even no memory to queue the statement. Only when there is not
 * even the memory to hand the error to a worker (or moving the completion itself throws) does it run on the caller's thread,
 * before the call returns. One
 * that returns a future is the same call with the future's completion (`util::future_from`): every error arrives through the
 * future, and the call can throw only `std::bad_alloc`, when there is no memory for the future itself.
 *
 * @warning Row handlers and completions run on the pool's worker threads. They must not block on another of this database's
 * futures (`get()`), which could leave no worker free to complete it; chain with `execute_into` / `stream_into` / `insert_into`
 * instead.
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
  void execute_into(const statement_t& statement, done_t done, const std::optional<ReadOptions>& read = std::nullopt) noexcept {
    execute_on(statement, nullptr, std::move(done), read);
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
    return util::future_from<std::vector<typename statement_t::result_type>>([&](auto done) { execute_into(statement, std::move(done), read); });
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
  void stream_into(const statement_t& statement, handler_t on_row, done_t done, const std::optional<ReadOptions>& read = std::nullopt) noexcept {
    stream_on(statement, nullptr, std::move(on_row), std::move(done), read);
  }

  /**
   * @brief As `stream_into`, with the completion as the returned future instead of a callback: the future completes normally once
   * the stream ends (whether it ran to the end or `on_row` stopped it), or holds the query's error.
   */
  template <RunnableStatement statement_t, RowCallback<typename statement_t::result_type> handler_t>
  [[nodiscard]] std::future<void> stream(const statement_t& statement, handler_t on_row, const std::optional<ReadOptions>& read = std::nullopt) {
    return util::future_from<void>([&](auto done) { stream_on(statement, nullptr, std::move(on_row), std::move(done), read); });
  }

  /**
   * @brief Runs `command` (a statement that returns no rows: an update, a delete, or a raw statement with no columns such as a
   * `CREATE TABLE`) in the background and calls `done` exactly once, on a worker thread, with the count of rows it affected and
   * the error (null on success). The primitive the command `execute` is built on.
   */
  template <CommandStatement statement_t, detail::CommandCallback done_t>
  void execute_into(const statement_t& command, done_t done) noexcept {
    command_on(command, nullptr, std::move(done));
  }

  /**
   * @return A future for the count of rows `command` affected (those an `UPDATE` changed, say; 0 for a `CREATE TABLE`), or holding
   * its error (`QueryError`).
   */
  template <CommandStatement statement_t>
  [[nodiscard]] std::future<std::uint64_t> execute(const statement_t& command) {
    return util::future_from<std::uint64_t>([&](auto done) { execute_into(command, std::move(done)); });
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
    return run_without_rows<detail::CreateTableSql<schema_t>>(nullptr);
  }

  /** @return A future that completes once `schema_t`'s table is dropped, if it existed (`DROP TABLE IF EXISTS`), or holds the error. */
  template <schema::IsSchema schema_t>
  [[nodiscard]] std::future<void> drop_table() {
    return run_without_rows<detail::DropTableSql<schema_t>>(nullptr);
  }

  /** @return A future for every row of `schema_t` whose `key_col` value is one of `keys`, e.g. `db.select_by_key_values<PlacesSchema, "id">(ids)`. */
  template <TextSchema schema_t, util::CTString key_col>
  [[nodiscard]] std::future<std::vector<typename schema_t::row_type>> select_by_key_values(std::span<const key_type<schema_t, key_col>> keys) {
    return select_by_key_values_on<schema_t, key_col>(keys, nullptr);
  }

  /** @return A future for every row of `schema_t` whose primary key is one of `keys`, e.g. `db.select_by_key<Orders>(ids)`. */
  template <TextSchema schema_t>
    requires schema::HasSingleColumnKey<schema_t>
  [[nodiscard]] std::future<std::vector<typename schema_t::row_type>> select_by_key(std::span<const primary_key_type<schema_t>> keys) {
    return select_by_key_values<schema_t, schema::primary_key_field<schema_t>::COLUMN_NAME>(keys);
  }

  /**
   * @brief Writes `rows` of `schema_t` in the background and calls `done` exactly once, on a worker thread, with the error
   * (null on success). Generated columns (see `Field::GENERATED`) are never sent: the database fills them in. The rows are
   * written atomically: all of them, or none.
   *
   * Normally that is one statement. A schema with an array-typed column takes several for many rows (see `detail::build_insert`),
   * and they run in a transaction: in the caller's, inside a `Transaction`, and otherwise in one of their own, on a connection
   * of their own while they run.
   *
   * The primitive `insert` is built on: for chaining an insert onto another asynchronous step (from an `execute_into`'s
   * completion, say) without a future in between. The rows are copied into the statements before it returns, so they need
   * only live for the call.
   */
  template <TextSchema schema_t, detail::StreamCallback done_t>
  void insert_into(const std::vector<typename schema_t::row_type>& rows, done_t done) noexcept {
    insert_rows_into<schema_t, typename schema_t::row_type>(rows, nullptr, std::move(done));
  }

  /** @brief As above, for a span. */
  template <TextSchema schema_t, detail::StreamCallback done_t>
  void insert_into(std::span<const typename schema_t::row_type> rows, done_t done) noexcept {
    insert_rows_into<schema_t, typename schema_t::row_type>(rows, nullptr, std::move(done));
  }

  /** @brief As above, for rows without the generated columns (`Schema::make_insert_row`). */
  template <TextSchema schema_t, detail::StreamCallback done_t>
    requires schema::HasGeneratedColumn<schema_t>
  void insert_into(const std::vector<typename schema_t::insert_row_type>& rows, done_t done) noexcept {
    insert_rows_into<schema_t, typename schema_t::insert_row_type>(rows, nullptr, std::move(done));
  }

  /**
   * @return As `insert_into`, with the completion as the returned future: it completes once the rows are written, or holds the
   * error. For no rows it is complete already.
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

  /**
   * @brief Starts a call that ends by calling `done`, so that `done` runs exactly once whatever throws, and nothing escapes:
   * `start` gets it as a `util::OnceCallback` to hand on, and if `start` throws (a codec refusing an argument, no memory to
   * queue a statement), `fail` gives it the error on a worker. Whatever `start` had handed it to before it threw finds it
   * called already.
   *
   * `done` is taken by reference, and moved only inside: should that move throw, or there be no memory to share `done` or to
   * post the failure, `fail` gives it the error here, before this returns (`done` as the failed move left it).
   */
  template <util::MovableValue done_t, detail::FailureReport<done_t> fail_t, std::invocable<const util::OnceCallback<done_t>&> start_t>
    requires detail::FailureReport<fail_t, util::OnceCallback<done_t>>
  void guarded(done_t&& done, const fail_t& fail, start_t start) noexcept {
    std::optional<util::OnceCallback<done_t>> once;
    try {
      once.emplace(std::forward<done_t>(done));
    } catch ( ... ) {  // no memory, or the move threw: `done` is as it was, or as that move left it
      report_now(fail, done, std::current_exception());
      return;
    }

    try {
      start(*once);
    } catch ( ... ) {
      std::exception_ptr error = std::current_exception();
      try {
        _client->post([fail, done = *once, error] mutable { fail(done, error); });
      } catch ( ... ) {
        report_now(fail, *once, error);
      }
    }
  }

 private:
  template <TextSchema schema_t>
  friend class Table;

  AsyncClient*       _client;
  AsyncClient::Lease _lease;     // a transaction's connection; empty for a database, and once the transaction has ended
  bool               _on_lease;  // a transaction's: statements go to `_lease`, and fail without it
  ReadOptions        _read;

  /** @brief Has `fail` give `done` `error` on this thread, dropping what `done` throws, as a worker would: it has nowhere to go. */
  template <typename fail_t, typename done_t>
    requires detail::FailureReport<fail_t, done_t>
  static void report_now(const fail_t& fail, done_t& done, const std::exception_ptr& error) noexcept {
    try {
      fail(done, error);
    } catch ( ... ) {  // NOLINT(bugprone-empty-catch) -- see above
    }
  }

  // ---- what the calls run, on `table` (a `Table`'s), or on the schema's own if it is null. They take the completion (and a
  // stream's row handler) by reference, so that nothing is moved before `guarded` can catch what a move throws. ----------

  /** @brief What every query's `execute_into` runs. */
  template <RunnableStatement statement_t, detail::ExecuteCallback<typename statement_t::result_type> done_t>
  void execute_on(const statement_t& statement, const schema::TableName* table, done_t&& done, const std::optional<ReadOptions>& read) noexcept {
    using result_t = statement_t::result_type;
    guarded(std::forward<done_t>(done), detail::FailWithNoRows<result_t>{}, [&](const auto& once) {
      send_for_rows<result_t>(detail::wire_on(statement, table), once, read);
    });
  }

  /** @brief What every `stream_into` runs. */
  template <RunnableStatement statement_t, RowCallback<typename statement_t::result_type> handler_t, detail::StreamCallback done_t>
  void stream_on(
      const statement_t& statement, const schema::TableName* table, handler_t&& on_row, done_t&& done, const std::optional<ReadOptions>& read
  ) noexcept {
    guarded(std::forward<done_t>(done), detail::FailWithError{}, [&](const auto& once) {
      send_streamed<typename statement_t::result_type>(detail::wire_on(statement, table), std::forward<handler_t>(on_row), once, read);
    });
  }

  /** @brief What every command's `execute_into` runs. */
  template <CommandStatement statement_t, detail::CommandCallback done_t>
  void command_on(const statement_t& command, const schema::TableName* table, done_t&& done) noexcept {
    guarded(std::forward<done_t>(done), detail::FailWithNoCount{}, [&](const auto& once) {
      detail::WireStatement wired = detail::wire_on(command, table);
      command_into(std::move(wired.sql), std::move(wired.params), once);
    });
  }

  /** @return A future that completes once the statement `sql_t` holds (`CreateTableSql`, say) has run, or holds its error. */
  template <typename sql_t>
  [[nodiscard]] std::future<void> run_without_rows(const schema::TableName* table) {
    return util::future_from<void>([&](auto done) {
      guarded(std::move(done), detail::FailWithError{}, [&](const auto& once) {
        command_into(detail::sql_on<sql_t>(table), {}, [once](std::uint64_t /*affected*/, const std::exception_ptr& error) { once(error); });
      });
    });
  }

  /** @brief What every `select_by_key_values` runs. */
  template <TextSchema schema_t, util::CTString key_col>
  [[nodiscard]] std::future<std::vector<typename schema_t::row_type>> select_by_key_values_on(
      std::span<const key_type<schema_t, key_col>> keys, const schema::TableName* table
  ) {
    using row_t = schema_t::row_type;
    if ( keys.empty() ) {
      return detail::ready_future(std::vector<row_t>{});
    }

    return util::future_from<std::vector<row_t>>([&](auto done) {
      guarded(std::move(done), detail::FailWithNoRows<row_t>{}, [&](const auto& once) {
        auto statement = by_key_values<schema_t, key_col>(keys);
        send_for_rows<row_t>(table != nullptr ? detail::wire(std::move(statement).on(*table)) : detail::wire(statement), once, std::nullopt);
      });  // moved onto `table`: the keys are copied once
    });
  }

  /** @brief What every `insert_into` runs, and through it every `insert`. */
  template <TextSchema schema_t, schema::InsertRowOf<schema_t> row_t, detail::StreamCallback done_t>
  void insert_rows_into(std::span<const row_t> rows, const schema::TableName* table, done_t&& done) noexcept {
    guarded(std::forward<done_t>(done), detail::FailWithError{}, [&](const auto& once) {
      if ( rows.empty() ) {  // nothing to send, but `done` still runs on a worker, as it would after an insert
        _client->post([once] { once(std::exception_ptr{}); });
        return;
      }

      run_inserts(detail::build_insert<schema_t, row_t>(rows, table), once);
    });
  }

  /** @brief What every `insert` runs: `insert_rows_into`, with a future, which for no rows is complete already. */
  template <TextSchema schema_t, schema::InsertRowOf<schema_t> row_t>
  [[nodiscard]] std::future<void> insert_rows(std::span<const row_t> rows, const schema::TableName* table = nullptr) {
    if ( rows.empty() ) {
      return detail::ready_future();
    }

    return util::future_from<void>([&](auto done) { insert_rows_into<schema_t, row_t>(rows, table, std::move(done)); });
  }

  /** @brief What every `insert_returning` runs: the inserts, each ending in `RETURNING`, read once the last has finished. */
  template <TextSchema schema_t, schema::InsertRowOf<schema_t> row_t, detail::ExecuteCallback<typename schema_t::row_type> done_t>
  void insert_rows_returning_into(
      std::span<const row_t> rows, const std::optional<ReadOptions>& read, const schema::TableName* table, done_t&& done
  ) noexcept {
    using written_t = schema_t::row_type;
    guarded(std::forward<done_t>(done), detail::FailWithNoRows<written_t>{}, [&](const auto& once) {
      std::vector<detail::InsertStatement> statements = detail::build_insert<schema_t, row_t>(rows, table);
      for ( detail::InsertStatement& statement : statements ) {
        statement.sql += detail::ReturningSql<schema_t>::VALUE;
      }

      const auto results = std::make_shared<detail::KeptResults>();
      results->format = BinaryReadableRow<written_t> ? read.value_or(_read).format : ResultFormat::Text;

      run_inserts(
          std::move(statements),
          [once, results](const std::exception_ptr& error) {
            if ( error ) {
              once(std::vector<written_t>{}, error);
              return;
            }

            std::vector<written_t> written;
            std::exception_ptr     unread;
            try {
              written = detail::read_all<written_t>(results->results);
            } catch ( ... ) {
              unread = std::current_exception();
            }

            once(unread ? std::vector<written_t>{} : std::move(written), unread);
          },
          results
      );
    });
  }

  /** @brief `insert_rows_returning_into`, with a future, which for no rows is complete already. */
  template <TextSchema schema_t, schema::InsertRowOf<schema_t> row_t>
  [[nodiscard]] std::future<std::vector<typename schema_t::row_type>> insert_rows_returning(
      std::span<const row_t> rows, const std::optional<ReadOptions>& read, const schema::TableName* table = nullptr
  ) {
    using written_t = schema_t::row_type;
    if ( rows.empty() ) {
      return detail::ready_future(std::vector<written_t>{});
    }

    return util::future_from<std::vector<written_t>>([&](auto done) {
      insert_rows_returning_into<schema_t, row_t>(rows, read, table, std::move(done));
    });
  }

  // ---- sending, once a statement is wired: these throw, so they run inside `guarded` ---------------------------------------

  /** @brief Sends `wired` and calls `done` with every row it returns, read as `result_t`s. */
  template <TextRow result_t, detail::ExecuteCallback<result_t> done_t>
  void send_for_rows(detail::WireStatement wired, done_t done, const std::optional<ReadOptions>& read) {
    const auto rows = std::make_shared<std::vector<result_t>>();

    send_streamed<result_t>(
        std::move(wired), [rows](result_t&& row) { rows->push_back(std::move(row)); },
        [rows, done = std::move(done)](const std::exception_ptr& error) mutable { done(error ? std::vector<result_t>{} : std::move(*rows), error); },
        read
    );
  }

  /** @brief Sends `wired`, calls `on_row` with each row it returns, read as a `result_t`, and then `done`. */
  template <TextRow result_t, RowCallback<result_t> handler_t, detail::StreamCallback done_t>
  void send_streamed(detail::WireStatement wired, handler_t on_row, done_t done, const std::optional<ReadOptions>& read) {
    ReadOptions options = read.value_or(_read);
    options.format = BinaryReadableRow<result_t> ? options.format : ResultFormat::Text;

    send_stream(
        std::move(wired.sql), std::move(wired.params), options,
        [column_names = std::move(wired.column_names), on_row = std::move(on_row)](QueryResult chunk) mutable {
          detail::check_result_columns(column_names, chunk);
          return read_rows<result_t>(chunk, on_row);  // false: `on_row` wants no more rows, stop the query
        },
        std::move(done)
    );
  }

  /** @brief Runs `sql`, which returns no rows, and calls `done` with the count of rows it affected and the error (null on success). */
  template <detail::CommandCallback done_t>
  void command_into(std::string sql, std::vector<std::string> params, done_t done) {
    send(std::move(sql), std::move(params), [done = std::move(done)](const QueryResult& result, const std::exception_ptr& error) mutable {
      done(error ? 0 : result.affected_rows(), error);
    });
  }

  /**
   * @brief Runs the statements of an insert, all at once, so that nothing comes between them: in the transaction, on one; else
   * one statement on the pool, or several in a transaction of their own. `results`, if given, gets each statement's result.
   */
  template <detail::StreamCallback done_t>
    requires std::copy_constructible<done_t>
  void run_inserts(std::vector<detail::InsertStatement> statements, done_t done, const detail::Results& results = {}) {
    if ( statements.size() > 1 && ! _on_lease ) {
      detail::run_atomically(*_client, std::move(statements), std::move(done), results);
    } else {
      detail::send_all(*_client, _on_lease ? &_lease : nullptr, std::move(statements), std::move(done), results);
    }
  }

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

 public:
  StatementRunner(const StatementRunner&) = delete;
  StatementRunner& operator=(const StatementRunner&) = delete;

 protected:
  StatementRunner(StatementRunner&&) noexcept = default;
  StatementRunner& operator=(StatementRunner&&) noexcept = default;
  ~StatementRunner() = default;
};

}  // namespace schemacht::postgres
