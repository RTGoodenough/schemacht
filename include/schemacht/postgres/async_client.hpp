#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "schemacht/postgres/session_settings.hpp"

struct pg_result;  // libpq's PGresult; kept out of this header

namespace schemacht::postgres {

/** @brief A statement Postgres rejected. */
class QueryError : public std::runtime_error {
 public:
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- a message and a code; the names and the one call site keep them apart
  QueryError(const std::string& message, std::string sqlstate) : std::runtime_error(message), _sqlstate(std::move(sqlstate)) {}

  QueryError(const QueryError&) = default;
  QueryError(QueryError&&) noexcept = default;
  QueryError& operator=(const QueryError&) = default;
  QueryError& operator=(QueryError&&) noexcept = default;
  ~QueryError() override = default;

  /** @return The five-character SQLSTATE code (e.g. `42P01`, undefined table); empty if the failure wasn't the server's. */
  [[nodiscard]] const std::string& sqlstate() const noexcept { return _sqlstate; }

 private:
  std::string _sqlstate;
};

/** @brief The rows a statement returned, as libpq holds them: one field at a time, as text or (if the statement asked for it, see `StreamOptions::binary`) in PostgreSQL's binary form. Move-only. */
class QueryResult {
 public:
  QueryResult() = default;
  QueryResult(const QueryResult&) = delete;
  QueryResult(QueryResult&&) noexcept = default;
  QueryResult& operator=(const QueryResult&) = delete;
  QueryResult& operator=(QueryResult&&) noexcept = default;
  ~QueryResult() = default;

  [[nodiscard]] std::size_t row_count() const noexcept;
  [[nodiscard]] std::size_t column_count() const noexcept;

  /**
   * @return The name of a result column, as the statement calls it (`AS name`).
   * @throws std::out_of_range if there is no such column.
   */
  [[nodiscard]] std::string_view column_name(std::size_t column) const;

  /**
   * @return The text of one field, valid while this result lives, or `std::nullopt` if the field is NULL (which is not
   * the same as an empty string).
   * @throws std::out_of_range if `row` or `column` doesn't exist.
   */
  [[nodiscard]] std::optional<std::string_view> text(std::size_t row, std::size_t column) const;

  /** @return Whether the fields are in binary form (`binary`) and not text (`text`); false for a result with no columns. */
  [[nodiscard]] bool is_binary() const noexcept;

  /**
   * @return The bytes of one field of a binary result, valid while this result lives, or `std::nullopt` if the field is NULL.
   * @throws std::out_of_range if `row` or `column` doesn't exist.
   * @throws std::logic_error if the result is text.
   */
  [[nodiscard]] std::optional<std::span<const std::byte>> binary(std::size_t row, std::size_t column) const;

  /**
   * @return The OID of the PostgreSQL type of a result column (`pg_type.oid`).
   * @throws std::out_of_range if there is no such column.
   */
  [[nodiscard]] std::uint32_t column_type(std::size_t column) const;

 private:
  friend class AsyncClient;

  struct Deleter {
    void operator()(pg_result* raw) const noexcept;
  };

  explicit QueryResult(pg_result* raw) : _raw(raw) {}

  std::unique_ptr<pg_result, Deleter> _raw;
};

/** @brief Rows in each chunk of a stream unless told otherwise; in measurements, faster than 100 or 10,000. */
inline constexpr std::size_t DEFAULT_STREAM_CHUNK_ROWS = 1000;

/**
 * @brief Runs statements on a pool of Postgres connections without ever blocking a thread on the network.
 *
 * - **Non-blocking IO.** Each connection is in libpq's non-blocking mode. One reactor thread `poll()`s the sockets of
 *   the statements in flight and advances each as its socket becomes ready, so a slow query costs a connection, not a
 *   thread.
 * - **A pool of connections,** each running one statement at a time. Statements beyond the pool's size queue up and
 *   start as connections free.
 * - **A pool of worker threads** runs the completions, so parsing a large result never stalls the reactor. The workers
 *   run nothing else: cancels and reconnects are driven by the reactor too.
 *
 * @warning A completion or chunk handler must not wait on the result of another statement of the same client (a
 * `future::get()` on it, say): that statement's completion needs a worker, and with every worker waiting, none is left to
 * run it. Chain the next step from the completion instead (`Database::execute_into`, `Database::stream_into`).
 *
 * Thread-safe: `run` may be called from any thread. Statements run in autocommit mode, one round trip each.
 *
 * `postgres::Database` is the typed interface built on it.
 */
class AsyncClient {
 public:
  struct Options {
    std::size_t     connections = 4;  ///< Connections opened up front; also the most statements in flight at once.
    std::size_t     workers = 2;      ///< Threads running completions.
    SessionSettings session;          ///< Applied to every connection, when it is opened and whenever it is re-opened after a lost link.
  };

