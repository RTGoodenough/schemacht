#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
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
#include "schemacht/query/row_sink.hpp"
#include "schemacht/schema/schema.hpp"
#include "schemacht/util/compile_time.hpp"

/**
 * A Postgres database, as a pool of connections that every schema reads and writes through: the same schemas, queries and row types
 * for any table, run on an `Async_Client` -- non-blocking IO over a pool of connections -- so many can be in flight from any threads.
 *
 * Every call returns a `std::future` at once and the work finishes in the background (a caller that wants to block calls `get()`). Each call is one
 * statement in autocommit mode, so there are no transactions spanning calls.
 *
 * @note Errors, including a statement that can't be started, arrive through the future, never as a throw from the call itself.
 */
namespace schemacht::postgres {

namespace detail {

/**
 * @brief Pushes the rows of a chunk of a result into `sink`, whichever form the result came in: each field goes to the sink as its text or its bytes, and the sink,
 * which knows the type of each column it builds, parses it.
 * @param batch Anything with `row_count()`, `column_count()`, `is_binary()`, `column_type(column)`, and `text(row, column)` or `binary(row, column)`
 * (`Query_Result` is one).
 * @param expected_columns How many columns the query returns.
 * @return Whether the sink wants more rows (false: it stopped the stream, and the rows after that were not pushed).
 * @throws std::runtime_error if the result does not have `expected_columns` columns, or a binary result has a column of a type the sink's field does not read.
 */
template <typename batch_t>
bool fill_sink(query::Row_Sink& sink, std::size_t expected_columns, const batch_t& batch) {
  if ( batch.column_count() != expected_columns && batch.row_count() > 0 ) {
    throw std::runtime_error("the result has " + std::to_string(batch.column_count()) + " columns, and the query returns " +
                             std::to_string(expected_columns));
  }
  const bool binary = batch.is_binary();
  if ( binary ) {
    for ( std::size_t position = 0; position < expected_columns; ++position ) {
      sink.check_binary_type(position, batch.column_type(position));
    }
  }
  sink.reserve(batch.row_count());
  for ( std::size_t row = 0; row < batch.row_count(); ++row ) {
    sink.begin_row();
    for ( std::size_t position = 0; position < expected_columns; ++position ) {
      if ( binary ) {
        if ( const auto bytes = batch.binary(row, position); bytes.has_value() ) {
          sink.put_binary(position, *bytes);
        } else {
          sink.put_null(position);
        }
      } else if ( const auto text = batch.text(row, position); text.has_value() ) {
        sink.put_text(position, *text);
      } else {
        sink.put_null(position);
      }
    }
    sink.end_row();
    if ( ! sink.wants_more() ) {
      return false;
    }
  }
  return true;
}

/** @brief Whether `done_t` can finish an `execute_into` for `result_t`: called once with the rows and the error (null on success). */
template <typename done_t, typename result_t>
concept ExecuteCompletion = std::invocable<done_t&, std::vector<result_t>, std::exception_ptr> &&
                            std::same_as<std::invoke_result_t<done_t&, std::vector<result_t>, std::exception_ptr>, void>;

/** @brief Whether `done_t` can finish a `stream_into`: called once with the error (null on success). */
template <typename done_t>
concept StreamCompletion = std::invocable<done_t&, std::exception_ptr> && std::same_as<std::invoke_result_t<done_t&, std::exception_ptr>, void>;

/**
 * @brief `void(value_t, std::exception_ptr)`, or `void(std::exception_ptr)` when `value_t` is `void` (there is nothing else to hand
 * back): a partial specialization, not `std::conditional_t`, because a function type cannot take a `void` argument -- the other
 * branch must never be formed, not merely go unused.
 */
template <typename value_t>
struct CompletionOf {
  using type = std::move_only_function<void(value_t, std::exception_ptr)>;
};

template <>
struct CompletionOf<void> {
  using type = std::move_only_function<void(std::exception_ptr)>;
};

/**
 * @brief The state shared between a running query and whoever is told when it finishes: the sink the rows go into, and the completion.
 *
 * @tparam sink_t  The sink: a `Row_Collector` (`value_t` is its collected rows) or a `Row_Handler` (`value_t` is `void`).
 * @tparam value_t What `finish` hands the completion: the collected rows, or nothing.
 */
template <typename sink_t, typename value_t>
struct Execution {
  using Completion = CompletionOf<value_t>::type;

