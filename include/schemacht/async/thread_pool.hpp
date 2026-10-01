#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace schemacht::async {

/**
 * @brief A fixed set of worker threads running posted tasks in the order they were posted.
 *
 * Used to run completions -- parsing a result into rows and fulfilling a
 * promise -- so that waiting for the network never occupies one of these
 * threads: only work that is ready to run does.
 *
 * @note Thread-safe. Destroying the pool runs every task already posted, then joins.
 */
class Thread_Pool {
 public:
  /** @throws std::invalid_argument if `threads` is 0. */
  explicit Thread_Pool(std::size_t threads);

  Thread_Pool(const Thread_Pool&) = delete;
  Thread_Pool(Thread_Pool&&) = delete;
  Thread_Pool& operator=(const Thread_Pool&) = delete;
  Thread_Pool& operator=(Thread_Pool&&) = delete;
  ~Thread_Pool();

  /**
   * @brief Queues `task` to run on some worker.
   *
   * The task may capture move-only state (a promise, say). A task that throws is dropped: tasks report their own errors (through a promise, say), and a stray exception
   * must not take the worker down.
   */
  void post(std::move_only_function<void()> task);

 private:
  void work();

  std::mutex                                  _mutex;
  std::condition_variable                     _wake;
  std::deque<std::move_only_function<void()>> _tasks;
  bool                                        _stopping = false;
  std::vector<std::thread>                    _threads;
};

}  // namespace schemacht::async
