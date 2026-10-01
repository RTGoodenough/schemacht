#include "schemacht/postgres/async_client.hpp"

#include <fcntl.h>
#include <libpq-fe.h>
#include <poll.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
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
  return std::make_exception_ptr(Query_Error(trimmed(PQresultErrorMessage(result)), state != nullptr ? state : ""));
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
// Query_Result
// ============================================================================

void Query_Result::Deleter::operator()(pg_result* raw) const noexcept { PQclear(raw); }

std::size_t Query_Result::row_count() const noexcept { return _raw ? static_cast<std::size_t>(PQntuples(_raw.get())) : 0; }

std::size_t Query_Result::column_count() const noexcept { return _raw ? static_cast<std::size_t>(PQnfields(_raw.get())) : 0; }

std::string_view Query_Result::column_name(std::size_t column) const {
  if ( column >= column_count() ) {
    throw std::out_of_range("Query_Result::column_name: no such column");
  }
  return PQfname(_raw.get(), static_cast<int>(column));
}

std::optional<std::string_view> Query_Result::text(std::size_t row, std::size_t column) const {
  if ( row >= row_count() || column >= column_count() ) {
    throw std::out_of_range("Query_Result::text: no such field");
  }
  const int row_index = static_cast<int>(row);
  const int column_index = static_cast<int>(column);
  if ( PQgetisnull(_raw.get(), row_index, column_index) != 0 ) {
    return std::nullopt;
  }
  return std::string_view{PQgetvalue(_raw.get(), row_index, column_index),
                          static_cast<std::size_t>(PQgetlength(_raw.get(), row_index, column_index))};
}

bool Query_Result::is_binary() const noexcept { return _raw && PQnfields(_raw.get()) > 0 && PQfformat(_raw.get(), 0) == 1; }

std::optional<std::span<const std::byte>> Query_Result::binary(std::size_t row, std::size_t column) const {
  if ( ! is_binary() ) {
    throw std::logic_error("Query_Result::binary: the result is text");
  }
  if ( row >= row_count() || column >= column_count() ) {
    throw std::out_of_range("Query_Result::binary: no such field");
  }
  const int row_index = static_cast<int>(row);
  const int column_index = static_cast<int>(column);
  if ( PQgetisnull(_raw.get(), row_index, column_index) != 0 ) {
    return std::nullopt;
  }
  return std::span<const std::byte>{
      reinterpret_cast<const std::byte*>(
          PQgetvalue(_raw.get(), row_index, column_index)),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast) -- libpq hands out bytes as char*
      static_cast<std::size_t>(PQgetlength(_raw.get(), row_index, column_index))};
}

std::uint32_t Query_Result::column_type(std::size_t column) const {
  if ( column >= column_count() ) {
    throw std::out_of_range("Query_Result::column_type: no such column");
  }
  return PQftype(_raw.get(), static_cast<int>(column));
}

// ============================================================================
// Async_Client::Impl
// ============================================================================

class Async_Client::Impl {
 public:
  Impl(const std::string& conninfo, const Options& options) : _session_sql(options.session.sql()) {
    if ( PQlibVersion() < MIN_LIBPQ_VERSION ) {  // the library loaded at run time, which is not always the one built against
      throw std::runtime_error("Async_Client needs libpq " + std::to_string(MIN_LIBPQ_VERSION / LIBPQ_MAJOR_DIVISOR) +
                               " or newer at run time, found " + std::to_string(PQlibVersion() / LIBPQ_MAJOR_DIVISOR));
    }
    if ( options.connections == 0 ) {
      throw std::invalid_argument("Async_Client needs at least one connection");
    }
    _workers = std::make_unique<async::Thread_Pool>(options.workers);

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
    _workers.reset();  // runs every completion still posted, while the connections they may reset are still open
    close_everything();
  }

  void run(std::string sql, std::vector<std::string> params, Completion done) {
    enqueue(Operation{.sql = std::move(sql), .params = std::move(params), .done = std::move(done), .stream = nullptr});
  }

