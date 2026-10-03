// A call that takes a completion calls it exactly once, whichever allocation fails: each test fails one chosen allocation of
// a call (on the caller's thread, or on the one worker), for each allocation the call makes in turn, and checks that every
// completion ran once and nothing threw or hung. Its own executable: it replaces the global operator new.
//
// Run with SCHEMACHT_TEST_DB set, as the integration tests are; without it, every test here is skipped.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "schemacht/schemacht.hpp"
#include "support/integration.hpp"
#include "support/schemas.hpp"

namespace sch = schemacht::schema;
namespace q = schemacht::query;
namespace pg = schemacht::postgres;
namespace t = schemacht::test;

using namespace std::chrono_literals;

namespace {

thread_local std::int64_t fail_in = 0;  // allocations on this thread until one fails; 0: none does

}  // namespace

// NOLINTBEGIN(cppcoreguidelines-owning-memory,cppcoreguidelines-no-malloc) -- the allocation functions themselves
void* operator new(std::size_t size) {
  if ( fail_in > 0 ) {
    --fail_in;
    if ( fail_in == 0 ) {
      throw std::bad_alloc();
    }
  }

  if ( void* memory = std::malloc(size == 0 ? 1 : size) ) {
    return memory;
  }

  throw std::bad_alloc();
}

void* operator new[](std::size_t size) { return operator new(size); }
void  operator delete(void* memory) noexcept { std::free(memory); }
void  operator delete[](void* memory) noexcept { std::free(memory); }
void  operator delete(void* memory, std::size_t /*size*/) noexcept { std::free(memory); }
void  operator delete[](void* memory, std::size_t /*size*/) noexcept { std::free(memory); }
// NOLINTEND(cppcoreguidelines-owning-memory,cppcoreguidelines-no-malloc)

namespace {

using Lists = sch::Schema<"lists", t::Name, t::Tags>;  // an array column: many rows go in several statements

constexpr auto ALL_KEYLESS = q::select<t::Keyless>();
constexpr auto NO_KEYLESS = q::delete_all<t::Keyless>();

/** @brief Counts the calls of the completions made from it, and waits for the first. */
class Probe {
 public:
  /** @return A completion of any kind (it takes whatever it is called with). */
  [[nodiscard]] auto done() const {
    return [state = _state](auto&&... /*results*/) {
      if ( state->calls.fetch_add(1) == 0 ) {
        state->first.set_value();
      }
    };
  }

  /** @return Whether the completion was called, within a few seconds. */
  [[nodiscard]] bool called() const { return _first.wait_for(5s) == std::future_status::ready; }

  [[nodiscard]] int calls() const { return _state->calls.load(); }

 private:
  struct State {
    std::atomic<int>   calls = 0;
    std::promise<void> first;
  };

  std::shared_ptr<State> _state = std::make_shared<State>();
  std::future<void>      _first = _state->first.get_future();
};

/** @brief Runs `task` on the database's one worker, and waits for it. */
template <std::invocable task_t>
void on_worker(pg::Database& database, task_t task) {
  std::promise<void> ran;
  database.pool().post([&ran, &task] {
    task();
    ran.set_value();
  });
  ran.get_future().get();
}

/**
 * @brief Runs `call` with a `Probe` once for each allocation it makes on the caller's thread, failing that one (the first, then
 * the second, ...), and once with none failing. @return The probes, to check once the database has finished everything.
 */
template <std::invocable<const Probe&> call_t>
std::vector<Probe> sweep_caller(call_t call) {
  std::vector<Probe> probes;
  for ( std::int64_t failing = 1;; ++failing ) {
    const Probe& probe = probes.emplace_back();
    fail_in = failing;
    call(probe);
    const bool failed = fail_in == 0;
    fail_in = 0;

    REQUIRE(probe.called());
    if ( ! failed ) {
      return probes;  // the call made fewer allocations than that: every one has been failed
    }
  }
}

/** @brief As `sweep_caller`, for the allocations `call` makes on the database's one worker. */
template <std::invocable<const Probe&> call_t>
std::vector<Probe> sweep_worker(pg::Database& database, call_t call) {
  std::vector<Probe> probes;
  for ( std::int64_t failing = 1;; ++failing ) {
    const Probe& probe = probes.emplace_back();
    on_worker(database, [failing] { fail_in = failing; });
    call(probe);
    REQUIRE(probe.called());

    bool failed = false;
    on_worker(database, [&failed] {
      failed = fail_in == 0;
      fail_in = 0;
    });
    if ( ! failed ) {
      return probes;
    }
  }
}

/** @brief Checks that each probe's completion ran exactly once: after the database is gone, so nothing can still call one. */
void check_once(const std::vector<Probe>& probes) {
  for ( const Probe& probe : probes ) {
    CHECK(probe.calls() == 1);
  }
}

pg::Database::Options one_worker() {
  pg::Database::Options options;
  options.pool.workers = 1;
  options.pool.connections = 2;
  return options;
}

}  // namespace

