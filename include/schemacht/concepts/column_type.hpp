#pragma once

#include <concepts>
#include <cstdint>
#include <type_traits>

#include "schemacht/postgres/concepts/field_codec.hpp"
#include "schemacht/util/concepts/type_utils.hpp"

namespace schemacht {

namespace detail {

template <auto member>
struct MemberOf {
  static constexpr bool IS_DATA_MEMBER = false;
};

template <typename class_t, typename value_t, value_t class_t::* member>
struct MemberOf<member> {
  static constexpr bool IS_DATA_MEMBER = true;
  using class_type = class_t;
  using value_type = value_t;
};

}  // namespace detail

/**
 * @brief Whether `member` (`&Wrapper::value`) is a data member whose type has a text form, of a class that is made from that one
 * value (`Wrapper{inner}`): a type that wraps a built-in one, as a strongly typed id does.
 */
template <auto member>
concept WrappableMember = detail::MemberOf<member>::IS_DATA_MEMBER && postgres::HasTextForm<typename detail::MemberOf<member>::value_type> &&
                          util::NonNarrowingTo<const typename detail::MemberOf<member>::value_type&, typename detail::MemberOf<member>::class_type>;

/** @brief Whether `enum_t` is an enumeration, whose values `EnumColumn` stores as labels. */
template <typename enum_t>
concept Enumeration = std::is_enum_v<enum_t>;

/**
 * @brief Whether `member` (`&Money::cents`) is an `std::int64_t` data member of a class made from that one value: an exact
 * amount held as a count of its smallest unit.
 */
template <auto member>
concept ScaledMember = detail::MemberOf<member>::IS_DATA_MEMBER && std::same_as<typename detail::MemberOf<member>::value_type, std::int64_t> &&
                       util::NonNarrowingTo<std::int64_t, typename detail::MemberOf<member>::class_type>;

/**
 * @brief Whether `to_stored` turns a `value_t` into a `stored_t`, and `from_stored` back, where `stored_t` is a type the database
 * knows (one with a text form).
 */
template <typename value_t, typename stored_t, auto to_stored, auto from_stored>
concept ConvertsThrough = postgres::HasTextForm<stored_t> && std::is_invocable_r_v<stored_t, decltype(to_stored), const value_t&> &&
                          std::is_invocable_r_v<value_t, decltype(from_stored), const stored_t&>;

}  // namespace schemacht
