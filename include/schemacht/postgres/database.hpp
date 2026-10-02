#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "schemacht/postgres/async_client.hpp"
#include "schemacht/postgres/key_lookup.hpp"
#include "schemacht/postgres/statements.hpp"
#include "schemacht/postgres/row_reader.hpp"
#include "schemacht/schema/schema.hpp"
#include "schemacht/util/compile_time.hpp"

/**
 * A Postgres database: a pool of connections (an `AsyncClient`) that runs typed queries, raw statements and inserts for
 * any schema, from any thread.
 *
 * Every call returns a `std::future` at once and the work finishes in the background (call `get()` to wait). Each call
 * is one statement in autocommit mode: there are no transactions spanning calls.
 *
 * @note Errors, including a statement that can't be started, arrive through the future, never as a throw from the call itself.
 *
 * @warning Row handlers and completions run on the pool's worker threads. They must not block on another of this database's
 * futures (`get()`), which could leave no worker free to complete it; chain with `execute_into` / `stream_into` instead.
 */
namespace schemacht::postgres {

namespace detail {

/** @brief Whether `done_t` can finish an `execute_into` for `result_t`: called once with the rows and the error (null on success). */
template <typename done_t, typename result_t>
concept ExecuteCallback = std::invocable<done_t&, std::vector<result_t>, std::exception_ptr> &&
                          std::same_as<std::invoke_result_t<done_t&, std::vector<result_t>, std::exception_ptr>, void>;

/** @brief Whether `done_t` can finish a `stream_into`: called once with the error (null on success). */
template <typename done_t>
concept StreamCallback = std::invocable<done_t&, std::exception_ptr> && std::same_as<std::invoke_result_t<done_t&, std::exception_ptr>, void>;

/**
 * @brief What a running statement hands its rows to (`take`, one at a time, never from two threads at once) and then tells
 * once that it ended (`finish`, with the error or null).
 */
template <typename execution_t, typename result_t>
concept Execution = RowConsumer<execution_t, result_t> && requires(execution_t& execution, const std::exception_ptr& error) {
  { execution.finish(error) } -> std::same_as<void>;
};

/** @brief The execution of `execute_into`: keeps every row, and hands them all to `done` at the end. */
template <schema::FieldTuple result_t, ExecuteCallback<result_t> done_t>
class Collector {
 public:
  explicit Collector(done_t done) : _done(std::move(done)) {}

  bool take(result_t&& row) {
    _rows.push_back(std::move(row));
    return true;
  }

  void finish(const std::exception_ptr& error) {
    if ( error ) {
      _done({}, error);
    } else {
      _done(std::move(_rows), {});
    }
  }

 private:
  std::vector<result_t> _rows;
  done_t                _done;
};

/** @brief The execution of `stream_into`: hands each row to `on_row` and keeps none; `on_row` returning `false` stops it. */
template <schema::FieldTuple result_t, RowCallback<result_t> handler_t, StreamCallback done_t>
class Streamer {
 public:
  Streamer(handler_t on_row, done_t done) : _on_row(std::move(on_row)), _done(std::move(done)) {}

  bool take(result_t&& row) {
    if constexpr ( std::same_as<std::invoke_result_t<handler_t&, result_t&&>, bool> ) {
      return std::invoke(_on_row, std::move(row));
    } else {
      std::invoke(_on_row, std::move(row));
      return true;
    }
  }

  void finish(const std::exception_ptr& error) { _done(error); }

 private:
  handler_t _on_row;
  done_t    _done;
};

/** @return A future that is already complete with `value`. */
template <typename value_t>
[[nodiscard]] std::future<value_t> ready_future(value_t value) {
  std::promise<value_t> promise;
  promise.set_value(std::move(value));
  return promise.get_future();
}

}  // namespace detail

/** @brief How a result is read: in chunks of how many rows, how many chunks may wait for the consumer, and in which form. */
struct ReadOptions {
  std::size_t chunk_rows = DEFAULT_STREAM_CHUNK_ROWS;  ///< Rows in each chunk (`AsyncClient::stream`).
  std::size_t max_buffered_chunks =
      AsyncClient::StreamOptions{}
          .max_buffered_chunks;                ///< Chunks read but not yet handled, before the pool stops reading and the server has to wait.
  ResultFormat format = ResultFormat::Binary;  ///< The form of the results. A row with a column that has no binary form (see `HasBinaryForm`) is always read as text.
};

/**
 * @brief Postgres database with a pool of connections
 */
class Database {
 public:
  struct Options {
    AsyncClient::Options pool;  ///< The connections, the threads that run completions, and the session settings.
    ReadOptions           read;  ///< How results are read, unless a call says otherwise.
  };

