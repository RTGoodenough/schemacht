#pragma once

#include <concepts>
#include <exception>
#include <type_traits>

namespace schemacht::util {

/** @brief Whether a `std::promise<value_t>` can be settled with a value moved in: what `settle` takes besides `void`. */
template <typename value_t>
concept PromiseValue = std::move_constructible<value_t> && ! std::is_reference_v<value_t>;

/** @brief Whether `future_from` can make a `std::future<value_t>`: a `PromiseValue`, or `void`. */
template <typename value_t>
concept FutureValue = PromiseValue<value_t> || std::is_void_v<value_t>;

struct UseFuture;  // util/futures.hpp

namespace detail {

template <typename done_t, typename value_t>
inline constexpr bool CALLABLE_WITH_OUTCOME = std::invocable<done_t&, value_t, std::exception_ptr>;

template <typename done_t>
inline constexpr bool CALLABLE_WITH_OUTCOME<done_t, void> = std::invocable<done_t&, std::exception_ptr>;

}  // namespace detail

/**
 * @brief Whether `done_t` can finish a call whose outcome is a `value_t`: `UseFuture` (the call then returns a
 * `std::future<value_t>`), or a completion called once with the value and the error (null on success); with the error alone
 * for `void`.
 */
template <typename done_t, typename value_t>
concept CompletionFor = std::same_as<done_t, UseFuture> || (std::move_constructible<done_t> && detail::CALLABLE_WITH_OUTCOME<done_t, value_t>);

}  // namespace schemacht::util
