// Whichever allocation fails, a call that takes a completion either throws std::bad_alloc and never calls it, or calls it
// exactly once: each test fails one chosen allocation of a call (on the caller's thread, or on the one worker), for each
// allocation the call makes in turn, and checks every completion once nothing more can run. Its own executable: it replaces
// the global operator new.
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
#include <optional>
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

using Lists = sch::Schema<"schemacht_it_fault_lists", t::Name, t::Tags>;               // an array column
using WorkerLists = sch::Schema<"schemacht_it_fault_worker_lists", t::Name, t::Tags>;  // the same, for the test on a worker (run alongside)

constexpr auto ALL_KEYLESS = q::select<t::Keyless>();
constexpr auto NO_KEYLESS = q::delete_all<t::Keyless>();
constexpr auto FEW_WORKER_LISTS = q::select<WorkerLists>().limit(3);

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

  /** @return Whether the completion ran as it should have: once, or never if the call threw. */
  [[nodiscard]] bool ran_as_due() const { return _state->calls.load() == (_threw ? 0 : 1); }

  /** @brief The call threw: its completion must never run. */
  void threw() { _threw = true; }

 private:
  struct State {
    std::atomic<int>   calls = 0;
    std::promise<void> first;
  };

  std::shared_ptr<State> _state = std::make_shared<State>();
  std::future<void>      _first = _state->first.get_future();
  bool                   _threw = false;
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
    Probe& probe = probes.emplace_back();
    fail_in = failing;
    bool threw = false;
    try {
      call(probe);
    } catch ( const std::bad_alloc& ) {
      threw = true;
    }
    const bool failed = fail_in == 0;
    fail_in = 0;

    if ( threw ) {
      probe.threw();
    } else {
      REQUIRE(probe.called());
    }
    if ( ! failed ) {
      return probes;  // the call made fewer allocations than that: every one has been failed
    }
  }
}

/**
 * @brief As `sweep_caller`, for a call on a transaction: each run gets a transaction of its own, begun before its allocations
 * are counted, and rolled back after.
 */
