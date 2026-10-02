#pragma once

#include <string_view>

namespace schemacht::postgres {

/**
 * @brief What every connection this library opens is told about itself before it runs a statement.
 *
 * The settings are applied with `SET` once a connection is made (and again after the asynchronous client reconnects one), so
 * they work with any connection string and leave the rest of the string's options alone.
 */
struct SessionSettings {
  /**
   * @brief Turns off PostgreSQL's JIT compilation for the session.
   *
   * The library's statements are lookups and fetches, and JIT compilation only slows them down: past about 10,000 result
   * rows the server compiles the query, which can take a large share of a fetch's time. Set this to `false` to keep the
   * server's own setting, e.g. for raw statements that are long analytic queries.
   */
  bool disable_jit = true;

  /** @return The `SET` statement that applies the settings, or an empty view if there is nothing to set. */
  [[nodiscard]] constexpr std::string_view sql() const { return disable_jit ? "SET jit = off" : ""; }
};

}  // namespace schemacht::postgres
