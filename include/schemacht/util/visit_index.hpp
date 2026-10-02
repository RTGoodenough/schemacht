#pragma once

#include <cstddef>
#include <stdexcept>

namespace schemacht::util {

namespace detail {

template <std::size_t count, std::size_t idx, typename visitor_t>
constexpr decltype(auto) visit_index_from(std::size_t index, visitor_t& visitor) {
  if constexpr ( idx + 1 == count ) {
    if ( index != idx ) {
      throw std::out_of_range("visit_index: index out of range");
    }
    return visitor.template operator()<idx>();
  } else {
    if ( index == idx ) {
      return visitor.template operator()<idx>();
    }
    return visit_index_from<count, idx + 1>(index, visitor);
  }
}

}  // namespace detail

/**
 * @brief Turns a run-time index into a compile-time one.
 *
 * Calls `visitor.template operator()<i>()` for the `i` equal to `index`, where `i` is a constant expression inside the
 * call: so the visitor can name the `i`-th type, `std::get<i>` of a tuple, and so on, even though `index` is only
 * known at run time.
 *
 * @code
 * util::visit_index<std::tuple_size_v<row_t>>(column, [&]<std::size_t idx> {
 *   using field_t = std::tuple_element_t<idx, row_t>;
 *   ... std::get<idx>(row) ...
 * });
 * @endcode
 *
 * Every index must give the same result type (which may be `void`, or a reference), and that is the result. The
 * cases are a chain of comparisons, which compilers turn into a `switch`.
 *
 * Cost, measured once on in-memory paths (100k rows, five columns of four types; best of seven): compared with a
 * hand-written table of function pointers per call site, this was about 5-8% slower, roughly one nanosecond per
 * column operation. The compiler keeps the larger case bodies out of line either way; the extra cost is passing the
 * visitor's captures through a closure instead of registers. Paths dominated by IO or parsing, like a sink parsing a
 * result, will not notice. Where the column is known at compile time, no dispatch is needed at all.
 *
 * @tparam count How many indices there are, `0 <= index < count`.
 * @throws std::out_of_range if `index >= count`.
 */
template <std::size_t count, typename visitor_t>
  requires(count > 0)
// NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward) -- the visitor is only called, never moved from; it is a forwarding reference so that a temporary lambda binds
constexpr decltype(auto) visit_index(std::size_t index, visitor_t&& visitor) {
  return detail::visit_index_from<count, 0>(index, visitor);
}

}  // namespace schemacht::util
