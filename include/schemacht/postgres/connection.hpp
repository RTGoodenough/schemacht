#pragma once

#include <pqxx/pqxx>

#include <string>
#include <string_view>

#include "schemacht/postgres/session_settings.hpp"

// libpqxx's classes change layout between minor versions, and the linker cannot tell headers of one from a library of another (a
// system libpqxx-dev of an older version is the usual way to mix them: `-I/usr/include` ahead of schemacht's, or `-lpqxx` finding
// the system's). The version below is the one schemacht's libraries are built against (GIT_TAG of pqxx in CMakeLists.txt).
#if PQXX_VERSION_MAJOR != 7 || PQXX_VERSION_MINOR != 10
#error \
    "schemacht is built against libpqxx 7.10, but these are the headers of another version (see PQXX_VERSION). Use the libpqxx installed beside schemacht (<prefix>/include, <prefix>/lib) and no other: link schemacht::schemacht from CMake rather than listing libraries by hand."
#endif

namespace schemacht::postgres {

/** @brief Thin RAII wrapper around a libpqxx connection. */
class Connection {
 public:
  /**
   * @brief Connects and applies `settings`.
   * @throws pqxx::broken_connection if the connection can't be made.
   */
  explicit Connection(const std::string& conninfo, const SessionSettings& settings = {}) : _conn(conninfo) {
    if ( const std::string_view setup = settings.sql(); ! setup.empty() ) {
      pqxx::nontransaction txn(_conn);
      txn.exec(setup);
    }
  }

  Connection(const Connection&) = delete;
  Connection(Connection&&) noexcept = default;
  Connection& operator=(const Connection&) = delete;
  Connection& operator=(Connection&&) noexcept = default;
  ~Connection() = default;

  [[nodiscard]] pqxx::connection&       handle() noexcept { return _conn; }
  [[nodiscard]] const pqxx::connection& handle() const noexcept { return _conn; }

 private:
  pqxx::connection _conn;
};

}  // namespace schemacht::postgres
