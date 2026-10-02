#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <functional>
#include <stdexcept>

#include "schemacht/async/thread_pool.hpp"

using schemacht::async::ThreadPool;

TEST_CASE("shutdown runs tasks that running tasks post", "[thread_pool]") {
  std::atomic<int> ran{0};
  ThreadPool       pool(2);
  // Each task posts the next: the chain only finishes if `pool` stays usable while it shuts down.
  std::function<void(int)> chain = [&](int left) {
    ++ran;
    if ( left > 0 ) {
      pool.post([&chain, left] { chain(left - 1); });
    }
  };
  pool.post([&chain] { chain(99); });
  pool.shutdown();
  CHECK(ran == 100);
}

TEST_CASE("shutdown is idempotent, and a task posted afterwards is dropped", "[thread_pool]") {
  std::atomic<int> ran{0};
  ThreadPool       pool(1);
  pool.post([&ran] { ++ran; });
  pool.shutdown();
  pool.shutdown();
  pool.post([&ran] { ++ran; });
  CHECK(ran == 1);
}

TEST_CASE("a task that throws does not stop the pool", "[thread_pool]") {
  std::atomic<int> ran{0};
  {
    ThreadPool pool(1);
    pool.post([] { throw std::runtime_error("dropped by the pool"); });
    pool.post([&ran] { ++ran; });
  }
  CHECK(ran == 1);
}
