#pragma once

#include <concepts>
#include <ranges>

namespace schemacht::util {

/**
 * @brief Whether a `const range_t` can be walked more than once and its values compared with `==`: what `all_distinct` checks.
 * A view that is walked only when not `const`, such as a `std::views::filter`, is not one.
 */
template <typename range_t>
concept EqualityComparableRange = std::ranges::forward_range<const range_t> && std::equality_comparable<std::ranges::range_value_t<const range_t>>;

}  // namespace schemacht::util
