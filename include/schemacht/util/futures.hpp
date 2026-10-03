#pragma once

#include <concepts>
#include <exception>
#include <future>
#include <type_traits>
#include <utility>

/**
 * Completing a `std::promise` from a completion that gets an error or a result: what turns a callback (`execute_into`,
 * `begin_into`, ...) into the future a caller waits on.
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
template <std::move_constructible value_t>
void settle(std::promise<value_t>& promise, const std::exception_ptr& error, std::type_identity_t<value_t> value) {
  if ( error ) {
    promise.set_exception(error);
  } else {
    promise.set_value(std::move(value));
  }
}

}  // namespace schemacht::util
