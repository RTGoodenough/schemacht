#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "schemacht/postgres/statements.hpp"
#include "support/schemas.hpp"

namespace pg = schemacht::postgres;
namespace t = schemacht::test;

TEST_CASE("rows are inserted with one array per column", "[insert]") {
  const std::vector<t::People::row_type> rows{
      {t::Id{1}, t::Name{R"(a "quoted" name)"}, t::Nick{"al"}, t::Score{1.5}, t::Active{true}},
      {t::Id{2}, t::Name{"bo"}, t::Nick{std::nullopt}, t::Score{-2}, t::Active{false}},
  };
  const std::vector<pg::detail::InsertStatement> statements = pg::detail::build_insert<t::People>(rows);
  REQUIRE(statements.size() == 1);
  CHECK(statements[0].sql == R"(INSERT INTO "people" ("id", "name", "nick", "score", "active") )"
                             R"(SELECT * FROM unnest($1::bigint[], $2::text[], $3::text[], $4::double precision[], $5::boolean[]))");
  CHECK(statements[0].values == std::vector<std::string>{R"({"1","2"})", R"({"a \"quoted\" name","bo"})", R"({"al",NULL})", R"({"1.5","-2"})",
                                                         R"({"t","f"})"});
}

TEST_CASE("generated columns are not inserted", "[insert]") {
  const std::vector<t::WithGenerated::row_type> rows{{t::Id{1}, t::Doubled{0}, t::Name{"x"}}};
  const auto                                    statements = pg::detail::build_insert<t::WithGenerated>(rows);
  CHECK(statements[0].sql == R"(INSERT INTO "with_generated" ("id", "name") SELECT * FROM unnest($1::bigint[], $2::text[]))");
  CHECK(statements[0].values.size() == 2);
}

TEST_CASE("an array-typed column falls back to VALUES, split to fit the parameter limit", "[insert]") {
  std::vector<t::Tagged::row_type> rows;
  rows.reserve(40000);
  for ( std::int64_t id = 0; id < 40000; ++id ) {
    rows.push_back({t::Id{id}, t::Tags{{"a", "b"}}});
  }
  const auto statements = pg::detail::build_insert<t::Tagged>(rows);
  REQUIRE(statements.size() == 2);  // 32767 rows of 2 parameters fit in one statement
  CHECK(statements[0].values.size() == std::size_t{2} * 32767);
  CHECK(statements[1].values.size() == std::size_t{2} * (40000 - 32767));
  CHECK(statements[0].sql.starts_with(R"(INSERT INTO "tagged" ("id", "tags") VALUES ($1, $2), ($3, $4))"));
  CHECK(statements[0].values[1] == R"({"a","b"})");
}
