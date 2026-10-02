#include "schemacht/postgres/async_client.hpp"

#include <fcntl.h>
#include <libpq-fe.h>
#include <poll.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "schemacht/async/thread_pool.hpp"

namespace schemacht::postgres {

namespace {

/** @brief The oldest libpq this client runs on: 17 has the chunked rows mode (`PQsetChunkedRowsMode`) and non-blocking cancel (`PQcancelCreate`). */
constexpr int MIN_LIBPQ_VERSION = 170000;
constexpr int LIBPQ_MAJOR_DIVISOR = 10000;

/** @return `message` without the trailing newline libpq adds. */
std::string trimmed(const char* message) {
  std::string text = message != nullptr ? message : "";
  while ( ! text.empty() && (text.back() == '\n' || text.back() == '\r') ) {
    text.pop_back();
  }
  return text;
}

std::exception_ptr runtime_error_ptr(const std::string& message) { return std::make_exception_ptr(std::runtime_error(message)); }

std::exception_ptr query_error_ptr(const pg_result* result) {
  const char* state = PQresultErrorField(result, PG_DIAG_SQLSTATE);
  return std::make_exception_ptr(QueryError(trimmed(PQresultErrorMessage(result)), state != nullptr ? state : ""));
}

// poll() flags are small signed ints; combine and test them as unsigned to keep the bit arithmetic well-defined.
constexpr std::int16_t poll_events(int first, int second = 0) {
  return static_cast<std::int16_t>(static_cast<unsigned>(first) | static_cast<unsigned>(second));
}

constexpr unsigned READABLE =
    static_cast<unsigned>(POLLIN) | static_cast<unsigned>(POLLERR) | static_cast<unsigned>(POLLHUP) | static_cast<unsigned>(POLLNVAL);

bool has_any(std::int16_t events, unsigned mask) { return (static_cast<unsigned>(events) & mask) != 0; }

bool is_success(ExecStatusType status) {
  return status == PGRES_TUPLES_OK || status == PGRES_TUPLES_CHUNK || status == PGRES_COMMAND_OK || status == PGRES_EMPTY_QUERY;
}

/** @brief SQLSTATE of a statement that was cancelled: what the server reports after our own cancel. */
constexpr std::string_view CANCELED_SQLSTATE = "57014";

/**
 * @brief Runs the session's `SET` on a connection that is not yet in non-blocking mode.
 * @return "" if it worked (or there was nothing to set), else why not.
 */
std::string apply_session(PGconn* connection, std::string_view setup) {
  if ( setup.empty() ) {
    return "";
  }
  pg_result*        result = PQexec(connection, std::string(setup).c_str());
  const bool        worked = result != nullptr && PQresultStatus(result) == PGRES_COMMAND_OK;
  const std::string message = worked ? "" : trimmed(result != nullptr ? PQresultErrorMessage(result) : PQerrorMessage(connection));
  PQclear(result);
  return message;
}

}  // namespace

// ============================================================================
// QueryResult
// ============================================================================

void QueryResult::Deleter::operator()(pg_result* raw) const noexcept { PQclear(raw); }

QueryResult::QueryResult(pg_result* raw)
    : _raw(raw),
      _row_count(raw != nullptr ? static_cast<std::size_t>(PQntuples(raw)) : 0),
      _column_count(raw != nullptr ? static_cast<std::size_t>(PQnfields(raw)) : 0),
      _binary(_column_count > 0 && PQfformat(raw, 0) == 1) {}

QueryResult::QueryResult(QueryResult&& other) noexcept
    : _raw(std::move(other._raw)),
      _row_count(std::exchange(other._row_count, 0)),
      _column_count(std::exchange(other._column_count, 0)),
      _binary(std::exchange(other._binary, false)) {}

QueryResult& QueryResult::operator=(QueryResult&& other) noexcept {
  _raw = std::move(other._raw);
  _row_count = std::exchange(other._row_count, 0);
  _column_count = std::exchange(other._column_count, 0);
  _binary = std::exchange(other._binary, false);
  return *this;
}

std::string_view QueryResult::column_name(std::size_t column) const {
  if ( column >= _column_count ) {
    throw std::out_of_range("QueryResult::column_name: no such column");
  }
  return PQfname(_raw.get(), static_cast<int>(column));
}

std::uint32_t QueryResult::column_type(std::size_t column) const {
  if ( column >= _column_count ) {
    throw std::out_of_range("QueryResult::column_type: no such column");
  }
  return PQftype(_raw.get(), static_cast<int>(column));
}

std::optional<std::string_view> QueryResult::field(std::size_t row, std::size_t column) const {
  if ( row >= _row_count || column >= _column_count ) {
    throw std::out_of_range("QueryResult::field: no such field");
  }
  const int row_index = static_cast<int>(row);
  const int column_index = static_cast<int>(column);
  if ( PQgetisnull(_raw.get(), row_index, column_index) != 0 ) {
    return std::nullopt;
  }
  return std::string_view{PQgetvalue(_raw.get(), row_index, column_index), static_cast<std::size_t>(PQgetlength(_raw.get(), row_index, column_index))};
}

// ============================================================================
// AsyncClient::Impl
// ============================================================================

class AsyncClient::Impl {
 public:
  Impl(const std::string& conninfo, const Options& options) : _session_sql(options.session.sql()) {
    if ( PQlibVersion() < MIN_LIBPQ_VERSION ) {  // the library loaded at run time, which is not always the one built against
      throw std::runtime_error("AsyncClient needs libpq " + std::to_string(MIN_LIBPQ_VERSION / LIBPQ_MAJOR_DIVISOR) +
                               " or newer at run time, found " + std::to_string(PQlibVersion() / LIBPQ_MAJOR_DIVISOR));
    }
    if ( options.connections == 0 ) {
      throw std::invalid_argument("AsyncClient needs at least one connection");
    }
    _workers = std::make_unique<async::ThreadPool>(options.workers);

    try {
      open_connections(conninfo, options.connections);
      open_wake_pipe();
    } catch ( ... ) {
      close_everything();
      throw;
    }
    _idle = _connections;
    _reactor = std::thread([this] { reactor_loop(); });
  }

