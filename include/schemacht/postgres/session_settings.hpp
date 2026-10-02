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

  /**
   * @brief Prints dates and times in ISO form (`DateStyle` `ISO, YMD`: `2026-10-01 20:55:12.264-04`), the form the date and time
   * column types (`std::chrono::sys_time` and the rest) read from a result in text. Set this to `false` only if no row with such
   * a column is ever read as text.
   */
  bool iso_dates = true;

  /** @return The `SET` statements that apply the settings, separated by `;`, or an empty view if there is nothing to set. */
  [[nodiscard]] constexpr std::string_view sql() const {
    if ( disable_jit && iso_dates ) {
      return "SET jit = off; SET DateStyle = 'ISO, YMD'";
    }

    if ( disable_jit ) {
      return "SET jit = off";
    }
    return iso_dates ? "SET DateStyle = 'ISO, YMD'" : "";
  }
};

}  // namespace schemacht::postgres
