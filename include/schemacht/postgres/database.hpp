#pragma once

#include <exception>
#include <memory>
#include <string>
#include <utility>

#include "schemacht/postgres/async_client.hpp"
#include "schemacht/postgres/statement_runner.hpp"
#include "schemacht/postgres/transaction.hpp"
#include "schemacht/util/futures.hpp"

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
 * @brief A Postgres database: a pool of connections (an `AsyncClient`) that runs typed queries, raw statements and inserts for
 * any schema, from any thread.
 *
 * - **Rows:** `execute` a query (`Prepared::bind`) or a raw statement for every row, or `stream` them as they arrive, one at a
 *   time (or a chunk at a time, with `stream_chunks`).
 * - **Changes:** `execute` an update or delete (command.hpp), or a raw statement with no columns (an `UPDATE`, a `CREATE TYPE`),
 *   for the count of rows it changed, or the rows themselves with `returning(...)`; `insert` rows of a schema, or
 *   `insert_returning` them to get back what the database filled in (an identity key, say).
 * - **Tables:** `create_table` and `drop_table` from a schema, for tests, examples and a first start; `table_differences` to
 *   check, when a program starts, that a table is what its schema says.
 * - **Transactions:** `begin` one, run the same calls on it, then `commit` or `rollback` (transaction.hpp).
 * - **Notices** the server sends are dropped, unless `Options::pool.on_notice` takes them.
 *
 * Every call returns at once and the work finishes in the background: a `std::future` (call `get()` to wait), or, given a
 * completion last, nothing (see `StatementRunner`). Outside a transaction, each call is one statement in autocommit mode.
 *
 * @note Errors arrive through the future or the completion, exactly once; a call throws only if it can't start at all (see
 * `StatementRunner`).
 *
 * @warning Row handlers and completions run on the pool's worker threads. They must not block on another of this database's
 * futures (`get()`), which could leave no worker free to complete it: chain the next call with a completion instead.
 *
 * Everything it runs is described on `StatementRunner`, which it shares with `Transaction`.
 */
// NOLINTNEXTLINE(misc-multiple-inheritance) -- composition: ClientHolder only owns the pool (the base-from-member idiom)
class Database : private detail::ClientHolder, public StatementRunner {
 public:
  /** @brief How the database is set up: its pool, and how it reads results. */
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
   * @brief Begins a transaction (`BEGIN`, with `options`). It takes a connection of the pool's until it ends, so it waits for
   * one if none is free; it must not outlive this database. examples/transfers.cpp makes transfers in transactions.
   * @param options The isolation level, and whether it is read-only.
   * @param done none given (`util::UseFuture`): returns a `std::future` of the transaction. Else called with it and the error (on
   *        failure, the transaction it gets has ended: it refuses every statement).
   */
  template <util::CompletionFor<Transaction> done_t = util::UseFuture>
  auto begin(const TransactionOptions& options = TransactionOptions(), done_t done = {}) {
    return util::complete<Transaction>(std::move(done), [&](auto completion) {
      // Shared here, on the caller's thread: a worker that can't queue the BEGIN still holds the completion, to say so.
      const auto shared = std::make_shared<decltype(completion)>(std::move(completion));
      held_client().lease([client = &held_client(), read = read_options(), sql = detail::begin_statement(options),
                           shared](AsyncClient::Lease lease, std::exception_ptr error) mutable {
        if ( ! error ) {
          try {
            // The lease goes with the BEGIN's completion: to the transaction, or (if the BEGIN failed) back to the pool.
            const auto held = std::make_shared<AsyncClient::Lease>(std::move(lease));
            client->run(*held, std::move(sql), {}, [client, held, read, shared](const QueryResult& /*began*/, const std::exception_ptr& began) {
              (*shared)(Transaction(*client, began ? AsyncClient::Lease() : std::move(*held), read), began);
            });
            return;
          } catch ( ... ) {  // no memory to send the BEGIN: the lease goes back unused
            error = std::current_exception();
          }
        }

        (*shared)(Transaction(*client, {}, read), error);  // ended: it refuses every statement
      });
    });
  }

  /** @brief As above, with the default options: `db.begin(done)`. */
  template <util::CompletionFor<Transaction> done_t>
  auto begin(done_t done) {
    return begin(TransactionOptions(), std::move(done));
  }

  Database(const Database&) = delete;  // it owns the pool: one per database
  Database(Database&&) = delete;
  Database& operator=(const Database&) = delete;
  Database& operator=(Database&&) = delete;
  ~Database() = default;
};

}  // namespace schemacht::postgres