  template <typename... arg_ts>
  explicit Execution(Completion completion, arg_ts&&... args) : sink(std::forward<arg_ts>(args)...), done(std::move(completion)) {}

  /** @brief Calls the completion with every row that was pushed (nothing, if `value_t` is `void`) and no error. */
  void finish() {
    if constexpr ( std::is_void_v<value_t> ) {
      done({});
    } else {
      done(sink.take(), {});
    }
  }

  /** @brief Calls the completion with `error` (and a default-constructed value, if `value_t` is not `void`: never looked at). */
  void fail(const std::exception_ptr& error) {
    if constexpr ( std::is_void_v<value_t> ) {
      done(error);
    } else {
      done(value_t{}, error);
    }
  }

  sink_t     sink;
  Completion done;
};

/** @return A future that is already complete with `value`. */
template <typename value_t>
[[nodiscard]] std::future<value_t> ready_future(value_t value) {
  std::promise<value_t> promise;
  promise.set_value(std::move(value));
  return promise.get_future();
}

}  // namespace detail

/**
 * @brief The form a query's result is asked for. `Binary` asks for PostgreSQL's binary form, which is less to send, format and parse (a geometry, a
 * `bytea` or a raster is half the bytes, and needs no hex). `Text` asks for text, which is easier to read when debugging.
 */
enum class ResultFormat : std::uint8_t {
  Binary,
  Text,
};

/** @brief How a result is read: in chunks of how many rows, how many chunks may wait for the consumer, and in which form. */
struct ReadOptions {
  std::size_t chunk_rows = DEFAULT_STREAM_CHUNK_ROWS;  ///< Rows in each chunk (`Async_Client::stream`).
  std::size_t max_buffered_chunks =
      Async_Client::StreamOptions{}
          .max_buffered_chunks;                ///< Chunks read but not yet handled, before the pool stops reading and the server has to wait.
  ResultFormat format = ResultFormat::Binary;  ///< The form of the results.
};

/**
 * @brief Postgres database with a pool of connections
 */
class Database {
 public:
  struct Options {
    Async_Client::Options pool;  ///< The connections, the threads that run completions, and the session settings.
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

  /** @return The pool, for what only it does: a statement given as text (`Async_Client::run`), or a task on its worker threads. */
  [[nodiscard]] Async_Client& pool() noexcept { return _client; }

  /**
   * @brief Runs `statement` in the background and calls `done` exactly once, on a worker thread, with every row (as `execute`
   * returns them) and the error (null on success).
   *
   * The primitive `execute` is built on: for chaining a statement's result into another asynchronous step (another `execute_into`
   * or `stream_into`, say) without a future in between that nothing ever waits on. Most callers want `execute`.
   * @tparam statement_t A query (`Prepared::bind`) of any schema, or a raw statement (`Raw_Statement::bind`).
   * @param read How to read the result; the database's default if not given.
   */
  template <RunnableStatement statement_t, detail::ExecuteCompletion<typename statement_t::result_type> done_t>
  void execute_into(const statement_t& statement, done_t done, const std::optional<ReadOptions>& read = std::nullopt) {
    using result_t = statement_t::result_type;
    static_assert(query::BinaryReadableRow<result_t>,
                  "reading results in PostgreSQL's binary form: every column the query returns needs a Field_Codec with parse_binary (see "
                  "HasBinaryForm)");
    using execution_t = detail::Execution<query::Row_Collector<result_t>, std::vector<result_t>>;
    start_into(detail::wire(statement), read, std::make_shared<execution_t>(std::move(done)));
  }