TEST_CASE("integration: a failed allocation on the caller's thread reaches the completion, once", "[integration][fault]") {
  auto                  database = std::make_unique<pg::Database>(schemacht::test::test_db(), one_worker());
  pg::Table<t::Keyless> keyless(*database, sch::TableName("schemacht_it_fault_keyless"));
  keyless.drop().get();
  keyless.create().get();
  const std::vector<t::Keyless::row_type> rows{t::Keyless::make_row("a", 1.0)};

  std::vector<std::vector<Probe>> sweeps;
  sweeps.push_back(sweep_caller([&](const Probe& probe) { keyless.insert_into(rows, probe.done()); }));
  sweeps.push_back(sweep_caller([&](const Probe& probe) { database->execute_into(q::Prepared<ALL_KEYLESS>::bind(), probe.done()); }));
  sweeps.push_back(sweep_caller([&](const Probe& probe) { keyless.execute_into(q::Prepared<NO_KEYLESS>::bind(), probe.done()); }));
  sweeps.push_back(sweep_caller([&](const Probe& probe) {
    keyless.stream_into(q::Prepared<ALL_KEYLESS>::bind(), [](const t::Keyless::row_type& /*row*/) {}, probe.done());
  }));
  sweeps.push_back(sweep_caller([&](const Probe& probe) { database->begin_into(probe.done()); }));

  {
    pg::Transaction       transaction = database->begin().get();
    pg::Table<t::Keyless> in_transaction(transaction, keyless.name());
    sweeps.push_back(sweep_caller([&](const Probe& probe) { in_transaction.insert_into(rows, probe.done()); }));
  }

  // Several statements, in a transaction of their own: a failure part way through queuing them writes none.
  pg::Table<Lists> lists(*database, sch::TableName("schemacht_it_fault_lists"));
  lists.drop().get();
  lists.create().get();
  const std::vector<Lists::row_type> many(32768, Lists::make_row("a", std::vector<std::string>{"x"}));
  sweeps.push_back(sweep_caller([&](const Probe& probe) { lists.insert_into(many, probe.done()); }));

  keyless.drop().get();
  lists.drop().get();
  database.reset();  // everything it still had finishes now: a completion called again would show below

  for ( const std::vector<Probe>& probes : sweeps ) {
    check_once(probes);
  }
}

TEST_CASE("integration: a failed allocation on a worker reaches the completion, once", "[integration][fault]") {
  auto             database = std::make_unique<pg::Database>(schemacht::test::test_db(), one_worker());
  pg::Table<Lists> lists(*database, sch::TableName("schemacht_it_fault_worker_lists"));
  lists.drop().get();
  lists.create().get();
  const std::vector<Lists::row_type> many(32768, Lists::make_row("a", std::vector<std::string>{"x"}));

  std::vector<std::vector<Probe>> sweeps;
  sweeps.push_back(sweep_worker(*database, [&](const Probe& probe) { database->begin_into(probe.done()); }));     // the BEGIN's own step
  sweeps.push_back(sweep_worker(*database, [&](const Probe& probe) { lists.insert_into(many, probe.done()); }));  // queuing BEGIN ... COMMIT

  lists.drop().get();
  database.reset();

  for ( const std::vector<Probe>& probes : sweeps ) {
    check_once(probes);
  }
}
