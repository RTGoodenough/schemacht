#pragma once

#include <concepts>
#include <exception>
#include <future>
#include <memory>
#include <type_traits>
#include <utility>

#include "schemacht/util/concepts/futures.hpp"  // IWYU pragma: export

/**
 * Completions and the futures made from them: completing a `std::promise` from a completion that gets an error or a result,
 * which turns a callback into the future a caller waits on, and `complete`, which lets one call take either.
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
 * `future_from<std::uint64_t>([&](auto done) { runner.insert<Orders>(rows, std::move(done)); })`.
 * @throws std::bad_alloc if the future itself can't be made, before `start` is called.
 */
template <FutureValue value_t, std::invocable<Settler<value_t>> start_t>
[[nodiscard]] std::future<value_t> future_from(start_t start) {
  auto                 promise = std::make_shared<std::promise<value_t>>();
  std::future<value_t> future = promise->get_future();

  start(Settler<value_t>(std::move(promise)));

  return future;
}

/** @brief The completion that makes a call return a `std::future` of its outcome: what every call takes unless given another. */
struct UseFuture {
  constexpr UseFuture() noexcept = default;  // declared, so that `= {}` is a default, not an aggregate's braces
};

/** @brief The completion `complete` hands its `start`: a `Settler` for `UseFuture`, else the one given. */
template <typename done_t, FutureValue value_t>
using completion_t = std::conditional_t<std::same_as<done_t, UseFuture>, Settler<value_t>, done_t>;

/**
 * @return What a call that takes a completion `done` (`CompletionFor<value_t>`) returns: for `UseFuture`, the future of its
 * outcome; for any other completion, nothing. `start` begins the work and gets the completion to hand on: a `Settler` of that
 * future, or `done` itself. So each call is written once, as
 * `return util::complete<value_t>(std::move(done), [&](auto done) { ... });`.
 * @throws std::bad_alloc for `UseFuture`, if the future itself can't be made, before `start` is called.
 */
template <FutureValue value_t, CompletionFor<value_t> done_t, std::invocable<completion_t<done_t, value_t>> start_t>
auto complete(done_t done, start_t start) {
  if constexpr ( std::same_as<done_t, UseFuture> ) {
    return future_from<value_t>(std::move(start));
  } else {
    start(std::move(done));
  }
}

}  // namespace schemacht::util