  void stream(std::string sql, std::vector<std::string> params, StreamOptions options, Chunk_Handler on_chunk, Stream_Completion done) {
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
    Stream(StreamOptions stream_options, Chunk_Handler chunk_handler, Stream_Completion completion)
        : options(stream_options), on_chunk(std::move(chunk_handler)), done(std::move(completion)) {}

    StreamOptions     options;
    Chunk_Handler     on_chunk;
    Stream_Completion done;

    std::mutex               mutex;
    std::deque<Query_Result> ready;               // chunks read and waiting for the handler
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
    bool                               cancel_sent = false;
    bool                               query_canceled = false;  // the statement ended with a cancellation (SQLSTATE 57014)
    std::shared_ptr<std::atomic<bool>> cancel_running;          // true while a worker is sending the cancel
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
    _workers->post([done = std::move(done), message] mutable { done(Query_Result{}, runtime_error_ptr(message)); });
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

  void reactor_loop() {
    std::vector<Active> active;
    std::vector<pollfd> descriptors;
    while ( true ) {
      const bool stopping = start_queued(active);
      serve_streams(active, stopping);
      if ( stopping && active.empty() ) {
        return;
      }

      descriptors.clear();
      descriptors.push_back(pollfd{.fd = _wake_read, .events = poll_events(POLLIN), .revents = 0});
      for ( const Active& statement : active ) {
        descriptors.push_back(pollfd{.fd = PQsocket(statement.connection), .events = interest_of(statement), .revents = 0});
      }

      if ( poll(descriptors.data(), static_cast<nfds_t>(descriptors.size()), -1) < 0 ) {
        if ( errno == EINTR ) {
          continue;
        }
        for ( Active& statement : active ) {  // poll itself failed: nothing more can be waited on
          statement.error = runtime_error_ptr("poll failed while waiting for Postgres");
          finish(statement);
        }
        active.clear();
        continue;
      }

      if ( descriptors.front().revents != 0 ) {
        drain_wake();
      }
      for ( std::size_t i = active.size(); i-- > 0; ) {
        const std::int16_t events = descriptors.at(i + 1).revents;
        if ( events != 0 && advance(active.at(i), events) ) {
          finish(active.at(i));
          active.erase(active.begin() + static_cast<std::ptrdiff_t>(i));
        }
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
      enqueue_chunk(statement.operation.stream, Query_Result(result));
      return;
    }
    record(statement, result);
  }

  static bool is_cancelling(const Active& statement) { return statement.cancel_running != nullptr && statement.cancel_running->load(); }

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
    if ( statement.operation.stream ) {
      finish_stream(statement);
      return;
    }
    PGconn*            connection = statement.connection;
    Query_Result       result;
    std::exception_ptr error = statement.error;
    if ( ! error ) {
      result = Query_Result(statement.result);
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
  void enqueue_chunk(const std::shared_ptr<Stream>& stream, Query_Result chunk) {
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
      Query_Result chunk;
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
   * @brief Cancels the statement on the server. The connection stays with the statement until that has been done, so a late
   * cancel can never land on the next statement that connection runs.
   */
  void cancel(Active& statement) {
    statement.cancel_sent = true;
    PGcancelConn* request = PQcancelCreate(statement.connection);  // made here: the reactor thread owns the connection
    if ( request == nullptr ) {
      return;  // no memory for it: the statement runs to its end and its rows are dropped
    }
    statement.cancel_running = std::make_shared<std::atomic<bool>>(true);
    _workers->post([this, request, running = statement.cancel_running] {
      std::ignore = PQcancelBlocking(request);  // it opens a connection of its own; if it fails the statement just runs on
      PQcancelFinish(request);
      running->store(false);
      wake();
    });
  }

  /** @brief Makes `connection` available again, first reconnecting it (off the reactor thread) if the link was lost. */
  void release(PGconn* connection) {
    if ( PQstatus(connection) != CONNECTION_BAD ) {
      push_idle(connection);
      return;
    }
    _workers->post([this, connection] {
      PQreset(connection);
      std::ignore = apply_session(connection, _session_sql);  // a reset connection is a new session; a failure here is the next statement's to find
      PQsetnonblocking(connection, 1);
      push_idle(connection);  // even if the reset failed: the next statement will fail and try again
    });
  }

  std::string                         _session_sql;
  std::unique_ptr<async::Thread_Pool> _workers;
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
// Async_Client
// ============================================================================

Async_Client::Async_Client(const std::string& conninfo, Options options) : _impl(std::make_unique<Impl>(conninfo, options)) {}

Async_Client::~Async_Client() = default;

void Async_Client::run(std::string sql, std::vector<std::string> params, Completion done) {
  _impl->run(std::move(sql), std::move(params), std::move(done));
}

void Async_Client::stream(std::string sql, std::vector<std::string> params, StreamOptions options, Chunk_Handler on_chunk, Stream_Completion done) {
  _impl->stream(std::move(sql), std::move(params), options, std::move(on_chunk), std::move(done));
}

void Async_Client::post(std::move_only_function<void()> task) { _impl->post(std::move(task)); }

}  // namespace schemacht::postgres