  /**
   * @return A future for every row satisfying `statement`, each a `statement_t::result_type` (a tuple of the returned columns' `Field`s;
   * the table's `row_type` unless the query projects). It holds the error instead if the statement failed (`Query_Error`), or its result
   * was not the columns it names (`std::runtime_error`).
   * @tparam statement_t A query (`Prepared::bind`) of any schema, or a raw statement (`Raw_Statement::bind`).
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
   * The primitive `stream` is built on: for chaining into another asynchronous step without a future nothing ever waits on
   * (`spatial::load_region` does this to build a mosaic once every tile has streamed in). Most callers want `stream`.
   */
  template <RunnableStatement statement_t, query::RowHandler<typename statement_t::result_type> handler_t, detail::StreamCompletion done_t>
  void stream_into(const statement_t& statement, handler_t on_row, done_t done, const std::optional<ReadOptions>& read = std::nullopt) {
    using result_t = statement_t::result_type;
    static_assert(query::BinaryReadableRow<result_t>,
                  "reading results in PostgreSQL's binary form: every column the query returns needs a Field_Codec with parse_binary (see "
                  "backend::HasBinaryForm)");
    using execution_t = detail::Execution<query::Row_Handler<result_t, handler_t>, void>;
    start_into(detail::wire(statement), read, std::make_shared<execution_t>(std::move(done), std::move(on_row)));
  }

  /**
   * @brief As `stream_into`, with the completion as the returned future instead of a callback: the future completes normally once
   * the stream ends (whether it ran to the end or `on_row` stopped it), or holds the query's error.
   */
  template <RunnableStatement statement_t, query::RowHandler<typename statement_t::result_type> handler_t>
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

  /** @return A future that completes once `rows` of `schema_t` are written. Generated columns (see `Field::GENERATED`) are never sent. */
  template <schema::IsSchema schema_t>
  [[nodiscard]] std::future<void> insert(std::vector<typename schema_t::row_type> rows) {
    const auto        promise = std::make_shared<std::promise<void>>();
    std::future<void> future = promise->get_future();
    try {
      if ( rows.empty() ) {
        promise->set_value();
        return future;
      }
      detail::InsertStatement statement = detail::build_insert<schema_t>(rows);
      _client.run(std::move(statement.sql), std::move(statement.values),
                  [promise](const Query_Result& /*inserted*/, const std::exception_ptr& error) {
                    if ( error ) {
                      promise->set_exception(error);
                    } else {
                      promise->set_value();
                    }
                  });
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
  /** @brief Starts `statement` into `execution`'s sink; `execution`'s completion is called when the pool is done with it, on a worker thread. */
  template <typename execution_t>
  void start_into(detail::WireStatement statement, const std::optional<ReadOptions>& read, const std::shared_ptr<execution_t>& execution) {
    try {
      const ReadOptions& how = read.has_value() ? *read : _read;
      // The pool may still be working after this returns: it gets its own copy of the statement's text and arguments, and shares ownership of the sink.
      const std::shared_ptr<query::Row_Sink> sink(execution, &execution->sink);
      const Async_Client::StreamOptions      options{
               .chunk_rows = how.chunk_rows, .max_buffered_chunks = how.max_buffered_chunks, .binary = how.format == ResultFormat::Binary};
      std::string              sql(statement.sql);
      std::vector<std::string> params = std::move(statement.params);
      _client.stream(
          std::move(sql), std::move(params), options,
          [statement, sink](Query_Result chunk) {
            detail::check_result_columns(statement, chunk.row_count(), chunk.column_count(),
                                         [&chunk](std::size_t column) { return chunk.column_name(column); });
            return detail::fill_sink(*sink, statement.column_names.size(), chunk);  // false: the sink is done, stop the query
          },
          [execution](const std::exception_ptr& error) {
            if ( error ) {
              execution->fail(error);
            } else {
              execution->finish();
            }
          });
    } catch ( ... ) {
      execution->fail(std::current_exception());
    }
  }

  Async_Client _client;
  ReadOptions  _read;
};

}  // namespace schemacht::postgres
