#pragma once

#include <exception>
#include <future>
#include <memory>
#include <string>
#include <utility>

#include "schemacht/postgres/async_client.hpp"
#include "schemacht/postgres/statement_runner.hpp"
#include "schemacht/postgres/transaction.hpp"

/**
 * A Postgres database: a pool of connections (an `AsyncClient`) that runs typed queries, raw statements and inserts for
 * any schema, from any thread.
 *
 * - **Rows:** `execute` a query (`Prepared::bind`) or a raw statement for every row, or `stream` them as they arrive.
 * - **Changes:** `execute` an update or delete (command.hpp), or a raw statement with no columns (an `UPDATE`, a `CREATE TYPE`),
 *   for the count of rows it changed, or the rows themselves with `returning(...)`; `insert` rows of a schema, or
 *   `insert_returning` them to get back what the database filled in (an identity key, say).
 * - **Tables:** `create_table` and `drop_table` from a schema, for tests, examples and a first start.
 * - **Transactions:** `begin` one, run the same calls on it, then `commit` or `rollback` (transaction.hpp).
 * - **Notices** the server sends are dropped, unless `Options::pool.on_notice` takes them.
 *
 * Every call returns a `std::future` at once and the work finishes in the background (call `get()` to wait). Outside a
 * transaction, each call is one statement in autocommit mode.
 *
 * @note Errors, including a statement that can't be started, arrive through the future, never as a throw from the call itself.
 *
 * @warning Row handlers and completions run on the pool's worker threads. They must not block on another of this database's
 * futures (`get()`), which could leave no worker free to complete it; chain with `execute_into` / `stream_into` / `begin_into` instead.
 */
namespace schemacht::postgres {

namespace detail {

/** @brief Owns the pool, so that it is made before the `StatementRunner` that sends to it: a base is made before the next base. */
class ClientHolder {
 protected:
  ClientHolder(const std::string& conninfo, const AsyncClient::Options& options) : _client(conninfo, options) {}

  [[nodiscard]] AsyncClient& held_client() noexcept { return _client; }

 private:
  AsyncClient _client;
};

}  // namespace detail

/**
 * @brief Postgres database with a pool of connections. Everything it runs is described on `StatementRunner`, which it
 * shares with `Transaction`.
 */
// NOLINTNEXTLINE(misc-multiple-inheritance) -- composition: ClientHolder only owns the pool (the base-from-member idiom)
class Database : private detail::ClientHolder, public StatementRunner {
 public:
  struct Options {
    AsyncClient::Options pool;  ///< The connections, the threads that run completions, and the session settings.
    ReadOptions          read;  ///< How results are read, unless a call says otherwise.
  };

  /**
   * @brief Opens the pool: `options.pool.connections` connections to the database, and the reactor and worker threads.
   * @throws std::runtime_error if a connection can't be made or the session settings can't be applied.
   * @throws std::invalid_argument if `options` asks for no connections, no workers, or an empty chunk.
   */
  Database(const std::string& conninfo, const Options& options)
      : ClientHolder(conninfo, options.pool), StatementRunner(held_client(), {}, false, options.read) {}
  explicit Database(const std::string& conninfo) : Database(conninfo, Options{}) {}

  /** @return The pool, for what only it does: a statement given as text (`AsyncClient::run`), or a task on its worker threads. */
  [[nodiscard]] AsyncClient& pool() noexcept { return held_client(); }

  /**
   * @brief Begins a transaction in the background and calls `done` exactly once, on a worker thread, with it and the error
   * (null on success; on failure the transaction it gets is ended). The primitive `begin` is built on, for chaining.
   *
   * The transaction takes a connection of the pool's until it ends, so it waits for one if none is free.
   */
  template <detail::TransactionCallback done_t>
  void begin_into(done_t done, const TransactionOptions& options = {}) {
    held_client().lease([client = &held_client(), read = read_options(), sql = detail::begin_statement(options),
                         done = std::move(done)](AsyncClient::Lease lease, const std::exception_ptr& error) mutable {
      if ( error ) {
        done(Transaction(*client, {}, read), error);
        return;
      }

      // The lease goes with the BEGIN's completion: to the transaction, or (if the BEGIN failed) back to the pool.
      const auto held = std::make_shared<AsyncClient::Lease>(std::move(lease));
      client->run(
          *held, std::move(sql), {},
          [client, held, read, done = std::move(done)](const QueryResult& /*began*/, const std::exception_ptr& began) mutable {
            done(Transaction(*client, began ? AsyncClient::Lease() : std::move(*held), read), began);
          }
      );
    });
  }

  /**
   * @return A future for a new transaction (`BEGIN`, with `options`), or holding the error. It must not outlive this database.
   *
   * @code
   * pg::Transaction transfer = database.begin().get();
   * transfer.execute(q::Prepared<WITHDRAW>::bind(from, amount)).get();
   * transfer.execute(q::Prepared<DEPOSIT>::bind(to, amount)).get();
   * transfer.commit().get();  // or nothing of it happened
   * @endcode
   */
  [[nodiscard]] std::future<Transaction> begin(const TransactionOptions& options = {}) {
    const auto               promise = std::make_shared<std::promise<Transaction>>();
    std::future<Transaction> future = promise->get_future();

    begin_into(
        [promise](Transaction transaction, const std::exception_ptr& error) {
          if ( error ) {
            promise->set_exception(error);
          } else {
            promise->set_value(std::move(transaction));
          }
        },
        options
    );

    return future;
  }

  Database(const Database&) = delete;  // it owns the pool: one per database
  Database(Database&&) = delete;
  Database& operator=(const Database&) = delete;
  Database& operator=(Database&&) = delete;
  ~Database() = default;
};

}  // namespace schemacht::postgres
