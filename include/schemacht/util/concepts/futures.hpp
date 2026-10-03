#pragma once

#include <concepts>
#include <type_traits>

namespace schemacht::util {

/** @brief Whether a `std::promise<value_t>` can be settled with a value moved in: what `settle` takes besides `void`. */
template <typename value_t>
concept PromiseValue = std::move_constructible<value_t> && ! std::is_reference_v<value_t>;

/** @brief Whether `future_from` can make a `std::future<value_t>`: a `PromiseValue`, or `void`. */
template <typename value_t>
concept FutureValue = PromiseValue<value_t> || std::is_void_v<value_t>;

/** @brief Whether a `value_t` is a value (not a reference) that can be moved in: what a `OnceCallback` holds. */
template <typename value_t>
concept MovableValue = std::move_constructible<value_t> && ! std::is_reference_v<value_t>;

}  // namespace schemacht::util
