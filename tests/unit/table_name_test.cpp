// A table named while the program runs: checked when made, by the rule a schema's own name follows.

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <stdexcept>
#include <string>

#include "schemacht/schema/table_name.hpp"

namespace sch = schemacht::schema;

TEST_CASE("a table name is quoted, with its schema if it has one", "[table_name]") {
  const sch::TableName plain("osm_roads");
  CHECK(plain.name() == "osm_roads");
  CHECK(plain.schema_name().empty());
  CHECK(plain.quoted() == R"("osm_roads")");

  const sch::TableName qualified("gis", "osm_roads_2026");
  CHECK(qualified.name() == "osm_roads_2026");
  CHECK(qualified.schema_name() == "gis");
  CHECK(qualified.quoted() == R"("gis"."osm_roads_2026")");

  CHECK(plain == sch::TableName("osm_roads"));
  CHECK(plain != sch::TableName("public", "osm_roads"));  // the same table only through the search path, not the same name
}

TEST_CASE("a table name that is not an identifier is refused", "[table_name]") {
  for ( const std::string& bad :
        std::initializer_list<std::string>{"", "Roads", "2026_roads", "osm roads", R"(osm"roads)", "osm.roads", std::string(64, 'a')} ) {
    CAPTURE(bad);
    CHECK_THROWS_AS(sch::TableName(bad), std::invalid_argument);
  }

  CHECK_NOTHROW(sch::TableName(std::string(63, 'a')));
  CHECK_THROWS_AS(sch::TableName("Gis", "roads"), std::invalid_argument);
  CHECK_THROWS_AS(sch::TableName("", "roads"), std::invalid_argument);
}
