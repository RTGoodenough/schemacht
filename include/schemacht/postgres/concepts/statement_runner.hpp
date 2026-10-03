#pragma once

#include <concepts>
#include <cstdint>
#include <exception>
#include <type_traits>
#include <vector>

/** The completions a call that takes one (`execute_into`, `insert_into`, ...) accepts. */
namespace schemacht::postgres::detail {

/** @brief Whether `done_t` can finish an `execute_into` for `result_t`: called once with the rows and the error (null on success). */
template <typename done_t, typename result_t>
concept ExecuteCallback = std::invocable<done_t&, std::vector<result_t>, std::exception_ptr> &&
                          std::same_as<std::invoke_result_t<done_t&, std::vector<result_t>, std::exception_ptr>, void>;

/**
 * @brief Whether `done_t` can finish a `stream_into`, an `insert_into`, a `commit_into` or a `rollback_into`: called once with
 * the error (null on success).
 */
template <typename done_t>
concept StreamCallback = std::invocable<done_t&, std::exception_ptr> && std::same_as<std::invoke_result_t<done_t&, std::exception_ptr>, void>;

/** @brief Whether `done_t` can finish a command's `execute_into`: called once with the count of rows affected and the error (null on success). */
template <typename done_t>
concept CommandCallback = std::invocable<done_t&, std::uint64_t, std::exception_ptr> &&
                          std::same_as<std::invoke_result_t<done_t&, std::uint64_t, std::exception_ptr>, void>;

/**
 * @brief Whether `fail_t` can finish a `done_t` whose call failed before its statement was sent (`StatementRunner::guarded`):
 * called with it and the error, it calls it with the result that goes with an error (no rows, a count of 0).
 */
template <typename fail_t, typename done_t>
concept FailureReport = std::invocable<const fail_t&, done_t&, std::exception_ptr>;

}  // namespace schemacht::postgres::detail
