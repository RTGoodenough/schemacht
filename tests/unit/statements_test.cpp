#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "schemacht/postgres/statements.hpp"
#include "schemacht/schema/field.hpp"
#include "schemacht/schema/schema.hpp"
#include "support/schemas.hpp"

namespace pg = schemacht::postgres;
namespace t = schemacht::test;
namespace sch = schemacht::schema;

TEST_CASE("rows are inserted with one array per column", "[insert]") {
  const std::vector<t::People::row_type> rows{
      {t::Id{1}, t::Name{R"(a "quoted" name)"}, t::Nick{"al"}, t::Score{1.5}, t::Active{true}},
      {t::Id{2}, t::Name{"bo"}, t::Nick{std::nullopt}, t::Score{-2}, t::Active{false}},
  };
  const std::vector<pg::detail::InsertStatement> statements = pg::detail::build_insert<t::People, t::People::row_type>(rows);

  REQUIRE(statements.size() == 1);
  CHECK(
      statements[0].sql == R"(INSERT INTO "people" ("id", "name", "nick", "score", "active") )"
                           R"(SELECT * FROM unnest($1::bigint[], $2::text[], $3::text[], $4::double precision[], $5::boolean[]))"
  );
  CHECK(
      statements[0].values ==
      std::vector<std::string>{R"({"1","2"})", R"({"a \"quoted\" name","bo"})", R"({"al",NULL})", R"({"1.5","-2"})", R"({"t","f"})"}
  );
}

TEST_CASE("generated columns are not inserted", "[insert]") {
  const std::vector<t::WithGenerated::row_type> rows{{t::Id{1}, t::Doubled{0}, t::Name{"x"}}};
  const auto                                    statements = pg::detail::build_insert<t::WithGenerated, t::WithGenerated::row_type>(rows);

  CHECK(statements[0].sql == R"(INSERT INTO "with_generated" ("id", "name") SELECT * FROM unnest($1::bigint[], $2::text[]))");
  CHECK(statements[0].values.size() == 2);
}

TEST_CASE("rows without their generated columns are inserted the same way", "[insert]") {
  const std::vector<t::WithGenerated::insert_row_type> rows{t::WithGenerated::make_insert_row(1, "x"), t::WithGenerated::make_insert_row(2, "y")};
  const auto statements = pg::detail::build_insert<t::WithGenerated, t::WithGenerated::insert_row_type>(rows);

  REQUIRE(statements.size() == 1);
  CHECK(statements[0].sql == R"(INSERT INTO "with_generated" ("id", "name") SELECT * FROM unnest($1::bigint[], $2::text[]))");
  CHECK(statements[0].values == std::vector<std::string>{R"({"1","2"})", R"({"x","y"})"});

  // With an array column, in VALUES: only the columns sent have a placeholder.
  using NumberedTags = sch::Schema<"numbered_tags", t::NumberedId, t::Tags, t::Nick>;
  const std::vector<NumberedTags::insert_row_type> tagged{NumberedTags::make_insert_row(std::vector<std::string>{"a"}, std::nullopt)};
  const auto                                       values = pg::detail::build_insert<NumberedTags, NumberedTags::insert_row_type>(tagged);

  REQUIRE(values.size() == 1);
  CHECK(values[0].sql == R"(INSERT INTO "numbered_tags" ("tags", "nick") VALUES ($1, NULL))");
  CHECK(values[0].values == std::vector<std::string>{R"({"a"})"});
}

TEST_CASE("an array-typed column falls back to VALUES, split to fit the parameter limit", "[insert]") {
  std::vector<t::Tagged::row_type> rows;
  rows.reserve(40000);
  for ( std::int64_t id = 0; id < 40000; ++id ) {
    rows.push_back({t::Id{id}, t::Tags{{"a", "b"}}});
  }

  const auto statements = pg::detail::build_insert<t::Tagged, t::Tagged::row_type>(rows);

  REQUIRE(statements.size() == 2);  // 32767 rows of 2 parameters fit in one statement
  CHECK(statements[0].values.size() == std::size_t{2} * 32767);
  CHECK(statements[1].values.size() == std::size_t{2} * (40000 - 32767));
  CHECK(statements[0].sql.starts_with(R"(INSERT INTO "tagged" ("id", "tags") VALUES ($1, $2), ($3, $4))"));
  CHECK(statements[0].values[1] == R"({"a","b"})");
}

