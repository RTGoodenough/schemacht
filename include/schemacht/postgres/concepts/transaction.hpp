#pragma once

#include <concepts>
#include <exception>
#include <type_traits>

namespace schemacht::postgres {

class Transaction;  // postgres/transaction.hpp

namespace detail {

/** @brief Whether `done_t` can finish a `begin_into`: called once with the transaction and the error (null on success; on failure, the transaction is ended). */
template <typename done_t>
concept TransactionCallback =
    std::invocable<done_t&, Transaction, std::exception_ptr> && std::same_as<std::invoke_result_t<done_t&, Transaction, std::exception_ptr>, void>;

}  // namespace detail

}  // namespace schemacht::postgres
