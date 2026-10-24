#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "schemacht/postgres/statements.hpp"
#include "schemacht/postgres/table_differences.hpp"
#include "schemacht/schema/field.hpp"
#include "schemacht/schema/schema.hpp"
#include "schemacht/schema/table.hpp"
#include "support/schemas.hpp"
#include "support/text.hpp"

namespace pg = schemacht::postgres;
namespace t = schemacht::test;
namespace sch = schemacht::schema;

namespace {

/** @return `statement`'s arguments, to compare. */
template <typename statement_t>
std::vector<std::string> params_of(const statement_t& statement) {
  return {statement.params().begin(), statement.params().end()};
}

}  // namespace

TEST_CASE("rows are inserted with one array per column", "[insert]") {
  const std::vector<t::People::row_type> rows{
      {t::Id{1}, t::Name{R"(a "quoted" name)"}, t::Nick{"al"}, t::Score{1.5}, t::Active{true}},
      {t::Id{2}, t::Name{"bo"}, t::Nick{std::nullopt}, t::Score{-2}, t::Active{false}},
  };
  const auto statement = pg::insert_statement<t::People>(rows);

  CHECK(
      statement.sql() == R"(INSERT INTO "people" ("id", "name", "nick", "score", "active") SELECT "id", "name", "nick", "score", "active" )"
                         R"(FROM unnest($1::bigint[], $2::text[], $3::text[], $4::double precision[], $5::boolean[]) )"
                         R"(AS u("id", "name", "nick", "score", "active"))"
  );
  CHECK(
      params_of(statement) ==
      std::vector<std::string>{R"({"1","2"})", R"({"a \"quoted\" name","bo"})", R"({"al",NULL})", R"({"1.5","-2"})", R"({"t","f"})"}
  );
}

TEST_CASE("generated columns are not inserted", "[insert]") {
  const std::vector<t::WithGenerated::row_type> rows{{t::Id{1}, t::Doubled{0}, t::Name{"x"}}};
  const auto                                    statement = pg::insert_statement<t::WithGenerated>(rows);

  CHECK(
      statement.sql() == R"(INSERT INTO "with_generated" ("id", "name") SELECT "id", "name" FROM unnest($1::bigint[], $2::text[]) AS u("id", "name"))"
  );
  CHECK(params_of(statement).size() == 2);
}

TEST_CASE("rows without their generated columns are inserted the same way", "[insert]") {
  const std::vector<t::WithGenerated::insert_row_type> rows{t::WithGenerated::make_insert_row(1, "x"), t::WithGenerated::make_insert_row(2, "y")};
  const auto                                           statement = pg::insert_statement<t::WithGenerated>(rows);

  CHECK(
      statement.sql() == R"(INSERT INTO "with_generated" ("id", "name") SELECT "id", "name" FROM unnest($1::bigint[], $2::text[]) AS u("id", "name"))"
  );
  CHECK(params_of(statement) == std::vector<std::string>{R"({"1","2"})", R"({"x","y"})"});
}

