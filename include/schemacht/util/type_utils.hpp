#pragma once

#include <cstddef>
#include <type_traits>

namespace schemacht::util {

namespace detail {
template <typename data_t, template <typename...> class template_t>
struct IsSpecializationOf : std::false_type {};

template <template <typename...> class template_t, typename... arg_ts>
struct IsSpecializationOf<template_t<arg_ts...>, template_t> : std::true_type {};
}  // namespace detail

/**
 * @brief Detects whether `data_t` is a specialization of `template_t`.
 *
 * Only matches templates shaped `template<typename...> class` -- doesn't
 * work for templates with non-type parameters
 */
template <typename data_t, template <typename...> class template_t>
concept SpecializationOf = detail::IsSpecializationOf<data_t, template_t>::value;

namespace detail {
/** @brief Counts the number of the type data_t in a variadic pack */
template <typename data_t, typename... data_ts>
constexpr size_t COUNT_OCCURRENCES = (std::is_same_v<data_t, data_ts> + ... + 0);
}  // namespace detail

/** @brief True if every type in `data_ts` appears exactly once. */
template <typename... data_ts>
concept UniqueTypes = ((detail::COUNT_OCCURRENCES<data_ts, data_ts...> == 1) && ...);

}  // namespace schemacht::util