TEST_CASE("a table is created from its schema", "[ddl]") {
  CHECK(
      pg::detail::CreateTableSql<t::People>::VALUE ==
      R"(CREATE TABLE "people" ("id" bigint NOT NULL, "name" text NOT NULL, "nick" text, "score" double precision NOT NULL, )"
      R"("active" boolean NOT NULL, PRIMARY KEY ("id")))"
  );
  CHECK(
      pg::detail::CreateTableSql<t::Counted>::VALUE ==
      R"(CREATE TABLE "counted" ("id" bigint NOT NULL, "count" bigint NOT NULL, "doc" jsonb NOT NULL, PRIMARY KEY ("id")))"
  );

  using Pairs = sch::Schema<"pairs", sch::Field<std::int16_t, "a", sch::KeyRole::Primary>, sch::Field<bool, "b", sch::KeyRole::Primary>>;
  CHECK(pg::detail::CreateTableSql<Pairs>::VALUE == R"(CREATE TABLE "pairs" ("a" smallint NOT NULL, "b" boolean NOT NULL, PRIMARY KEY ("a", "b")))");

  // Each unique column is unique on its own, after the primary key.
  using Accounts = sch::Schema<
      "accounts", t::Id, sch::Field<std::string, "email", sch::KeyRole::Unique>,
      sch::Field<std::optional<std::string>, "phone", sch::KeyRole::Unique>>;
  CHECK(
      pg::detail::CreateTableSql<Accounts>::VALUE == R"(CREATE TABLE "accounts" ("id" bigint NOT NULL, "email" text NOT NULL, "phone" text, )"
                                                     R"(PRIMARY KEY ("id"), UNIQUE ("email"), UNIQUE ("phone")))"
  );

  // A std::vector column is an array of its element's type.
  using Arrays = sch::Schema<
      "arrays", t::Id, sch::Field<std::vector<std::string>, "tags">, sch::Field<std::optional<std::vector<std::int32_t>>, "ranks">,
      sch::Field<std::vector<std::byte>, "blob">>;
  CHECK(
      pg::detail::CreateTableSql<Arrays>::VALUE == R"(CREATE TABLE "arrays" ("id" bigint NOT NULL, "tags" text[] NOT NULL, "ranks" integer[], )"
                                                   R"("blob" bytea NOT NULL, PRIMARY KEY ("id")))"
  );

  // A generated key column is numbered by the database.
  CHECK(
      pg::detail::CreateTableSql<t::Numbered>::VALUE ==
      R"(CREATE TABLE "numbered" ("id" integer NOT NULL GENERATED ALWAYS AS IDENTITY, "name" text NOT NULL, "nick" text, PRIMARY KEY ("id")))"
  );

  using Keyless = sch::Schema<"keyless", t::Name>;
  CHECK(pg::detail::CreateTableSql<Keyless>::VALUE == R"(CREATE TABLE "keyless" ("name" text NOT NULL))");
  CHECK(pg::detail::DropTableSql<t::People>::VALUE == R"(DROP TABLE IF EXISTS "people")");
}

TEST_CASE("an insert gives back every column with RETURNING", "[insert]") {
  CHECK(pg::detail::ReturningSql<t::Numbered>::VALUE == R"( RETURNING "id", "name", "nick")");
  CHECK(pg::detail::ReturningSql<t::Tiles>::VALUE == R"( RETURNING "id", ST_AsBinary("rast") AS "rast")");  // read as a query reads it
}
