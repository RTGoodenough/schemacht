#pragma once

// What the integration tests share: the test database's connection string, and running SQL given as text.

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <exception>
#include <future>
#include <string>
#include <utility>

#include "schemacht/postgres.hpp"

namespace schemacht::test {

/** @return The test database's libpq connection string, from SCHEMACHT_TEST_DB; skips the test if it is not set. */
inline std::string test_db() {
  const char* conninfo = std::getenv("SCHEMACHT_TEST_DB");
  if ( conninfo == nullptr ) {
    SKIP("SCHEMACHT_TEST_DB is not set");
  }

  return conninfo;
}

/** @brief Runs `sql` (no arguments, no rows wanted) on the pool and waits for it. */
inline void exec(postgres::Database& database, std::string sql) {
  std::promise<void> done;
  database.pool().run(std::move(sql), {}, [&done](const postgres::QueryResult& /*result*/, const std::exception_ptr& error) {
    if ( error ) {
      done.set_exception(error);
    } else {
      done.set_value();
    }
  });

  done.get_future().get();
}

}  // namespace schemacht::test