  /**
   * @brief Opens the pool: `options.pool.connections` connections to the database, and the reactor and worker threads.
   * @throws std::runtime_error if a connection can't be made or the session settings can't be applied.
   * @throws std::invalid_argument if `options` asks for no connections, no workers, or an empty chunk.
   */
  Database(const std::string& conninfo, Options options) : _client(conninfo, options.pool), _read(options.read) {}
  explicit Database(const std::string& conninfo) : Database(conninfo, Options{}) {}

  Database(const Database&) = delete;  // it owns the pool: one per database
  Database(Database&&) = delete;
  Database& operator=(const Database&) = delete;
  Database& operator=(Database&&) = delete;
  ~Database() = default;

  /** @return The pool, for what only it does: a statement given as text (`AsyncClient::run`), or a task on its worker threads. */
  [[nodiscard]] AsyncClient& pool() noexcept { return _client; }

  /**
   * @brief Runs `statement` in the background and calls `done` exactly once, on a worker thread, with every row (as `execute`
   * returns them) and the error (null on success).
   *
   * The primitive `execute` is built on: for chaining a statement's result into another asynchronous step (another `execute_into`
   * or `stream_into`, say) without a future in between that nothing ever waits on. Most callers want `execute`.
   * @tparam statement_t A query (`Prepared::bind`) of any schema, or a raw statement (`RawStatement::bind`).
   * @param read How to read the result; the database's default if not given.
   */
  template <RunnableStatement statement_t, detail::ExecuteCallback<typename statement_t::result_type> done_t>
  void execute_into(const statement_t& statement, done_t done, const std::optional<ReadOptions>& read = std::nullopt) {
    using result_t = statement_t::result_type;
    start_into<result_t>(detail::wire(statement), read, std::make_shared<detail::Collector<result_t, done_t>>(std::move(done)));
  }

  /**
   * @return A future for every row satisfying `statement`, each a `statement_t::result_type` (a tuple of the returned columns' `Field`s;
   * the table's `row_type` unless the query projects). It holds the error instead if the statement failed (`QueryError`), or its result
   * was not the columns it names (`std::runtime_error`).
   * @tparam statement_t A query (`Prepared::bind`) of any schema, or a raw statement (`RawStatement::bind`).
   * @param read How to read the result; the database's default if not given.
   */
  template <RunnableStatement statement_t>
  [[nodiscard]] std::future<std::vector<typename statement_t::result_type>> execute(const statement_t&                statement,
                                                                                    const std::optional<ReadOptions>& read = std::nullopt) {
    using result_t = statement_t::result_type;
    const auto                         promise = std::make_shared<std::promise<std::vector<result_t>>>();
    std::future<std::vector<result_t>> future = promise->get_future();
    execute_into(
        statement,
        [promise](std::vector<result_t> rows, const std::exception_ptr& error) {
          if ( error ) {
            promise->set_exception(error);
          } else {
            promise->set_value(std::move(rows));
          }
        },
        read);
    return future;
  }

  /**
   * @brief Runs `statement` in the background, calls `on_row` with each row (a `statement_t::result_type`, as `execute` returns
   * them) in order, and calls `done` exactly once, on a worker thread, when the stream ends, with the error (null on success).
   *
   * `on_row` is called one row at a time, never from two threads at once, on a thread of the pool's (not the caller's), and
   * returns `void`, or a `bool` where `false` stops: no more rows are delivered and the query is cancelled. If `on_row` throws,
   * that is the stream's error; rows delivered before an error (the database's or `on_row`'s) stay delivered.
   *
   * The primitive `stream` is built on: for chaining into another asynchronous step without a future in between. Most
   * callers want `stream`.
   */
  template <RunnableStatement statement_t, RowCallback<typename statement_t::result_type> handler_t, detail::StreamCallback done_t>
  void stream_into(const statement_t& statement, handler_t on_row, done_t done, const std::optional<ReadOptions>& read = std::nullopt) {
    using result_t = statement_t::result_type;
    start_into<result_t>(detail::wire(statement), read,
                         std::make_shared<detail::Streamer<result_t, handler_t, done_t>>(std::move(on_row), std::move(done)));
  }