  /** @brief Called on a worker thread once a statement finishes. On failure the result is empty and the exception set. */
  using Completion = std::move_only_function<void(QueryResult, std::exception_ptr)>;

  /** @brief How `stream` reads a statement. */
  struct StreamOptions {
    std::size_t chunk_rows = DEFAULT_STREAM_CHUNK_ROWS;  ///< Rows in each chunk (the last has what is left).
    std::size_t max_buffered_chunks = 4;  ///< Chunks read but not yet handled, before the client stops reading and the server has to wait.
    bool        binary =
        false;  ///< Ask for the result in PostgreSQL's binary form: read it with `QueryResult::binary`, not `text` (which would return garbage).
  };

  /** @brief Called on a worker thread with each chunk of a stream, in turn. Returns whether to go on: `false` stops the stream. */
  using ChunkHandler = std::move_only_function<bool(QueryResult)>;

  /** @brief Called once, on a worker thread, when a stream ends: with no error if it ran to its end or was stopped. */
  using StreamCompletion = std::move_only_function<void(std::exception_ptr)>;

  /**
   * @brief Opens `options.connections` connections and starts the reactor and workers.
   * @throws std::runtime_error if a connection can't be made or the session settings can't be applied.
   * @throws std::invalid_argument if `options` asks for no connections or no workers.
   */
  explicit AsyncClient(const std::string& conninfo, Options options);
  explicit AsyncClient(const std::string& conninfo) : AsyncClient(conninfo, Options{}) {}

  AsyncClient(const AsyncClient&) = delete;
  AsyncClient(AsyncClient&&) = delete;
  AsyncClient& operator=(const AsyncClient&) = delete;
  AsyncClient& operator=(AsyncClient&&) = delete;

  /**
   * @brief Stops accepting statements and waits for the ones in flight. Statements still queued complete with an
   * error, so every `Completion` handed in is called exactly once.
   */
  ~AsyncClient();

  /**
   * @brief Starts a statement and returns at once.
   *
   * @param sql    The statement, with `$1`, `$2`, ... for arguments.
   * @param params The text of each argument, in order.
   * @param done   Called exactly once, on a worker thread, with the rows or the error (`QueryError` for one the server
   *               reported, `std::runtime_error` for a lost connection or a client that shut down).
   */
  void run(std::string sql, std::vector<std::string> params, Completion done);

  /**
   * @brief Starts a statement whose rows are handled as they arrive, `options.chunk_rows` at a time, and returns at once.
   *
   * Nothing holds the whole result: memory is about `chunk_rows` x (`max_buffered_chunks` + 1) rows however large it is, and
   * the first rows are handled while the server is still producing the rest.
   *
   * - **In order, one at a time.** `on_chunk` gets each chunk on a worker thread, never two at once, and parsing it there
   *   leaves the reactor free.
   * - **Backpressure.** When `max_buffered_chunks` chunks wait for `on_chunk`, the client stops reading the socket, so the
   *   server's send buffer fills and it waits (`ClientWrite`), instead of the rows piling up here.
   * - **Stopping.** `on_chunk` returning `false` (or throwing) ends the stream: the statement is cancelled on the server,
   *   what is still in flight is dropped, and the connection goes back to the pool once the cancel is done.
   * - **Errors.** `done` gets the error, if any, once every chunk read before it has been handled. A chunk the server was
   *   still building when the statement failed is not delivered, so rows before the failing one arrive only up to a chunk.
   * - **Shutdown.** Destroying the client stops its streams, which then end with an error.
   *
   * @param sql     The statement, with `$1`, `$2`, ... for arguments.
   * @param params  The text of each argument, in order.
   * @param options How to read it.
   * @param on_chunk Called with each chunk (a `QueryResult` of up to `chunk_rows` rows).
   * @param done    Called exactly once, on a worker thread, when the stream has ended.
   * @throws std::invalid_argument if `options` asks for a chunk of no rows or no buffered chunks.
   */
  void stream(std::string sql, std::vector<std::string> params, StreamOptions options, ChunkHandler on_chunk, StreamCompletion done);

  /** @brief Runs `task` on the worker threads. */
  void post(std::move_only_function<void()> task);

 private:
  class Impl;
  std::unique_ptr<Impl> _impl;
};

}  // namespace schemacht::postgres
