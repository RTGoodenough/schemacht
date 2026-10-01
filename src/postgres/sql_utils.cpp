#include "schemacht/postgres/sql_utils.hpp"

#include <stdexcept>
#include <string>

#include "schemacht/util/identifier.hpp"

namespace schemacht::postgres {

std::string quote_identifier(const std::string& name) {
  if ( ! util::is_identifier(name) ) {
    throw std::invalid_argument("Invalid table/column identifier: " + name);
  }
  return "\"" + name + "\"";
}

std::string quote_identifier_list(std::span<const std::string_view> names) {
  std::string result;
  bool        first = true;
  for ( const std::string_view name : names ) {
    if ( ! first ) {
      result += ", ";
    }
    first = false;
    result += quote_identifier(std::string(name));
  }
  return result;
}

}  // namespace schemacht::postgres
