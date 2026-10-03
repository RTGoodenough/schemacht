#pragma once

#include <concepts>
#include <type_traits>

namespace schemacht::util {

/** @brief Whether a `std::promise<value_t>` can be settled with a value moved in: what `settle` takes besides `void`. */
template <typename value_t>
concept PromiseValue = std::move_constructible<value_t> && ! std::is_reference_v<value_t>;

}  // namespace schemacht::util
