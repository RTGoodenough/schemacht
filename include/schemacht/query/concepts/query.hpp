#pragma once

#include <cstddef>
#include <type_traits>

#include "schemacht/query/concepts/predicate.hpp"
#include "schemacht/schema/concepts/schema.hpp"

namespace schemacht::query {

template <schema::IsSchema schema_t, std::size_t predicate_capacity, std::size_t order_capacity>
struct Query;  // query/query.hpp

namespace detail {

template <typename data_t>
struct is_query : std::false_type {};

template <schema::IsSchema schema_t, std::size_t predicate_capacity, std::size_t order_capacity>
struct is_query<Query<schema_t, predicate_capacity, order_capacity>> : std::true_type {};

}  // namespace detail

/** @brief Detects whether `data_t` is a `Query<...>` specialization. */
template <typename data_t>
concept IsQuery = detail::is_query<data_t>::value;

/** @brief What `Prepared` accepts: a `Query`, or a bare `Predicate`. */
template <typename data_t>
concept IsQuerySource = IsQuery<data_t> || IsPredicate<data_t>;

}  // namespace schemacht::query