  /**
   * @brief As `stream_into`, with the completion as the returned future instead of a callback: the future completes normally once
   * the stream ends (whether it ran to the end or `on_row` stopped it), or holds the query's error.
   */
  template <RunnableStatement statement_t, RowCallback<typename statement_t::result_type> handler_t>
  [[nodiscard]] std::future<void> stream(const statement_t& statement, handler_t on_row, const std::optional<ReadOptions>& read = std::nullopt) {
    const auto        promise = std::make_shared<std::promise<void>>();
    std::future<void> future = promise->get_future();
    stream_into(
        statement, std::move(on_row),
        [promise](const std::exception_ptr& error) {
          if ( error ) {
            promise->set_exception(error);
          } else {
            promise->set_value();
          }
        },
        read);
    return future;
  }

  /** @return A future for every row of `schema_t` whose `key_col` value is one of `keys`, e.g. `db.select_by_key_values<PlacesSchema, "id">(ids)`. */
  template <schema::IsSchema schema_t, util::CTString key_col>
  [[nodiscard]] std::future<std::vector<typename schema_t::row_type>> select_by_key_values(std::span<const key_type<schema_t, key_col>> keys) {
    if ( keys.empty() ) {
      return detail::ready_future(std::vector<typename schema_t::row_type>{});
    }
    return execute(by_key_values<schema_t, key_col>(keys));
  }

  /**
   * @return A future that completes once `rows` of `schema_t` are written, or holds the error. Generated columns (see
   * `Field::GENERATED`) are never sent. The rows go in one statement, atomically, unless the schema has an array-typed
   * column (see `detail::build_insert`).
   */
  template <schema::IsSchema schema_t>
  [[nodiscard]] std::future<void> insert(std::vector<typename schema_t::row_type> rows) {
    const auto        promise = std::make_shared<std::promise<void>>();
    std::future<void> future = promise->get_future();
    try {
      if ( rows.empty() ) {
        promise->set_value();
        return future;
      }
      run_in_order(std::make_shared<std::vector<detail::InsertStatement>>(detail::build_insert<schema_t>(rows)), 0, promise);
    } catch ( ... ) {
      promise->set_exception(std::current_exception());
    }
    return future;
  }

  /** @brief As above, copying `rows` (which the caller may then reuse or destroy at once). */
  template <schema::IsSchema schema_t>
  [[nodiscard]] std::future<void> insert(std::span<const typename schema_t::row_type> rows) {
    return insert<schema_t>(std::vector<typename schema_t::row_type>(rows.begin(), rows.end()));
  }

 private:
  /**
   * @brief Starts `statement`, whose rows `execution` takes as they arrive; `execution` is told when it ends (on a worker
   * thread), or at once if it cannot start. A row with a column that has no binary form is read as text.
   */
  template <schema::FieldTuple result_t, detail::Execution<result_t> execution_t>
  void start_into(detail::WireStatement statement, const std::optional<ReadOptions>& read, const std::shared_ptr<execution_t>& execution) {
    try {
      const ReadOptions how = [&] {
        ReadOptions chosen = read.has_value() ? *read : _read;
        if constexpr ( ! BinaryReadableRow<result_t> ) {
          chosen.format = ResultFormat::Text;
        }
        return chosen;
      }();
      const AsyncClient::StreamOptions options{
          .chunk_rows = how.chunk_rows, .max_buffered_chunks = how.max_buffered_chunks, .binary = how.format == ResultFormat::Binary};
      // The pool may still be working after this returns: it gets its own copy of the statement's text and arguments, and
      // shares ownership of the execution.
      std::string              sql(statement.sql);
      std::vector<std::string> params = std::move(statement.params);
      _client.stream(
          std::move(sql), std::move(params), options,
          [statement, execution](QueryResult chunk) {
            detail::check_result_columns(statement, chunk);
            return read_rows<result_t>(chunk, *execution);  // false: the execution wants no more rows, stop the query
          },
          [execution](const std::exception_ptr& error) { execution->finish(error); });
    } catch ( ... ) {
      execution->finish(std::current_exception());
    }
  }

  /** @brief Runs `statements` from `next` on, one after another, then completes `promise`; the first error stops it. */
  void run_in_order(const std::shared_ptr<std::vector<detail::InsertStatement>>& statements, std::size_t next,
                    const std::shared_ptr<std::promise<void>>& promise) {
    if ( next == statements->size() ) {
      promise->set_value();
      return;
    }
    detail::InsertStatement& statement = statements->at(next);
    _client.run(std::move(statement.sql), std::move(statement.values),
                [this, statements, next, promise](const QueryResult& /*inserted*/, const std::exception_ptr& error) {
                  if ( error ) {
                    promise->set_exception(error);
                  } else {
                    run_in_order(statements, next + 1, promise);
                  }
                });
  }

  AsyncClient _client;
  ReadOptions  _read;
};

}  // namespace schemacht::postgres
