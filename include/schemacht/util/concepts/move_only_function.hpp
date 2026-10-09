#pragma once

#include <type_traits>

namespace schemacht::util {

/**
 * @brief Whether a `callable_t` can be called as `result_t(argument_ts...)`: what `MoveOnlyFunction<result_t(argument_ts...)>`
 * is made from. It is called as an lvalue, as the function holds it, and its result must convert to `result_t`.
 */
template <typename callable_t, typename result_t, typename... argument_ts>
concept CallableAs = std::is_invocable_r_v<result_t, std::decay_t<callable_t>&, argument_ts...>;

}  // namespace schemacht::util
