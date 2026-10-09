#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "schemacht/util/move_only_function.hpp"

namespace schemacht::async {

/**
 * @brief A fixed set of worker threads running posted tasks.
 *
 * Tasks start in the order they were posted, and with more than one thread they run concurrently. Used to run completions
 * (parsing a result into rows, fulfilling a promise) so that waiting for the network never occupies one of these threads.
 *
 * @note Thread-safe. `shutdown` (and the destructor) runs every task posted, including tasks that running tasks post, then joins.
 */
class ThreadPool {
 public:
  /** @throws std::invalid_argument if `threads` is 0. */
  explicit ThreadPool(std::size_t threads);

  ~ThreadPool();

  /**
   * @brief Runs every task already posted, and any those tasks post, then joins the threads. Safe to call more than once,
   * and from no task of this pool. The pool stays usable as an object: a task posted afterwards is dropped.
   */
  void shutdown();

  /**
   * @brief Queues `task` to run on some worker.
   *
   * The task may capture move-only state (a promise, say). A task that throws is dropped: tasks report their own errors (through a promise, say), and a stray exception
   * must not take the worker down.
   */
  void post(util::MoveOnlyFunction<void()> task);

 private:
  std::mutex                                 _mutex;
  std::condition_variable                    _wake;
  std::deque<util::MoveOnlyFunction<void()>> _tasks;
  bool                                       _stopping = false;
  bool                                       _joined = false;
  std::vector<std::thread>                   _threads;

  void work();

 public:
  ThreadPool(const ThreadPool&) = delete;
  ThreadPool(ThreadPool&&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;
  ThreadPool& operator=(ThreadPool&&) = delete;
};

}  // namespace schemacht::async
