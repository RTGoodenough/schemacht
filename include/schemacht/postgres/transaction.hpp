#pragma once

#include <concepts>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#include "schemacht/postgres/async_client.hpp"
#include "schemacht/postgres/statement_runner.hpp"

namespace schemacht::postgres {

/** @brief How much a transaction sees of other transactions' changes while it runs: PostgreSQL's isolation levels. */
enum class Isolation : std::uint8_t {
  ReadCommitted,   ///< Each statement sees what was committed before it started (PostgreSQL's default).
  RepeatableRead,  ///< Every statement sees what was committed before the transaction's first statement.
  Serializable,    ///< As if the transactions ran one after another; one that can't be fails with SQLSTATE `40001`, to be retried.
};

/** @brief How a transaction starts. */
struct TransactionOptions {
  Isolation isolation = Isolation::ReadCommitted;
  bool      read_only = false;  ///< Refuse every write (SQLSTATE `25006`).
};

namespace detail {

/** @return The error for a `COMMIT` that PostgreSQL answered with `ROLLBACK`: a statement in the transaction had failed. */
[[nodiscard]] inline QueryError rolled_back() { return {"the transaction was rolled back, not committed: a statement in it had failed", "25P02"}; }

/** @return The `BEGIN` that starts a transaction with `options`. */
[[nodiscard]] inline std::string begin_statement(const TransactionOptions& options) {
  std::string sql = "BEGIN ISOLATION LEVEL ";
  switch ( options.isolation ) {
    case Isolation::ReadCommitted:
      sql += "READ COMMITTED";
      break;
    case Isolation::RepeatableRead:
      sql += "REPEATABLE READ";
      break;
    case Isolation::Serializable:
      sql += "SERIALIZABLE";
      break;
  }

  if ( options.read_only ) {
    sql += " READ ONLY";
  }

  return sql;
}

}  // namespace detail

/**
 * @brief A transaction: statements on one connection of the pool's, which take effect together when it commits, or not at
 * all. Made by `Database::begin`.
 *
 * It runs what a `Database` runs (`execute`, `stream`, `insert`, `create_table`, ...), on its own connection, in the order the
 * calls are made; a call need not wait for the one before it. Then `commit` or `rollback`. One dropped without either is
 * rolled back. Once it has ended, every statement on it fails with `std::logic_error`.
 *
 * - **A statement the server rejects** fails the transaction: PostgreSQL refuses every later statement in it (SQLSTATE
 *   `25P02`) until it ends, and `commit` then reports that it rolled back (a `QueryError` with `25P02`).
 * - **An error before a statement is sent** (an argument its codec refuses, say) is that call's error alone: PostgreSQL never
 *   sees the statement, so the transaction stays open and a later `commit` commits the rest. Roll back if the rest should
 *   not stand without it.
 * - **It holds a connection** from `begin` until it ends, so a pool with as many transactions open as it has connections runs
 *   nothing else until one ends, and a `begin` waits.
 * - **A stream stopped early** is not cancelled on the server, which would fail the transaction: its other rows are read and dropped.
 * - **Not thread-safe:** use a transaction from one thread at a time (its completions may run on any worker). It must not
 *   outlive its `Database`.
 */
class Transaction : public StatementRunner {
 public:
  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;

  /** @brief Takes over `other`'s transaction; `other` is then ended, and refuses every statement. */
  Transaction(Transaction&& other) noexcept = default;

  /** @brief Rolls back this transaction if it is still open (as the destructor does), then takes over `other`'s. */
  Transaction& operator=(Transaction&& other) noexcept {
    if ( this != &other ) {
      abandon();
      StatementRunner::operator=(std::move(other));
    }

    return *this;
  }

  /** @brief Rolls back the transaction if it is still open, without waiting, and gives its connection back to the pool. */
  ~Transaction() { abandon(); }

  /** @return Whether the transaction has not ended: statements on it still run. */
  [[nodiscard]] bool is_open() const noexcept { return holds_lease(); }

  /**
   * @brief Commits the transaction and calls `done` once, on a worker thread, with the error (null if it committed). The
   * transaction has ended either way, and its connection goes back to the pool.
   *
   * The error is the `COMMIT`'s (a `QueryError`; under `Serializable`, `40001` means: run the transaction again), a `QueryError`
   * with `25P02` if a statement in it had failed and PostgreSQL rolled it back instead, or `std::logic_error` if it had already
   * ended.
   */
  template <detail::StreamCallback done_t>
  void commit_into(done_t done) {
    end(true, std::move(done));
  }

  /** @return A future that completes once the transaction is committed, or holds why it was not (see `commit_into`). */
  [[nodiscard]] std::future<void> commit() {
    const auto        promise = std::make_shared<std::promise<void>>();
    std::future<void> future = promise->get_future();

    commit_into([promise](const std::exception_ptr& error) { detail::settle(*promise, error); });

    return future;
  }

  /** @brief Rolls back the transaction and calls `done` once, on a worker thread, with the error (null on success). */
  template <detail::StreamCallback done_t>
  void rollback_into(done_t done) {
    end(false, std::move(done));
  }

  /** @return A future that completes once the transaction is rolled back, or holds the error (`std::logic_error` if it had already ended). */
  [[nodiscard]] std::future<void> rollback() {
    const auto        promise = std::make_shared<std::promise<void>>();
    std::future<void> future = promise->get_future();

    rollback_into([promise](const std::exception_ptr& error) { detail::settle(*promise, error); });

    return future;
  }

 private:
  friend class Database;

  /** @param lease The connection, with the transaction begun on it; empty for a transaction that never started (it refuses everything). */
  Transaction(AsyncClient& client, AsyncClient::Lease lease, const ReadOptions& read) : StatementRunner(client, std::move(lease), true, read) {}

  /** @brief Ends the transaction, committing it or rolling it back, and calls `done` with the outcome. */
  template <detail::StreamCallback done_t>
  void end(bool committing, done_t done) {
    // Let go of at the end of this call: the connection goes back once this is done. An empty lease (the transaction had
    // already ended) gets std::logic_error.
    const AsyncClient::Lease lease = take_lease();
    client().run(lease, committing ? "COMMIT" : "ROLLBACK", {},
                 [done = std::move(done), committing](const QueryResult& result, const std::exception_ptr& error) mutable {
                   if ( ! error && committing && result.command_tag() == "ROLLBACK" ) {
                     done(std::make_exception_ptr(detail::rolled_back()));
                   } else {
                     done(error);
                   }
                 });
  }

  /**
   * @brief Rolls back without waiting, if the transaction is open. Should the `ROLLBACK` not even be queued (no memory), the
   * connection still comes back inside the transaction, and the pool re-establishes it rather than use it as it is.
   */
  void abandon() noexcept {
    const AsyncClient::Lease lease = take_lease();
    if ( ! lease ) {
      return;
    }

    try {
      client().run(lease, "ROLLBACK", {}, [](const QueryResult& /*result*/, const std::exception_ptr& /*error*/) {});
    } catch ( ... ) {  // NOLINT(bugprone-empty-catch) -- see above: letting go of the lease is enough
    }
  }
};

namespace detail {

/** @brief Whether `done_t` can finish a `begin_into`: called once with the transaction and the error (null on success; on failure, the transaction is ended). */
template <typename done_t>
concept TransactionCallback =
    std::invocable<done_t&, Transaction, std::exception_ptr> && std::same_as<std::invoke_result_t<done_t&, Transaction, std::exception_ptr>, void>;

}  // namespace detail

}  // namespace schemacht::postgres
