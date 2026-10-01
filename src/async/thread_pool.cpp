#include "schemacht/async/thread_pool.hpp"

#include <stdexcept>
#include <utility>

namespace schemacht::async {

Thread_Pool::Thread_Pool(std::size_t threads) {
  if ( threads == 0 ) {
    throw std::invalid_argument("Thread_Pool needs at least one thread");
  }
  _threads.reserve(threads);
  for ( std::size_t i = 0; i < threads; ++i ) {
    _threads.emplace_back([this] { work(); });
  }
}

Thread_Pool::~Thread_Pool() {
  {
    const std::scoped_lock lock(_mutex);
    _stopping = true;
  }
  _wake.notify_all();
  for ( std::thread& thread : _threads ) {
    thread.join();
  }
}

void Thread_Pool::post(std::move_only_function<void()> task) {
  {
    const std::scoped_lock lock(_mutex);
    _tasks.push_back(std::move(task));
  }
  _wake.notify_one();
}

void Thread_Pool::work() {
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
