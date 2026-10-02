#pragma once

#include <concepts>
#include <cstdint>
#include <exception>
#include <type_traits>
#include <vector>

namespace schemacht::postgres::detail {

/** @brief Whether `done_t` can finish an `execute_into` for `result_t`: called once with the rows and the error (null on success). */
template <typename done_t, typename result_t>
concept ExecuteCallback = std::invocable<done_t&, std::vector<result_t>, std::exception_ptr> &&
                          std::same_as<std::invoke_result_t<done_t&, std::vector<result_t>, std::exception_ptr>, void>;

/** @brief Whether `done_t` can finish a `stream_into`, a `commit_into` or a `rollback_into`: called once with the error (null on success). */
template <typename done_t>
concept StreamCallback = std::invocable<done_t&, std::exception_ptr> && std::same_as<std::invoke_result_t<done_t&, std::exception_ptr>, void>;

/** @brief Whether `done_t` can finish a command's `execute_into`: called once with the count of rows affected and the error (null on success). */
template <typename done_t>
concept CommandCallback = std::invocable<done_t&, std::uint64_t, std::exception_ptr> &&
                          std::same_as<std::invoke_result_t<done_t&, std::uint64_t, std::exception_ptr>, void>;

}  // namespace schemacht::postgres::detail
