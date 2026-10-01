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
   * The library's statements are lookups and fetches, and JIT only costs them: from about 10,000 rows in a result the server
   * compiles the query, which took 9 ms of a 47 ms fetch and 41 ms of a 100,000-row one (benchmarks/README.md). Set this to
   * `false` to leave the server's own setting alone, e.g. for a raw statement that is a long analytic query.
   */
  bool disable_jit = true;

  /** @return The `SET` statement that applies the settings, or an empty view if there is nothing to set. */
  [[nodiscard]] constexpr std::string_view sql() const { return disable_jit ? "SET jit = off" : ""; }
};

}  // namespace schemacht::postgres
