#pragma once

#include <atomic>
#include <concepts>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <type_traits>
#include <utility>

#include "schemacht/util/concepts/futures.hpp"  // IWYU pragma: export

/**
 * Completions and the futures made from them: completing a `std::promise` from a completion that gets an error or a result
 * (what turns a callback, `execute_into` or `begin_into`, into the future a caller waits on), and a completion that runs
 * exactly once however many paths may finish it.
 */
namespace schemacht::util {

/** @brief Completes `promise` with `error`, or with nothing if there is none. */
inline void settle(std::promise<void>& promise, const std::exception_ptr& error) {
  if ( error ) {
    promise.set_exception(error);
  } else {
    promise.set_value();
  }
}

/**
 * @brief Completes `promise` with `error`, or with `value` if there is none. `value_t` is the promise's, so `3` settles a
 * `std::promise<std::int64_t>`.
 */
template <PromiseValue value_t>
void settle(std::promise<value_t>& promise, const std::exception_ptr& error, std::type_identity_t<value_t> value) {
  if ( error ) {
    promise.set_exception(error);
  } else {
    promise.set_value(std::move(value));
  }
}

/** @brief The completion `future_from` hands out: called with the value and the error (null on success), it settles the future. */
template <FutureValue value_t>
class Settler {
 public:
  explicit Settler(std::shared_ptr<std::promise<value_t>> promise) noexcept : _promise(std::move(promise)) {}

  void operator()(value_t value, const std::exception_ptr& error) const { settle(*_promise, error, std::move(value)); }

 private:
  std::shared_ptr<std::promise<value_t>> _promise;
};

/** @brief As above, for a future of nothing: called with the error alone. */
template <>
class Settler<void> {
 public:
  explicit Settler(std::shared_ptr<std::promise<void>> promise) noexcept : _promise(std::move(promise)) {}

  void operator()(const std::exception_ptr& error) const { settle(*_promise, error); }

 private:
  std::shared_ptr<std::promise<void>> _promise;
};

/**
 * @return The future of a call that takes a completion instead: `start` gets a `Settler` and hands it to that call, as in
 * `future_from<void>([&](auto done) { runner.insert_into<Orders>(rows, std::move(done)); })`.
 * @throws std::bad_alloc if the future itself can't be made, before `start` is called.
 */
template <FutureValue value_t, std::invocable<Settler<value_t>> start_t>
[[nodiscard]] std::future<value_t> future_from(start_t start) {
  auto                 promise = std::make_shared<std::promise<value_t>>();
  std::future<value_t> future = promise->get_future();

  start(Settler<value_t>(std::move(promise)));

  return future;
}

/**
 * @brief A completion that runs only the first time it is called, on whichever path gets there first; later calls do
 * nothing. Copies share it.
 *
 * For a call that hands its completion on and must still report a failure through it should the handing on throw part way:
 * the call keeps a copy, and the completion still runs exactly once.
 */
template <MovableValue done_t>
class OnceCallback {
 public:
  /**
   * @throws std::bad_alloc, and then `done` is left as it was: it is moved only once there is room for it. Or what moving
   * `done` throws, which leaves it as that move did (as it was, for one that copies).
   */
  explicit OnceCallback(done_t&& done) : _state(std::make_shared<State>(std::move(done))) {}

  /** @brief Calls the completion with `args`, unless it has been called already. */
  template <typename... args_t>
    requires std::invocable<done_t&, args_t...>
  void operator()(args_t&&... args) const {
    if ( ! _state->called.exchange(true) ) {
      std::invoke(_state->done, std::forward<args_t>(args)...);
    }
  }

 private:
  struct State {
    explicit State(done_t&& completion) : done(std::move(completion)) {}

    std::atomic<bool> called = false;
    done_t            done;
  };

  std::shared_ptr<State> _state;
};

}  // namespace schemacht::util
