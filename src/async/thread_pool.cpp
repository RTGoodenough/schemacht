#include "schemacht/async/thread_pool.hpp"

#include <cstddef>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace schemacht::async {

ThreadPool::ThreadPool(std::size_t threads) {
  if ( threads == 0 ) {
    throw std::invalid_argument("ThreadPool needs at least one thread");
  }
  _threads.reserve(threads);
  for ( std::size_t i = 0; i < threads; ++i ) {
    _threads.emplace_back([this] { work(); });
  }
}

ThreadPool::~ThreadPool() { shutdown(); }

void ThreadPool::shutdown() {
  {
    const std::scoped_lock lock(_mutex);
    if ( _stopping ) {
      return;
    }
    _stopping = true;
  }
  _wake.notify_all();
  for ( std::thread& thread : _threads ) {
    thread.join();
  }
  const std::scoped_lock lock(_mutex);
  _joined = true;
  _tasks.clear();  // posted after the last worker left: nothing will run them
}

void ThreadPool::post(std::move_only_function<void()> task) {
  {
    const std::scoped_lock lock(_mutex);
    if ( _joined ) {
      return;
    }
    _tasks.push_back(std::move(task));
  }
  _wake.notify_one();
}

void ThreadPool::work() {
  while ( true ) {
    std::move_only_function<void()> task;
    {
      std::unique_lock lock(_mutex);
      _wake.wait(lock, [this] { return _stopping || ! _tasks.empty(); });
      if ( _tasks.empty() ) {
        return;  // stopping, and nothing left to run
      }
      task = std::move(_tasks.front());
      _tasks.pop_front();
    }
    try {
      task();
    } catch ( ... ) {  // NOLINT(bugprone-empty-catch) -- see `post`: tasks report their own errors
    }
  }
}

}  // namespace schemacht::async