template <std::invocable<pg::Transaction&, const Probe&> call_t>
std::vector<Probe> sweep_transaction(pg::Database& database, call_t call) {
  std::vector<Probe> probes;
  for ( std::int64_t failing = 1;; ++failing ) {
    pg::Transaction transaction = database.begin().get();
    Probe&          probe = probes.emplace_back();
    fail_in = failing;
    bool threw = false;
    try {
      call(transaction, probe);
    } catch ( const std::bad_alloc& ) {
      threw = true;
    }
    const bool failed = fail_in == 0;
    fail_in = 0;

    if ( threw ) {
      probe.threw();
    } else {
      REQUIRE(probe.called());
    }

    if ( ! failed ) {
      return probes;
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

/** @brief Checks that each probe's completion ran as it should have: after the database is gone, so nothing can still call one. */
void check_once(const std::vector<Probe>& probes) {
  for ( const Probe& probe : probes ) {
    CHECK(probe.ran_as_due());
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
  auto                         database = std::make_unique<pg::Database>(schemacht::test::test_db(), one_worker());
  const sch::Table<t::Keyless> keyless("schemacht_it_fault_keyless");
  database->execute(pg::drop_table_statement<t::Keyless>().on(keyless)).get();
  database->execute(pg::create_table_statement<t::Keyless>().on(keyless)).get();
  const std::vector<t::Keyless::row_type> rows{t::Keyless::make_row("a", 1.0)};

  std::vector<std::vector<Probe>> sweeps;
  sweeps.push_back(sweep_caller([&](const Probe& probe) { database->execute(pg::insert_statement<t::Keyless>(rows).on(keyless), probe.done()); }));
  sweeps.push_back(sweep_caller([&](const Probe& probe) { database->execute(q::Prepared<ALL_KEYLESS>::bind(), probe.done()); }));
  sweeps.push_back(sweep_caller([&](const Probe& probe) { database->execute(q::Prepared<NO_KEYLESS>::bind().on(keyless), probe.done()); }));
  sweeps.push_back(sweep_caller([&](const Probe& probe) {
    database->stream(q::Prepared<ALL_KEYLESS>::bind().on(keyless), [](const t::Keyless::row_type& /*row*/) {}, probe.done());
  }));
  sweeps.push_back(sweep_caller([&](const Probe& probe) {
    database->stream_chunks(q::Prepared<ALL_KEYLESS>::bind().on(keyless), [](const std::vector<t::Keyless::row_type>& /*rows*/) {}, probe.done());
  }));
  sweeps.push_back(sweep_caller([&](const Probe& probe) { database->begin({}, probe.done()); }));

  // On a transaction: its statements, and ending it.
  sweeps.push_back(sweep_transaction(*database, [&](pg::Transaction& transaction, const Probe& probe) {
    transaction.execute(pg::insert_statement<t::Keyless>(rows).on(keyless), probe.done());
  }));
  sweeps.push_back(sweep_transaction(*database, [&](pg::Transaction& transaction, const Probe& probe) {
    transaction.execute(pg::insert_returning_statement<t::Keyless>(rows).on(keyless), probe.done());
  }));
  sweeps.push_back(sweep_transaction(*database, [&](pg::Transaction& transaction, const Probe& probe) {
    transaction.stream(q::Prepared<ALL_KEYLESS>::bind().on(keyless), [](const t::Keyless::row_type& /*row*/) {}, probe.done());
  }));
  sweeps.push_back(sweep_transaction(*database, [&](pg::Transaction& transaction, const Probe& probe) {
    transaction.stream_chunks(q::Prepared<ALL_KEYLESS>::bind().on(keyless), [](const std::vector<t::Keyless::row_type>& /*rows*/) {}, probe.done());
  }));
  sweeps.push_back(sweep_transaction(*database, [](pg::Transaction& transaction, const Probe& probe) { transaction.commit(probe.done()); }));
  sweeps.push_back(sweep_transaction(*database, [](pg::Transaction& transaction, const Probe& probe) { transaction.rollback(probe.done()); }));

  // An upsert, and a statement given as text with an argument.
  const sch::Table<t::People> people("schemacht_it_fault_people");
  database->execute(pg::drop_table_statement<t::People>().on(people)).get();
  database->execute(pg::create_table_statement<t::People>().on(people)).get();
  const std::vector<t::People::row_type> someone{t::People::make_row(1, "a", std::nullopt, 1.0, true)};
  const std::string                      raise = "UPDATE " + people.name().quoted() + " SET score = score + $1";
  sweeps.push_back(sweep_caller([&](const Probe& probe) { database->execute(pg::upsert_statement<t::People>(someone).on(people), probe.done()); }));
  sweeps.push_back(sweep_caller([&](const Probe& probe) { database->execute(pg::unchecked_sql(raise, 1.0), probe.done()); }));
  sweeps.push_back(sweep_transaction(*database, [&](pg::Transaction& transaction, const Probe& probe) {
    transaction.execute(pg::upsert_statement<t::People>(someone).on(people), probe.done());
  }));
  sweeps.push_back(sweep_transaction(*database, [&](pg::Transaction& transaction, const Probe& probe) {
    transaction.execute(pg::unchecked_sql(raise, 1.0), probe.done());
  }));

  // Many rows of a schema with an array column: one statement all the same.
  database->drop_table<Lists>().get();
  database->create_table<Lists>().get();
  const std::vector<Lists::row_type> many(32768, Lists::make_row("a", std::vector<std::string>{"x"}));
  sweeps.push_back(sweep_caller([&](const Probe& probe) { database->insert<Lists>(many, probe.done()); }));

  database->execute(pg::drop_table_statement<t::Keyless>().on(keyless)).get();
  database->execute(pg::drop_table_statement<t::People>().on(people)).get();
  database->drop_table<Lists>().get();
  database.reset();  // everything it still had finishes now: a completion called again would show below

  for ( const std::vector<Probe>& probes : sweeps ) {
    check_once(probes);
  }
}

TEST_CASE("integration: a failed allocation on a worker reaches the completion, once", "[integration][fault]") {
  auto database = std::make_unique<pg::Database>(schemacht::test::test_db(), one_worker());
  database->drop_table<WorkerLists>().get();
  database->create_table<WorkerLists>().get();
  const std::vector<WorkerLists::row_type> many(32768, WorkerLists::make_row("a", std::vector<std::string>{"x"}));

  std::vector<std::vector<Probe>> sweeps;
  sweeps.push_back(sweep_worker(*database, [&](const Probe& probe) { database->begin({}, probe.done()); }));  // the BEGIN's own step
  sweeps.push_back(sweep_worker(*database, [&](const Probe& probe) { database->insert<WorkerLists>(many, probe.done()); }));
  sweeps.push_back(sweep_worker(*database, [&](const Probe& probe) {  // reading a chunk's rows, on the worker
    database->stream_chunks(q::Prepared<FEW_WORKER_LISTS>::bind(), [](const std::vector<WorkerLists::row_type>& /*rows*/) {}, probe.done());
  }));

  database->drop_table<WorkerLists>().get();
  database.reset();

  for ( const std::vector<Probe>& probes : sweeps ) {
    check_once(probes);
  }
}