TEST_CASE("an array column goes as text, each value its own array literal, and is cast back", "[insert]") {
  // unnest would flatten an array of arrays: each row's array goes as one text element instead.
  const std::vector<t::Tagged::row_type> rows{{t::Id{1}, t::Tags{{"a", "b,c"}}}, {t::Id{2}, t::Tags{{}}}};
  const auto                             statement = pg::insert_statement<t::Tagged>(rows);

  CHECK(
      statement.sql() == R"(INSERT INTO "tagged" ("id", "tags") SELECT "id", "tags"::text[] FROM unnest($1::bigint[], $2::text[]) AS u("id", "tags"))"
  );
  CHECK(params_of(statement) == std::vector<std::string>{R"({"1","2"})", R"({"{\"a\",\"b,c\"}","{}"})"});

  // Without the generated column: only the columns sent are in it.
  using NumberedTags = sch::Schema<"numbered_tags", t::NumberedId, t::Tags, t::Nick>;
  const std::vector<NumberedTags::insert_row_type> tagged{NumberedTags::make_insert_row(std::vector<std::string>{"a"}, std::nullopt)};
  const auto                                       numbered = pg::insert_statement<NumberedTags>(tagged);

  CHECK(
      numbered.sql() == R"(INSERT INTO "numbered_tags" ("tags", "nick") SELECT "tags"::text[], "nick" FROM unnest($1::text[], $2::text[]) )"
                        R"(AS u("tags", "nick"))"
  );
  CHECK(params_of(numbered) == std::vector<std::string>{R"({"{\"a\"}"})", "{NULL}"});
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

TEST_CASE("a table named while the program runs gets the same statements, naming it", "[ddl][insert][table_name]") {
  const sch::Table<t::People>   archive("gis", "people_2026");
  const sch::Table<t::Numbered> numbered("gis", "people_2026");

  CHECK(pg::drop_table_statement<t::People>().on(archive).sql() == R"(DROP TABLE IF EXISTS "gis"."people_2026")");
  CHECK(pg::drop_table_statement<t::People>().sql() == R"(DROP TABLE IF EXISTS "people")");
  CHECK(
      pg::create_table_statement<t::Numbered>().on(numbered).sql() ==
      R"(CREATE TABLE "gis"."people_2026" ("id" integer NOT NULL GENERATED ALWAYS AS IDENTITY, "name" text NOT NULL, "nick" text, )"
      R"(PRIMARY KEY ("id")))"
  );

  const std::vector<t::People::row_type> people{{t::Id{1}, t::Name{"al"}, t::Nick{std::nullopt}, t::Score{1}, t::Active{true}}};
  CHECK(pg::insert_statement<t::People>(people).on(archive).sql().starts_with(R"(INSERT INTO "gis"."people_2026" ("id", )"));
}

TEST_CASE("an insert gives back every column with RETURNING", "[insert]") {
  CHECK(pg::detail::ReturningSql<t::Numbered>::VALUE == R"( RETURNING "id", "name", "nick")");
  const std::vector<t::Numbered::insert_row_type> rows{t::Numbered::make_insert_row("a", std::nullopt)};
  CHECK(pg::insert_returning_statement<t::Numbered>(rows).sql().ends_with(R"(AS u("name", "nick") RETURNING "id", "name", "nick")"));
  CHECK(pg::detail::ReturningSql<t::Tiles>::VALUE == R"( RETURNING "id", ST_AsBinary("rast") AS "rast")");  // read as a query reads it
}

TEST_CASE("a statement made from a schema, sent as a temporary, gives up its arguments rather than copying them", "[insert]") {
  const std::vector<t::Tagged::row_type> rows{{t::Id{1}, t::Tags{{"a"}}}};
  auto                                   statement = pg::insert_statement<t::Tagged>(rows);
  const auto                             copied = pg::detail::wire(statement);
  CHECK(statement.params().size() == 2);

  const auto moved = pg::detail::wire(std::move(statement));
  CHECK(moved.params == copied.params);
  CHECK(moved.sql == copied.sql);
  CHECK(statement.params().empty());  // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move) -- what this checks
}

namespace {

/** @brief A merge by a SQL function of the application's own: `keep_longer("current"."name", excluded."name")`. */
struct KeepLonger {
  static constexpr std::string_view FUNCTION = "keep_longer";
};

/** @brief A merge by a function named with its schema: `app.merge_scores("current"."score", excluded."score")`. */
struct MergeScores {
  static constexpr std::string_view FUNCTION = "app.merge_scores";
};

/** @brief A merge by a SQL operator: `"current"."score" + excluded."score"`. */
struct Add {
  static constexpr std::string_view OPERATOR = "+";
};

}  // namespace

TEST_CASE("an upsert updates the row already there: merged as a MergeWith says, else overwritten", "[upsert]") {
  const std::vector<t::People::row_type> people{{t::Id{1}, t::Name{"al"}, t::Nick{std::nullopt}, t::Score{1}, t::Active{true}}};
  const auto statement = pg::upsert_statement<t::People, pg::MergeWith<"name", KeepLonger>, pg::MergeWith<"score", Add>>(people);

  CHECK(
      statement.sql() == R"(INSERT INTO "people" AS "current" ("id", "name", "nick", "score", "active") )"
                         R"(SELECT "id", "name", "nick", "score", "active" )"
                         R"(FROM unnest($1::bigint[], $2::text[], $3::text[], $4::double precision[], $5::boolean[]) )"
                         R"(AS u("id", "name", "nick", "score", "active") )"
                         R"(ON CONFLICT ("id") DO UPDATE SET "name" = keep_longer("current"."name", excluded."name"), )"
                         R"("nick" = excluded."nick", "score" = "current"."score" + excluded."score", "active" = excluded."active")"
  );
  CHECK(params_of(statement) == params_of(pg::insert_statement<t::People>(people)));  // its arguments are the insert's

  const sch::Table<t::People> archive("gis", "people_2026");
  CHECK(pg::upsert_statement<t::People>(people).on(archive).sql().starts_with(R"(INSERT INTO "gis"."people_2026" AS "current" ("id", )"));
}

TEST_CASE("an upsert calls a merge function named with its schema there, whatever schema the table is in", "[upsert]") {
  const std::vector<t::People::row_type> people{{t::Id{1}, t::Name{"al"}, t::Nick{std::nullopt}, t::Score{1}, t::Active{true}}};
  const sch::Table<t::People>            archive("gis", "people_2026");
  const auto                             statement = pg::upsert_statement<t::People, pg::MergeWith<"score", MergeScores>>(people).on(archive);

  CHECK(statement.sql().starts_with(R"(INSERT INTO "gis"."people_2026" AS "current" )"));
  CHECK(schemacht::test::contains(statement.sql(), R"("score" = app.merge_scores("current"."score", excluded."score"))"));
}

TEST_CASE("an upsert leaves a kept column of the row already there as it is", "[upsert]") {
  const std::vector<t::People::row_type> people{{t::Id{1}, t::Name{"al"}, t::Nick{std::nullopt}, t::Score{1}, t::Active{true}}};
  CHECK(
      pg::upsert_statement<t::People, pg::Keep<"name">, pg::MergeWith<"score", Add>, pg::Keep<"active">>(people).sql().ends_with(
          R"( ON CONFLICT ("id") DO UPDATE SET "nick" = excluded."nick", "score" = "current"."score" + excluded."score")"
      )
  );
}

TEST_CASE("an upsert matches on the whole primary key, and never sets a key or generated column", "[upsert]") {
  const std::vector<t::Pairs::row_type> pairs{t::Pairs::make_row(1, 2, 3)};
  CHECK(pg::upsert_statement<t::Pairs>(pairs).sql().ends_with(R"( ON CONFLICT ("a", "b") DO UPDATE SET "v" = excluded."v")"));

  const std::vector<t::WithGenerated::insert_row_type> generated{t::WithGenerated::make_insert_row(1, "a")};
  CHECK(
      pg::upsert_statement<t::WithGenerated>(generated).sql() ==
      R"(INSERT INTO "with_generated" AS "current" ("id", "name") SELECT "id", "name" FROM unnest($1::bigint[], $2::text[]) )"
      R"(AS u("id", "name") ON CONFLICT ("id") DO UPDATE SET "name" = excluded."name")"
  );
}

TEST_CASE("an upsert with a ConflictOn matches on that unique column, and never sets it or the primary key", "[upsert]") {
  // The schema may then have a key the database numbers: the matched row keeps it.
  using WayId = sch::Field<std::int64_t, "way_id", sch::KeyRole::Primary, true>;
  using Dataset = sch::Field<std::string, "dataset">;
  using NodeIds = sch::Field<std::vector<std::int64_t>, "node_ids", sch::KeyRole::Unique>;
  using Pushed = sch::Field<std::int64_t, "pushed">;
  using Ways = sch::Schema<"ways", WayId, Dataset, NodeIds, Pushed>;

  const std::vector<Ways::insert_row_type> ways{Ways::make_insert_row("north", std::vector<std::int64_t>{1, 2}, 7)};
  CHECK(
      pg::upsert_statement<Ways, pg::ConflictOn<"node_ids">>(ways).sql() ==
      R"(INSERT INTO "ways" AS "current" ("dataset", "node_ids", "pushed") SELECT "dataset", "node_ids"::bigint[], "pushed" )"
      R"(FROM unnest($1::text[], $2::text[], $3::bigint[]) AS u("dataset", "node_ids", "pushed") )"
      R"(ON CONFLICT ("node_ids") DO UPDATE SET "dataset" = excluded."dataset", "pushed" = excluded."pushed")"
  );

  // A key the rows carry is not set either, and the rules apply to the rest, wherever the ConflictOn is among them.
  using Email = sch::Field<std::string, "email", sch::KeyRole::Unique>;
  using Users = sch::Schema<"users", t::Id, Email, t::Name, t::Score>;
  const std::vector<Users::row_type> users{Users::make_row(1, "al@example.com", "al", 2.0)};
  CHECK(
      pg::upsert_statement<Users, pg::MergeWith<"score", Add>, pg::ConflictOn<"email">>(users).sql().ends_with(
          R"( ON CONFLICT ("email") DO UPDATE SET "name" = excluded."name", "score" = "current"."score" + excluded."score")"
      )
  );

  // A composite key is skipped whole.
  using Code = sch::Field<std::string, "code", sch::KeyRole::Unique>;
  using CodedPairs = sch::Schema<"coded_pairs", t::PairA, t::PairB, Code, t::PairValue>;
  const std::vector<CodedPairs::row_type> coded{CodedPairs::make_row(1, 2, "x", 3)};
  CHECK(pg::upsert_statement<CodedPairs, pg::ConflictOn<"code">>(coded).sql().ends_with(R"( ON CONFLICT ("code") DO UPDATE SET "v" = excluded."v")"));

  // A schema with no primary key at all.
  using Setting = sch::Schema<"settings", sch::Field<std::string, "key", sch::KeyRole::Unique>, sch::Field<std::string, "value">>;
  const std::vector<Setting::row_type> settings{Setting::make_row("theme", "dark")};
  CHECK(
      pg::upsert_statement<Setting, pg::ConflictOn<"key">>(settings).sql().ends_with(
          R"( ON CONFLICT ("key") DO UPDATE SET "value" = excluded."value")"
      )
  );
}

TEST_CASE("a statement given as text is sent as it is, with its arguments' text", "[unchecked_sql]") {
  const auto statement = pg::unchecked_sql(R"(UPDATE "people" SET "score" = $1 WHERE "name" = ANY($2))", 2.5, std::vector<std::string>{"a", "b,c"});
  CHECK(statement.sql() == R"(UPDATE "people" SET "score" = $1 WHERE "name" = ANY($2))");
  CHECK(params_of(statement) == std::vector<std::string>{"2.5", R"({"a","b,c"})"});
  CHECK(! statement.error());

  const auto plain = pg::unchecked_sql(R"(CREATE INDEX ON "people" ("name"))");
  CHECK(plain.params().empty());

  const std::string_view view = "a view";
  CHECK(
      params_of(pg::unchecked_sql("SELECT $1, $2, $3", "a literal", view, std::string("a string"))) ==
      std::vector<std::string>{"a literal", "a view", "a string"}
  );
}

TEST_CASE("a statement given as text, sent as a temporary, gives up its arguments rather than copying them", "[unchecked_sql]") {
  auto       statement = pg::unchecked_sql("SELECT $1", std::string("a"));
  const auto copied = pg::detail::wire(statement);

  const auto moved = pg::detail::wire(std::move(statement));
  CHECK(moved.params == copied.params);
  CHECK(moved.sql == copied.sql);
  CHECK(moved.column_names.empty());
  CHECK(statement.params().empty());  // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move) -- what this checks
}

TEST_CASE("a table check lists what the schema says of each column, and names the table once", "[table_differences]") {
  const auto check = pg::table_differences_statement<t::Numbered>();

  CHECK(check.sql().starts_with(
      R"(WITH RECURSIVE "target" AS (SELECT "t"."name", "c"."oid", "c"."relkind" IN ('r', 'p') AS "is_table" FROM (VALUES ('"numbered"')) )"
      R"(AS "t" ("name") LEFT JOIN "pg_catalog"."pg_class" AS "c" )"
  ));
  CHECK(
      schemacht::test::contains(
          check.sql(),
          R"(AS (VALUES (1, 'id', 'integer', TRUE, TRUE, TRUE), (2, 'name', 'text', TRUE, FALSE, FALSE), (3, 'nick', 'text', FALSE, FALSE, FALSE)), )"
      )
  );
  CHECK(check.sql().ends_with(R"(SELECT "problem", "column", "expected", "found" FROM "differences" ORDER BY "position", "ordinal", "column")"));
  CHECK(check.params().empty());
  CHECK(pg::detail::wire(check).column_names == std::vector<std::string_view>{"problem", "column", "expected", "found"});

  const sch::Table<t::Numbered> archive("gis", "people_2026");
  CHECK(schemacht::test::contains(check.on(archive).sql(), R"((VALUES ('"gis"."people_2026"')) AS "t" ("name"))"));

  // A schema without a key says nothing of the table's.
  CHECK(schemacht::test::contains(check.sql(), "'primary_key'"));
  CHECK_FALSE(schemacht::test::contains(pg::table_differences_statement<t::Keyless>().sql(), "'primary_key'"));
}

TEST_CASE("a table difference is put in words", "[table_differences]") {
  using enum pg::TableProblem;
  const auto words = [](pg::TableProblem problem, std::string column, std::string expected, std::string found) {
    return pg::describe(pg::TableDifference{problem, std::move(column), std::move(expected), std::move(found)});
  };

  CHECK(words(MissingTable, "", R"("gis"."roads")", "") == R"(the table "gis"."roads" is not in the database)");
  CHECK(words(MissingColumn, "geom", "geometry(LineString,4326)", "") == R"(the column "geom" (geometry(LineString,4326)) is missing)");
  CHECK(
      words(UnknownType, "geom", "geometry(LineString,4326)", "text") ==
      R"(the column "geom" is of type text, and the database has no type geometry(LineString,4326))"
  );
  CHECK(words(WrongType, "id", "bigint", "integer") == R"(the column "id" is of type integer, not bigint)");
  CHECK(words(Nullability, "name", "NOT NULL", "nullable") == R"(the column "name" is nullable, not NOT NULL)");
  CHECK(words(NoDefault, "id", "", "") == R"(the column "id" has no default, identity or expression to fill it in, and is never inserted)");
  CHECK(words(AlwaysGenerated, "id", "", "") == R"(the column "id" is always generated by the database, and is inserted)");
  CHECK(words(PrimaryKey, "", "a, b", "a") == "the primary key is (a), not (a, b)");
  CHECK(words(PrimaryKey, "", "id", "") == "the table has no primary key, not (id)");
  CHECK(
      words(ExtraRequiredColumn, "required", "", "integer") == R"(the column "required" (integer) is NOT NULL with no default, and not in the schema)"
  );
}