  Impl(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl& operator=(Impl&&) = delete;

  ~Impl() {
    {
      const std::scoped_lock lock(_mutex);
      _stopping = true;
    }
    wake();
    _reactor.join();
    _workers->shutdown();  // runs every completion still posted, while `_workers` is still valid for the ones that post more
    // A reconnect still in progress is dropped: its connection is closed below with the others.
    close_everything();
  }

  void run(std::string sql, std::vector<std::string> params, Completion done) {
    enqueue(Operation{.sql = std::move(sql), .params = std::move(params), .done = std::move(done), .stream = nullptr});
  }

  void stream(std::string sql, std::vector<std::string> params, StreamOptions options, ChunkHandler on_chunk, StreamCompletion done) {
    if ( options.chunk_rows == 0 || options.max_buffered_chunks == 0 ) {
      throw std::invalid_argument("a stream needs chunks of at least one row and at least one buffered chunk");
    }
    enqueue(Operation{
        .sql = std::move(sql),
        .params = std::move(params),
        .done = nullptr,
        .stream = std::make_shared<Stream>(options, std::move(on_chunk), std::move(done)),
    });
  }

  void post(std::move_only_function<void()> task) { _workers->post(std::move(task)); }

 private:
  /**
   * @brief What a streamed statement shares between the reactor, which reads its chunks, and the workers, which handle them.
   * Everything but `options`, `on_chunk` (only ever used by the one delivering task) and `done` is guarded by `mutex`.
   */
  struct Stream {
    Stream(StreamOptions stream_options, ChunkHandler chunk_handler, StreamCompletion completion)
        : options(stream_options), on_chunk(std::move(chunk_handler)), done(std::move(completion)) {}

    StreamOptions     options;
    ChunkHandler     on_chunk;
    StreamCompletion done;

    std::mutex               mutex;
    std::deque<QueryResult> ready;               // chunks read and waiting for the handler
    bool                     delivering = false;  // a worker task is handling chunks
    bool                     stop = false;        // the consumer is done (it said so, or threw) or the client is shutting down: chunks are dropped
    bool                     aborted = false;     // ... because of the shutdown: the stream did not end by itself
    std::exception_ptr       handler_error;       // what the handler threw
    bool                     finished = false;    // the statement's last result has been read
    std::exception_ptr       error;               // what the server, or the connection, reported
    bool                     completed = false;   // `done` has been called (or posted)
  };

  /** @brief A statement waiting to start: a plain one (`done`) or a stream (`stream`). */
  struct Operation {
    std::string              sql;
    std::vector<std::string> params;
    Completion               done;
    std::shared_ptr<Stream>  stream;
  };

  /** @brief A statement that has been sent and is waiting on its connection's socket. */
  struct Active {
    Operation          operation;
    PGconn*            connection = nullptr;
    bool               flushing = true;   // libpq still has bytes of the request to write
    pg_result*         result = nullptr;  // the last successful result, owned until handed to the completion
    std::exception_ptr error;
    // A stream's state, all of it used by the reactor thread only:
    bool                               paused = false;     // not reading: the consumer has fallen behind
    bool                               read_done = false;  // every result has been read; only a cancel still running holds the connection
    bool          cancel_sent = false;
    bool          query_canceled = false;  // the statement ended with a cancellation (SQLSTATE 57014)
    PGcancelConn* cancel = nullptr;        // a cancel being sent, driven by the reactor (owned); null when there is none
    std::int16_t  cancel_events = 0;       // what the cancel waits for on its own socket
  };

  /**
   * @brief A connection whose link was lost, being re-established by the reactor: first the reset, then the session's `SET`.
   * Until it is done, the connection is neither idle nor running a statement.
   */
  struct Reconnecting {
    PGconn*      connection = nullptr;
    bool         resetting = true;  // false: the reset is done and the session settings are being applied
    std::int16_t events = 0;        // what it waits for on the connection's socket
  };

  // ---- setup and teardown --------------------------------------------------

  void open_connections(const std::string& conninfo, std::size_t count) {
    for ( std::size_t i = 0; i < count; ++i ) {
      PGconn* connection = PQconnectdb(conninfo.c_str());
      if ( PQstatus(connection) != CONNECTION_OK ) {
        const std::string message = trimmed(PQerrorMessage(connection));
        PQfinish(connection);
        throw std::runtime_error("could not connect to Postgres: " + message);
      }
      _connections.push_back(connection);
      if ( const std::string message = apply_session(connection, _session_sql); ! message.empty() ) {
        throw std::runtime_error("could not apply the session settings to a Postgres connection: " + message);
      }
      if ( PQsetnonblocking(connection, 1) != 0 ) {
        throw std::runtime_error("could not put a Postgres connection in non-blocking mode");
      }
    }
  }

  void open_wake_pipe() {
    std::array<int, 2> ends{};
    if ( pipe2(ends.data(), O_NONBLOCK | O_CLOEXEC) != 0 ) {
      throw std::runtime_error("could not create the reactor's wake-up pipe");
    }
    _wake_read = ends.at(0);
    _wake_write = ends.at(1);
  }

  void close_everything() {
    for ( PGconn* connection : _connections ) {
      PQfinish(connection);
    }
    _connections.clear();
    for ( const int descriptor : {_wake_read, _wake_write} ) {
      if ( descriptor >= 0 ) {
        close(descriptor);
      }
    }
    _wake_read = -1;
    _wake_write = -1;
  }

  // ---- talking to the reactor ---------------------------------------------

  /** @brief Interrupts the reactor's `poll`, so it looks at the queue and the idle connections again. */
  void wake() const {
    const char byte = 1;
    // A full pipe (EAGAIN) means a wake-up is already pending, which is all this is for.
    [[maybe_unused]] const ssize_t written = write(_wake_write, &byte, 1);
  }

  void drain_wake() const {
    constexpr std::size_t         DRAIN_CHUNK = 64;
    std::array<char, DRAIN_CHUNK> sink{};
    while ( read(_wake_read, sink.data(), sink.size()) > 0 ) {
    }
  }

  void push_idle(PGconn* connection) {
    {
      const std::scoped_lock lock(_mutex);
      _idle.push_back(connection);
    }
    wake();
  }

  void complete_with_error(Completion done, const std::string& message) {
    _workers->post([done = std::move(done), message] mutable { done(QueryResult{}, runtime_error_ptr(message)); });
  }

  /** @brief Ends an operation that never started: its completion gets the error. */
  void reject(Operation operation, const std::string& message) {
    if ( operation.stream ) {
      _workers->post([stream = std::move(operation.stream), error = runtime_error_ptr(message)] { stream->done(error); });
      return;
    }
    complete_with_error(std::move(operation.done), message);
  }

  void enqueue(Operation operation) {
    {
      std::unique_lock lock(_mutex);
      if ( _stopping ) {
        lock.unlock();
        reject(std::move(operation), "the Postgres client is shutting down");
        return;
      }
      _queued.push_back(std::move(operation));
    }
    wake();
  }

  // ---- the reactor ---------------------------------------------------------

  /** @brief Where each socket the reactor waits on is in its `pollfd` list. */
  struct PollLayout {
    std::vector<std::size_t> cancel_at;          // per active statement: its cancel's index, or 0 for none
    std::size_t              reconnects_at = 0;  // the first reconnect's index
  };

  void reactor_loop() {
    std::vector<Active> active;
    std::vector<pollfd> descriptors;
    PollLayout          layout;
    while ( true ) {
      const bool stopping = start_queued(active);
      serve_streams(active, stopping);
      if ( stopping && active.empty() ) {
        return;
      }
      collect_descriptors(active, descriptors, layout);
      if ( poll(descriptors.data(), static_cast<nfds_t>(descriptors.size()), -1) < 0 ) {
        if ( errno != EINTR ) {
          fail_everything(active);
        }
        continue;
      }
      if ( descriptors.front().revents != 0 ) {
        drain_wake();
      }
      advance_reconnects(descriptors, layout);
      advance_statements(active, descriptors, layout);
    }
  }

  /** @brief The wake-up pipe, then each statement's socket, then the sockets of its cancel (if any), then those of reconnects. */
  void collect_descriptors(const std::vector<Active>& active, std::vector<pollfd>& descriptors, PollLayout& layout) const {
    descriptors.clear();
    layout.cancel_at.clear();
    descriptors.push_back(pollfd{.fd = _wake_read, .events = poll_events(POLLIN), .revents = 0});
    for ( const Active& statement : active ) {
      descriptors.push_back(pollfd{.fd = PQsocket(statement.connection), .events = interest_of(statement), .revents = 0});
    }
    for ( const Active& statement : active ) {
      layout.cancel_at.push_back(statement.cancel != nullptr ? descriptors.size() : 0);
      if ( statement.cancel != nullptr ) {
        descriptors.push_back(pollfd{.fd = PQcancelSocket(statement.cancel), .events = statement.cancel_events, .revents = 0});
      }
    }
    layout.reconnects_at = descriptors.size();
    for ( const Reconnecting& reconnect : _reconnecting ) {
      descriptors.push_back(pollfd{.fd = PQsocket(reconnect.connection), .events = reconnect.events, .revents = 0});
    }
  }

  /** @brief `poll` itself failed, so nothing more can be waited on: every statement fails, and reconnecting connections go back as they are. */
  void fail_everything(std::vector<Active>& active) {
    for ( Active& statement : active ) {
      statement.error = runtime_error_ptr("poll failed while waiting for Postgres");
      finish(statement);
    }
    active.clear();
    for ( const Reconnecting& reconnect : _reconnecting ) {
      push_idle(reconnect.connection);  // the next statement on it fails, and tries again
    }
    _reconnecting.clear();
  }

  void advance_reconnects(const std::vector<pollfd>& descriptors, const PollLayout& layout) {
    for ( std::size_t i = _reconnecting.size(); i-- > 0; ) {
      const std::int16_t events = descriptors.at(layout.reconnects_at + i).revents;
      if ( events != 0 && advance_reconnect(_reconnecting.at(i), events) ) {
        push_idle(_reconnecting.at(i).connection);
        _reconnecting.erase(_reconnecting.begin() + static_cast<std::ptrdiff_t>(i));
      }
    }
  }

  void advance_statements(std::vector<Active>& active, const std::vector<pollfd>& descriptors, const PollLayout& layout) {
    for ( std::size_t i = active.size(); i-- > 0; ) {
      Active&           statement = active.at(i);
      const std::size_t cancel_at = layout.cancel_at.at(i);
      bool              done = cancel_at != 0 && descriptors.at(cancel_at).revents != 0 && advance_cancel(statement);
      if ( const std::int16_t events = descriptors.at(i + 1).revents; ! done && events != 0 ) {
        done = advance(statement, events);
      }
      if ( done ) {
        finish(statement);
        active.erase(active.begin() + static_cast<std::ptrdiff_t>(i));
      }
    }
  }

  /** @return What to wait for on the statement's socket: to write its request, or to read, or (for a stream that is not reading) nothing. */
  static std::int16_t interest_of(const Active& statement) {
    if ( statement.flushing ) {
      return poll_events(POLLIN, POLLOUT);
    }
    if ( statement.paused || statement.read_done ) {
      return 0;  // a paused stream leaves the rows in the socket, so the server has to wait
    }
    return poll_events(POLLIN);
  }

  /** @brief Starts queued statements on idle connections. @return Whether the client is shutting down. */
  bool start_queued(std::vector<Active>& active) {
    std::unique_lock lock(_mutex);
    while ( ! _queued.empty() ) {
      if ( _stopping ) {
        Operation operation = std::move(_queued.front());
        _queued.pop_front();
        lock.unlock();
        reject(std::move(operation), "the Postgres client is shutting down");
        lock.lock();
        continue;
      }
      if ( _idle.empty() ) {
        break;
      }
      Operation operation = std::move(_queued.front());
      _queued.pop_front();
      PGconn* connection = _idle.back();
      _idle.pop_back();
      lock.unlock();
      begin(std::move(operation), connection, active);
      lock.lock();
    }
    return _stopping;
  }

  void begin(Operation operation, PGconn* connection, std::vector<Active>& active) {
    std::vector<const char*> values;
    values.reserve(operation.params.size());
    for ( const std::string& param : operation.params ) {
      values.push_back(param.c_str());
    }

    const int binary_results = operation.stream && operation.stream->options.binary ? 1 : 0;  // the format of every column of the result
    const int sent = PQsendQueryParams(connection, operation.sql.c_str(), static_cast<int>(values.size()), nullptr, values.data(), nullptr, nullptr,
                                       binary_results);
    Active    statement{.operation = std::move(operation), .connection = connection};
    // A stream asks for its results a chunk at a time; that has to be said after the statement is sent and before its first result is read.
    const bool chunked = sent != 0 && (! statement.operation.stream ||
                                       PQsetChunkedRowsMode(connection, static_cast<int>(statement.operation.stream->options.chunk_rows)) != 0);
    if ( sent == 0 || ! chunked ) {
      statement.error = runtime_error_ptr(trimmed(PQerrorMessage(connection)));
      finish(statement);
      return;
    }

    active.push_back(std::move(statement));
    if ( advance(active.back(), poll_events(POLLOUT)) ) {  // try to write the request at once; it may even fail here
      finish(active.back());
      active.pop_back();
    }
  }

  /** @brief Does what `events` on the connection's socket allow. @return True once the statement is finished. */
  bool advance(Active& statement, std::int16_t events) {
    PGconn* connection = statement.connection;
    if ( statement.read_done ) {
      return false;  // waiting only for its cancel to finish
    }

    if ( statement.flushing && has_any(events, static_cast<unsigned>(POLLOUT)) ) {
      const int flushed = PQflush(connection);
      if ( flushed < 0 ) {
        statement.error = runtime_error_ptr(trimmed(PQerrorMessage(connection)));
        return true;
      }
      statement.flushing = flushed > 0;
    }

    if ( has_any(events, READABLE) ) {
      if ( PQconsumeInput(connection) == 0 ) {
        statement.error = runtime_error_ptr(trimmed(PQerrorMessage(connection)));
        return true;
      }
      return read_results(statement);
    }
    return false;
  }

  /**
   * @brief Takes every result libpq has ready. A stream that is full stops here (`paused`) and leaves the rest: in libpq's
   * buffer, and in the socket's.
   * @return True once the statement is finished.
   */
  bool read_results(Active& statement) {
    PGconn* connection = statement.connection;
    while ( PQisBusy(connection) == 0 ) {
      if ( statement.operation.stream && is_full(*statement.operation.stream) ) {
        statement.paused = true;
        return false;
      }
      pg_result* result = PQgetResult(connection);
      if ( result == nullptr ) {
        statement.read_done = true;  // every result has been read: the statement is done, once a cancel it sent has been
        return ! is_cancelling(statement);
      }
      take(statement, result);
    }
    return false;
  }

  /** @brief Hands a chunk to the stream's handler; anything else is the statement's outcome. */
  void take(Active& statement, pg_result* result) {
    if ( statement.operation.stream && PQresultStatus(result) == PGRES_TUPLES_CHUNK ) {
      enqueue_chunk(statement.operation.stream, QueryResult(result));
      return;
    }
    record(statement, result);
  }

  static bool is_cancelling(const Active& statement) { return statement.cancel != nullptr; }

  static void record(Active& statement, pg_result* result) {
    if ( is_success(PQresultStatus(result)) ) {
      if ( statement.result != nullptr ) {
        PQclear(statement.result);
      }
      statement.result = result;
      return;
    }
    if ( ! statement.error ) {
      statement.error = query_error_ptr(result);
      const char* state = PQresultErrorField(result, PG_DIAG_SQLSTATE);
      statement.query_canceled = state != nullptr && std::string_view(state) == CANCELED_SQLSTATE;
    }
    PQclear(result);
  }

  /** @brief Returns the connection to the pool and hands the outcome to a worker. */
  void finish(Active& statement) {
    if ( statement.cancel != nullptr ) {  // only when poll failed: a finished statement normally waits for its cancel
      end_cancel(statement);
    }
    if ( statement.operation.stream ) {
      finish_stream(statement);
      return;
    }
    PGconn*            connection = statement.connection;
    QueryResult       result;
    std::exception_ptr error = statement.error;
    if ( ! error ) {
      result = QueryResult(statement.result);
    } else if ( statement.result != nullptr ) {
      PQclear(statement.result);
    }
    statement.result = nullptr;

    release(connection);
    _workers->post([done = std::move(statement.operation.done), result = std::move(result), error] mutable { done(std::move(result), error); });
  }

  // ---- streams -------------------------------------------------------------

  /** @return Whether the stream has as many chunks waiting as it may buffer (a stopped one never does: it drops them). */
  static bool is_full(Stream& stream) {
    const std::scoped_lock lock(stream.mutex);
    return ! stream.stop && stream.ready.size() >= stream.options.max_buffered_chunks;
  }

  /** @brief Queues a chunk for the handler, and starts the task that handles them if none is running. A stopped stream drops it. */
  void enqueue_chunk(const std::shared_ptr<Stream>& stream, QueryResult chunk) {
    {
      const std::scoped_lock lock(stream->mutex);
      if ( stream->stop ) {
        return;
      }
      stream->ready.push_back(std::move(chunk));
      if ( stream->delivering ) {
        return;
      }
      stream->delivering = true;
    }
    _workers->post([this, stream] { deliver(stream); });
  }

  /** @brief The worker task: handles the stream's chunks one at a time, in order, until none is waiting or the stream is stopped. */
  void deliver(const std::shared_ptr<Stream>& stream) {
    while ( true ) {
      QueryResult chunk;
      {
        const std::scoped_lock lock(stream->mutex);
        if ( stream->stop ) {
          stream->ready.clear();
        }
        if ( stream->ready.empty() ) {
          stream->delivering = false;
          break;
        }
        chunk = std::move(stream->ready.front());
        stream->ready.pop_front();
      }
      bool               more = true;
      std::exception_ptr failure;
      try {
        more = stream->on_chunk(std::move(chunk));
      } catch ( ... ) {
        failure = std::current_exception();
        more = false;
      }
      {
        const std::scoped_lock lock(stream->mutex);
        if ( failure ) {
          stream->handler_error = failure;
        }
        stream->stop = stream->stop || ! more;
      }
      wake();  // there is room for another chunk, or the stream is to be stopped: the reactor looks again
    }
    complete_if_done(stream);
    wake();
  }

  /** @brief Ends the stream, once, when every chunk that will be handled has been. */
  void complete_if_done(const std::shared_ptr<Stream>& stream) {
    std::exception_ptr error;
    {
      const std::scoped_lock lock(stream->mutex);
      if ( stream->completed || ! stream->finished || stream->delivering || (! stream->stop && ! stream->ready.empty()) ) {
        return;
      }
      stream->completed = true;
      stream->ready.clear();
      if ( stream->handler_error ) {
        error = stream->handler_error;
      } else if ( stream->aborted ) {
        error = runtime_error_ptr("the Postgres client is shutting down");
      } else {
        error = stream->error;
      }
    }
    _workers->post([stream, error] { stream->done(error); });
  }

  /** @brief A stream's statement has been read to its end: the connection goes back, and the stream ends once its chunks are handled. */
  void finish_stream(Active& statement) {
    const std::shared_ptr<Stream> stream = statement.operation.stream;
    if ( statement.result != nullptr ) {
      PQclear(statement.result);
      statement.result = nullptr;
    }
    release(statement.connection);
    {
      const std::scoped_lock lock(stream->mutex);
      stream->finished = true;
      // The error after our own cancel is the cancellation, which is what was asked for. Any other (a timeout is 57014 too) is an error.
      stream->error = statement.query_canceled && stream->stop ? nullptr : statement.error;
    }
    complete_if_done(stream);
  }

  /** @brief Tells every stream to stop that has been told to (or must, because the client is shutting down). */
  void serve_streams(std::vector<Active>& active, bool stopping) {
    for ( std::size_t i = active.size(); i-- > 0; ) {
      if ( active.at(i).operation.stream && serve_stream(active.at(i), stopping) ) {
        finish(active.at(i));
        active.erase(active.begin() + static_cast<std::ptrdiff_t>(i));
      }
    }
  }

  /**
   * @brief Stops the statement's stream if it has to stop, and reads on if it was waiting for the handler to catch up.
   * @return True once the statement is finished.
   */
  bool serve_stream(Active& statement, bool stopping) {
    Stream& stream = *statement.operation.stream;
    bool    stop = false;
    bool    room = false;
    {
      const std::scoped_lock lock(stream.mutex);
      if ( stopping && ! stream.stop ) {
        stream.stop = true;
        stream.aborted = true;
      }
      stop = stream.stop;
      room = stream.ready.size() < stream.options.max_buffered_chunks;
    }
    if ( stop && ! statement.read_done && ! statement.cancel_sent ) {
      cancel(statement);
    }
    if ( statement.read_done ) {
      return ! is_cancelling(statement);
    }
    if ( statement.paused && (room || stop) ) {
      statement.paused = false;
      return read_results(statement);  // libpq may hold the next results already, and then no socket event would say so
    }
    return false;
  }

  /**
   * @brief Starts cancelling the statement on the server, over a connection of the cancel's own that the reactor drives
   * (`advance_cancel`). The statement keeps its connection until the cancel is done, so a late cancel can never land on
   * the next statement that connection runs.
   */
  static void cancel(Active& statement) {
    statement.cancel_sent = true;
    PGcancelConn* request = PQcancelCreate(statement.connection);
    if ( request == nullptr ) {
      return;  // no memory for it: the statement runs to its end and its rows are dropped
    }
    if ( PQcancelStart(request) == 0 || PQcancelSocket(request) < 0 ) {
      PQcancelFinish(request);  // it could not even start: likewise
      return;
    }
    statement.cancel = request;
    statement.cancel_events = poll_events(POLLOUT);  // a cancel starts by connecting, as if the last poll said "writing"
  }

  /** @brief Does the next step of the statement's cancel. @return True once the statement is finished: its results were all read, and the cancel is over. */
  static bool advance_cancel(Active& statement) {
    switch ( PQcancelPoll(statement.cancel) ) {
      case PGRES_POLLING_READING:
        statement.cancel_events = poll_events(POLLIN);
        return false;
      case PGRES_POLLING_WRITING:
        statement.cancel_events = poll_events(POLLOUT);
        return false;
      case PGRES_POLLING_ACTIVE:
        return false;
      case PGRES_POLLING_OK:
      case PGRES_POLLING_FAILED:  // if it failed, the statement just runs on
        break;
    }
    end_cancel(statement);
    return statement.read_done;
  }

  static void end_cancel(Active& statement) {
    PQcancelFinish(statement.cancel);
    statement.cancel = nullptr;
  }

  /** @brief Makes `connection` available again, first re-establishing it (driven by the reactor) if the link was lost. */
  void release(PGconn* connection) {
    if ( PQstatus(connection) != CONNECTION_BAD ) {
      push_idle(connection);
      return;
    }
    if ( PQresetStart(connection) == 0 || PQsocket(connection) < 0 ) {
      push_idle(connection);  // the reset could not start: the next statement on it fails, and tries again
      return;
    }
    _reconnecting.push_back(Reconnecting{.connection = connection, .resetting = true, .events = poll_events(POLLOUT)});
  }

  /** @brief Does the next step of a reconnect. @return True once it is over (worked or not), and the connection can go back to the pool. */
  bool advance_reconnect(Reconnecting& reconnect, std::int16_t events) {
    PGconn* connection = reconnect.connection;
    if ( reconnect.resetting ) {
      switch ( PQresetPoll(connection) ) {
        case PGRES_POLLING_READING:
          reconnect.events = poll_events(POLLIN);
          return false;
        case PGRES_POLLING_WRITING:
          reconnect.events = poll_events(POLLOUT);
          return false;
        case PGRES_POLLING_ACTIVE:
          return false;
        case PGRES_POLLING_FAILED:
          return true;  // the next statement on it fails, and tries again
        case PGRES_POLLING_OK:
          break;
      }
      // A reset connection is a new session: apply the settings again, without blocking.
      PQsetnonblocking(connection, 1);
      if ( _session_sql.empty() || PQsendQuery(connection, _session_sql.c_str()) == 0 ) {
        return true;
      }
      reconnect.resetting = false;
      reconnect.events = poll_events(POLLIN, POLLOUT);
      return false;
    }

    if ( has_any(events, static_cast<unsigned>(POLLOUT)) ) {
      const int flushed = PQflush(connection);
      if ( flushed < 0 ) {
        return true;
      }
      if ( flushed == 0 ) {
        reconnect.events = poll_events(POLLIN);
      }
    }
    if ( has_any(events, READABLE) ) {
      if ( PQconsumeInput(connection) == 0 ) {
        return true;
      }
      while ( PQisBusy(connection) == 0 ) {
        pg_result* result = PQgetResult(connection);
        if ( result == nullptr ) {
          return true;
        }
        PQclear(result);  // if the SET failed, the session keeps the server's defaults
      }
    }
    return false;
  }

  std::string                         _session_sql;
  std::vector<Reconnecting>           _reconnecting;  // reactor thread only
  std::unique_ptr<async::ThreadPool> _workers;
  std::vector<PGconn*>                _connections;
  int                                 _wake_read = -1;
  int                                 _wake_write = -1;

  std::mutex            _mutex;  // guards the three below
  std::deque<Operation> _queued;
  std::vector<PGconn*>  _idle;
  bool                  _stopping = false;

  std::thread _reactor;
};

// ============================================================================
// AsyncClient
// ============================================================================

AsyncClient::AsyncClient(const std::string& conninfo, Options options) : _impl(std::make_unique<Impl>(conninfo, options)) {}

AsyncClient::~AsyncClient() = default;

void AsyncClient::run(std::string sql, std::vector<std::string> params, Completion done) {
  _impl->run(std::move(sql), std::move(params), std::move(done));
}

void AsyncClient::stream(std::string sql, std::vector<std::string> params, StreamOptions options, ChunkHandler on_chunk, StreamCompletion done) {
  _impl->stream(std::move(sql), std::move(params), options, std::move(on_chunk), std::move(done));
}

void AsyncClient::post(std::move_only_function<void()> task) { _impl->post(std::move(task)); }

}  // namespace schemacht::postgres
