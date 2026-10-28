#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

#include "schemacht/util/concepts/move_only_function.hpp"  // IWYU pragma: export

namespace schemacht::util {

template <typename signature_t>
class MoveOnlyFunction;

/**
 * @brief A callable of one signature that owns what it wraps, and can be moved but not copied: what `std::move_only_function`
 * is in C++23, for a library that compiles as C++20. It can hold a lambda that captured a promise or another completion, which
 * `std::function` cannot.
 *
 * A callable of up to three pointers' size that moves without throwing is kept inside the function itself (`HELD_INSIDE`
 * says); a larger one is allocated. So a task that captures a shared pointer and an error pointer is made without allocating,
 * which is what the library's error guarantee needs where a worker or the reactor posts a stream's completion (see
 * `AsyncClient`); a function itself is larger than that, so a task capturing a completion allocates, and is made where a
 * `std::bad_alloc` can still be reported.
 *
 * Empty when default-made, made from `nullptr`, or moved from (`operator bool` says), and calling it then is undefined, as with
 * `std::function`. Calling one does not allocate.
 */
template <typename result_t, typename... argument_ts>
class MoveOnlyFunction<result_t(argument_ts...)> {  // NOLINT(cppcoreguidelines-special-member-functions): all five are declared below
 public:
  MoveOnlyFunction() noexcept = default;

  /** @brief Empty, as default-made: `nullptr` spells "no completion" where one is optional. */
  MoveOnlyFunction(std::nullptr_t) noexcept {}  // NOLINT(google-explicit-constructor,*-explicit-constructor): as std::function

  /** @brief Takes `callable` by move (or copy, if an lvalue): the function holds it and calls it. */
  template <CallableAs<result_t, argument_ts...> callable_t>
    requires(! std::same_as<std::remove_cvref_t<callable_t>, MoveOnlyFunction> && std::constructible_from<std::decay_t<callable_t>, callable_t>)
  // NOLINTNEXTLINE(google-explicit-constructor,*-explicit-constructor) -- a lambda converts to it, as to a std::function
  MoveOnlyFunction(callable_t&& callable) : _invoke(&invoke_as<std::decay_t<callable_t>>), _manage(&manage_as<std::decay_t<callable_t>>) {
    using held_t = std::decay_t<callable_t>;

    if constexpr ( HELD_INSIDE<held_t> ) {
      ::new (static_cast<void*>(_storage.inside.data())) held_t(std::forward<callable_t>(callable));
    } else {
      _storage.outside = new held_t(std::forward<callable_t>(callable));  // NOLINT(cppcoreguidelines-owning-memory): `manage_as` deletes it
    }
  }

  MoveOnlyFunction(MoveOnlyFunction&& other) noexcept
      : _invoke(std::exchange(other._invoke, nullptr)), _manage(std::exchange(other._manage, &manage_nothing)) {
    _manage(_storage, &other._storage);
  }

  MoveOnlyFunction& operator=(MoveOnlyFunction&& other) noexcept {
    if ( this != &other ) {
      _manage(_storage, nullptr);
      _invoke = std::exchange(other._invoke, nullptr);
      _manage = std::exchange(other._manage, &manage_nothing);
      _manage(_storage, &other._storage);
    }

    return *this;
  }

  ~MoveOnlyFunction() { _manage(_storage, nullptr); }

  MoveOnlyFunction(const MoveOnlyFunction&) = delete;
  MoveOnlyFunction& operator=(const MoveOnlyFunction&) = delete;

  /** @brief Calls what it holds. @pre it holds something. */
  // NOLINTNEXTLINE(performance-unnecessary-value-param) -- the signature's own parameter types, as std::move_only_function takes them
  result_t operator()(argument_ts... arguments) { return _invoke(_storage, std::forward<argument_ts>(arguments)...); }

  /** @return Whether it holds something to call. */
  [[nodiscard]] explicit operator bool() const noexcept { return _invoke != nullptr; }

  /** @brief How big a callable fits inside the function: three pointers. */
  static constexpr std::size_t INSIDE_SIZE = 3 * sizeof(void*);

  /** @brief Whether a `held_t` is kept inside the function, so making a function of one does not allocate. */
  template <typename held_t>
  static constexpr bool HELD_INSIDE =
      sizeof(held_t) <= INSIDE_SIZE && alignof(held_t) <= alignof(void*) && std::is_nothrow_move_constructible_v<held_t>;

 private:
  /** @brief Where the callable is: in `inside`, or allocated and pointed to by `outside`. */
  union Storage {
    void* outside = nullptr;
    alignas(void*) std::array<std::byte, INSIDE_SIZE> inside;  // raw storage, constructed into
  };

  using Invoke = result_t (*)(Storage&, argument_ts&&...);
  /// Moves the callable in `*source` into `target` and destroys the one in `source`; with no `source`, destroys the one in `target`.
  using Manage = void (*)(Storage& target, Storage* source) noexcept;

  Storage _storage{};
  Invoke  _invoke = nullptr;
  Manage  _manage = &manage_nothing;

  template <typename held_t>
  [[nodiscard]] static held_t* held_in(Storage& storage) noexcept {
    if constexpr ( HELD_INSIDE<held_t> ) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): what was constructed there
      return std::launder(reinterpret_cast<held_t*>(storage.inside.data()));
    } else {
      return static_cast<held_t*>(storage.outside);
    }
  }

  template <typename held_t>
  static result_t invoke_as(Storage& storage, argument_ts&&... arguments) {
    return static_cast<result_t>((*held_in<held_t>(storage))(std::forward<argument_ts>(arguments)...));  // a void result drops the callable's
  }

  template <typename held_t>
  static void manage_as(Storage& target, Storage* source) noexcept {
    if constexpr ( HELD_INSIDE<held_t> ) {
      if ( source != nullptr ) {
        auto* moved = held_in<held_t>(*source);
        ::new (static_cast<void*>(target.inside.data())) held_t(std::move(*moved));
        moved->~held_t();

      } else {
        held_in<held_t>(target)->~held_t();
      }

    } else {
      if ( source != nullptr ) {
        target.outside = std::exchange(source->outside, nullptr);

      } else {
        delete held_in<held_t>(target);  // NOLINT(cppcoreguidelines-owning-memory): made by the constructor
      }
    }
  }

  static void manage_nothing(Storage& /*target*/, Storage* /*source*/) noexcept {}
};

}  // namespace schemacht::util
