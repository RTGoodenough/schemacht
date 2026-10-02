#pragma once

#include <chrono>
#include <concepts>
#include <ratio>

namespace schemacht::postgres {

/**
 * @brief Whether a `std::chrono::duration` holds every microsecond exactly: an integer count of microseconds or of a whole
 * fraction of one (`std::chrono::microseconds`, `std::chrono::nanoseconds`, `system_clock::duration`).
 */
template <typename duration_t>
concept MicrosecondExact = std::same_as<duration_t, std::chrono::duration<typename duration_t::rep, typename duration_t::period>> &&
                           std::integral<typename duration_t::rep> && std::ratio_divide<std::micro, typename duration_t::period>::den == 1;

}  // namespace schemacht::postgres
