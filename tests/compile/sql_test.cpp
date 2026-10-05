// The SQL that `Prepared` writes, checked while compiling: every STATIC_REQUIRE is a static_assert.

#include <catch2/catch_test_macros.hpp>

#include <concepts>
#include <string_view>
#include <tuple>

#include "schemacht/json.hpp"
#include "schemacht/query.hpp"
#include "schemacht/query/sql.hpp"
#include "schemacht/schema/table.hpp"
#include "support/schemas.hpp"

namespace q = schemacht::query;
namespace t = schemacht::test;

using People = q::On<t::People>;

namespace {

constexpr auto BY_ID = People::col<"id">() == q::arg<0>();
constexpr auto NAME_IN = People::col<"name">().in(q::arg<0>());
constexpr auto NAME_LIKE = q::like(People::col<"name">(), q::arg<0>());
constexpr auto OR_INSIDE_AND = (People::col<"id">() == q::arg<0>() || People::col<"name">() == q::arg<1>()) && People::col<"active">() == true;
constexpr auto AND_INSIDE_OR = People::col<"id">() == q::arg<0>() || (People::col<"name">() == q::arg<1>() && People::col<"active">() == false);
constexpr auto NEGATED = ! (People::col<"id">() > -3);
constexpr auto NO_NICK = People::col<"nick">().is_null() || People::col<"nick">().is_not_null();
constexpr auto ORDERED = q::select(People::col<"active">() == true)
                             .order_by(People::col<"score">().desc(), People::col<"id">().asc())
                             .limit(10)
                             .project(People::col<"name">(), People::col<"id">());
constexpr auto LIMIT_ARG = q::select<t::People>().limit(q::arg<0>());
constexpr auto SIMILAR = People::col<"name">().apply<t::trgm::Similar>(q::arg<0>());
constexpr auto MOST_SIMILAR = q::select(SIMILAR).order_by(People::col<"name">().ranked_by<t::trgm::Distance>(q::arg<0>())).limit(3);
constexpr auto SIMILAR_ABOVE = People::col<"name">().apply<t::trgm::SimilarAbove>(q::arg<0>(), q::arg<1>());
constexpr auto COVERING_NEAREST = q::select(q::On<t::Regions>::col<"area">().apply<t::geo::Covers>(q::arg<0>()))
                                      .order_by(q::On<t::Regions>::col<"area">().ranked_by<t::geo::Nearest>(q::arg<1>()));
constexpr auto HAS_KEY = q::On<t::Counted>::col<"doc">().apply<t::HasKey>(q::arg<0>());
using TwoDocs = schemacht::schema::Schema<
    "two_docs", t::Id, schemacht::schema::Field<schemacht::json::Json, "a">, schemacht::schema::Field<schemacht::json::Json, "b">>;
constexpr auto EITHER_HAS_KEY = q::On<TwoDocs>::col<"a">().apply<t::HasKey>(q::arg<0>()) || q::On<TwoDocs>::col<"b">().apply<t::HasKey>(q::arg<0>());
constexpr auto SAME_IGNORING_CASE = q::select(People::col<"name">().apply<t::expression::SameIgnoringCase>(q::arg<0>()))
                                        .order_by(People::col<"name">().asc())
                                        .project(People::col<"name">());
constexpr auto LOWER_STARTS_WITH = q::delete_where(People::col<"name">().apply<t::expression::LowerStartsWith>(q::arg<0>())).returning();
constexpr auto LOWER_SIMILAR_ABOVE = People::col<"name">().apply<t::expression::LowerSimilarAbove>(q::arg<0>(), q::arg<1>());
constexpr auto NAME_IS = q::On<t::Counted>::col<"doc">().apply<t::expression::NameIs>(q::arg<0>());
constexpr auto SAME_BYTES =
    q::update<t::People>().set(People::col<"active">(), true).where(People::col<"name">().apply<t::expression::SameBytes>(q::arg<0>()));
constexpr auto CONTAINS = schemacht::json::contains(q::On<t::Counted>::col<"doc">(), q::arg<0>());
constexpr auto COUNT_IN = q::On<t::Counted>::col<"count">().in(q::arg<0>());
constexpr auto ALL_TILES = q::select<t::Tiles>();
constexpr auto RAISE =
    q::update<t::People>().set(People::col<"score">(), q::arg<0>()).set(People::col<"active">(), true).where(People::col<"id">() == q::arg<1>());
constexpr auto WHERE_FIRST = q::update<t::People>().where(People::col<"active">() == false).set(People::col<"id">(), -1);
constexpr auto FORGET_NICKS = q::update<t::People>().set_null(People::col<"nick">()).all_rows();
constexpr auto REMOVE_INACTIVE = q::delete_where(People::col<"active">() == false || People::col<"name">().in(q::arg<0>()));
constexpr auto REMOVE_ALL = q::delete_all<t::People>();
constexpr auto RAISE_RETURNING = RAISE.returning(People::col<"score">(), People::col<"id">());
constexpr auto RETURNING_FIRST = q::update<t::People>().returning().set(People::col<"active">(), false).where(People::col<"id">() == q::arg<0>());
constexpr auto REMOVE_RETURNING = REMOVE_ALL.returning();
constexpr auto REMOVE_TILES = q::delete_where(q::On<t::Tiles>::col<"id">() == q::arg<0>()).returning(q::On<t::Tiles>::col<"rast">());

}  // namespace

TEST_CASE("comparisons and lists", "[sql]") {
  STATIC_REQUIRE(q::Prepared<BY_ID>::SQL == R"(SELECT "id", "name", "nick", "score", "active" FROM "people" WHERE "id" = $1)");
  STATIC_REQUIRE(q::Prepared<NAME_IN>::SQL == R"(SELECT "id", "name", "nick", "score", "active" FROM "people" WHERE "name" = ANY($1::text[]))");
  STATIC_REQUIRE(q::Prepared<COUNT_IN>::SQL == R"(SELECT "id", "count", "doc" FROM "counted" WHERE "count" = ANY($1::bigint[]))");
}

TEST_CASE("operations are written as their operator or function", "[sql]") {
  STATIC_REQUIRE(q::Prepared<NAME_LIKE>::SQL == R"(SELECT "id", "name", "nick", "score", "active" FROM "people" WHERE "name" ~~ $1::text)");
  STATIC_REQUIRE(q::Prepared<CONTAINS>::SQL == R"(SELECT "id", "count", "doc" FROM "counted" WHERE "doc" @> $1::jsonb)");
}

TEST_CASE("an extension's operations: the pg_trgm example", "[sql][extension]") {
  // A test written as an operator, the argument cast to the column's type.
  STATIC_REQUIRE(q::Prepared<SIMILAR>::SQL == R"(SELECT "id", "name", "nick", "score", "active" FROM "people" WHERE "name" % $1::text)");

  // A test that takes a number too: a function, the number a double precision.
  STATIC_REQUIRE(
      q::Prepared<SIMILAR_ABOVE>::SQL ==
      R"(SELECT "id", "name", "nick", "score", "active" FROM "people" WHERE similar_above("name", $1::text, $2::double precision))"
  );
  STATIC_REQUIRE(q::Prepared<SIMILAR_ABOVE>::SLOTS.info.at(1).is_real);

  // A ranking, here with the same argument as the test.
  STATIC_REQUIRE(
      q::Prepared<MOST_SIMILAR>::SQL ==
      R"(SELECT "id", "name", "nick", "score", "active" FROM "people" WHERE "name" % $1::text ORDER BY "name" <-> $1::text LIMIT 3)"
  );
  STATIC_REQUIRE(q::Prepared<MOST_SIMILAR>::ARG_COUNT == 1);
}

TEST_CASE("an operation or ranking whose argument has a type of its own", "[sql][extension]") {
  // Cast to the argument's type, not the column's.
  STATIC_REQUIRE(
      q::Prepared<COVERING_NEAREST>::SQL == R"(SELECT "id", "area" FROM "regions" WHERE "area" @> $1::point ORDER BY "area" <-> $2::point)"
  );
  STATIC_REQUIRE(q::Prepared<HAS_KEY>::SQL == R"(SELECT "id", "count", "doc" FROM "counted" WHERE "doc" ? $1::text)");

  STATIC_REQUIRE(q::Prepared<COVERING_NEAREST>::ARG_COUNT == 2);

  // Its type does not depend on the column, so one argument can be tested against several.
  STATIC_REQUIRE(q::Prepared<EITHER_HAS_KEY>::SQL == R"(SELECT "id", "a", "b" FROM "two_docs" WHERE "a" ? $1::text OR "b" ? $1::text)");
  STATIC_REQUIRE(q::Prepared<EITHER_HAS_KEY>::ARG_COUNT == 1);

  // `bind` takes the argument's type, and sends its text form.
  const auto bound = q::Prepared<COVERING_NEAREST>::bind(t::Point{"(0.5,0.5)"}, t::Point{"(2,2)"});
  REQUIRE(bound.params().size() == 2);
  CHECK(std::get<q::ArgumentText>(bound.params()[0]).text == "(0.5,0.5)");
  CHECK(std::get<q::ArgumentText>(bound.params()[1]).text == "(2,2)");
}

TEST_CASE("an operation on an expression of its column writes SQL around the column", "[sql][extension]") {
  STATIC_REQUIRE(q::WritesAroundColumn<t::expression::SameIgnoringCase>);
  STATIC_REQUIRE(q::WritesAroundColumn<t::expression::SameBytes>);  // a suffix alone
  STATIC_REQUIRE_FALSE(q::WritesAroundColumn<t::trgm::Similar>);
  STATIC_REQUIRE(q::column_prefix<t::expression::SameBytes>().empty());
  STATIC_REQUIRE(q::column_suffix<t::expression::SameBytes>() == "::bytea");

  // An operator, with SQL around the argument too. Only the test is written so: the select list and ORDER BY name the column.
  STATIC_REQUIRE(q::Prepared<SAME_IGNORING_CASE>::SQL == R"(SELECT "name" FROM "people" WHERE lower("name") = lower($1::text) ORDER BY "name")");

  // A function, and one that takes a number too; in a DELETE, whose RETURNING names the columns themselves.
  STATIC_REQUIRE(
      q::Prepared<LOWER_STARTS_WITH>::SQL ==
      R"(DELETE FROM "people" WHERE starts_with(lower("name"), $1::text) RETURNING "id", "name", "nick", "score", "active")"
  );
  STATIC_REQUIRE(
      q::Prepared<LOWER_SIMILAR_ABOVE>::SQL ==
      R"(SELECT "id", "name", "nick", "score", "active" FROM "people" WHERE similar_above(lower("name"), $1::text, $2::double precision))"
  );

  // The expression has another type than the column, so the operation declares its argument's.
  STATIC_REQUIRE(q::Prepared<NAME_IS>::SQL == R"(SELECT "id", "count", "doc" FROM "counted" WHERE ("doc" ->> 'name') = $1::text)");

  // A suffix alone, in an UPDATE.
  STATIC_REQUIRE(q::Prepared<SAME_BYTES>::SQL == R"(UPDATE "people" SET "active" = TRUE WHERE "name"::bytea = $1::text::bytea)");
}

TEST_CASE("AND and OR are parenthesized only where needed", "[sql]") {
  STATIC_REQUIRE(
      q::Prepared<OR_INSIDE_AND>::SQL ==
      R"(SELECT "id", "name", "nick", "score", "active" FROM "people" WHERE ("id" = $1 OR "name" = $2) AND "active" = TRUE)"
  );
  STATIC_REQUIRE(
      q::Prepared<AND_INSIDE_OR>::SQL ==
      R"(SELECT "id", "name", "nick", "score", "active" FROM "people" WHERE "id" = $1 OR "name" = $2 AND "active" = FALSE)"
  );
  STATIC_REQUIRE(q::Prepared<NEGATED>::SQL == R"(SELECT "id", "name", "nick", "score", "active" FROM "people" WHERE NOT ("id" > -3))");
  STATIC_REQUIRE(
      q::Prepared<NO_NICK>::SQL == R"(SELECT "id", "name", "nick", "score", "active" FROM "people" WHERE "nick" IS NULL OR "nick" IS NOT NULL)"
  );
}

TEST_CASE("ordering, limits and projection", "[sql]") {
  using Ordered = q::Prepared<ORDERED>;

  STATIC_REQUIRE(Ordered::SQL == R"(SELECT "name", "id" FROM "people" WHERE "active" = TRUE ORDER BY "score" DESC, "id" LIMIT 10)");
  STATIC_REQUIRE(std::same_as<Ordered::result_type, std::tuple<t::Name, t::Id>>);
  STATIC_REQUIRE(Ordered::ARG_COUNT == 0);

  STATIC_REQUIRE(q::Prepared<LIMIT_ARG>::SQL == R"(SELECT "id", "name", "nick", "score", "active" FROM "people" LIMIT $1)");
}

TEST_CASE("a type's SelectExpression is used for the columns it returns", "[sql]") {
  STATIC_REQUIRE(q::Prepared<ALL_TILES>::SQL == R"(SELECT "id", ST_AsBinary("rast") AS "rast" FROM "tiles")");
}

TEST_CASE("argument slots take their type from where they are used", "[sql]") {
  STATIC_REQUIRE(q::Prepared<OR_INSIDE_AND>::ARG_COUNT == 2);
  STATIC_REQUIRE(q::Prepared<NAME_IN>::SLOTS.info.at(0).is_list);
  STATIC_REQUIRE(q::Prepared<LIMIT_ARG>::SLOTS.info.at(0).is_limit);

  STATIC_REQUIRE(std::same_as<q::Prepared<BY_ID>::result_type, t::People::row_type>);
}

TEST_CASE("a statement knows where it names its table, and runs on another named while the program runs", "[sql][table_name]") {
  // The slot covers the quoted name exactly, wherever the statement puts it.
  constexpr auto TABLE_AT = [](std::string_view sql, q::sql::TableSlot slot) { return sql.substr(slot.at, slot.size); };
  STATIC_REQUIRE(TABLE_AT(q::Prepared<ORDERED>::SQL, q::Prepared<ORDERED>::TABLE_SLOT) == R"("people")");
  STATIC_REQUIRE(TABLE_AT(q::Prepared<RAISE_RETURNING>::SQL, q::Prepared<RAISE_RETURNING>::TABLE_SLOT) == R"("people")");
  STATIC_REQUIRE(TABLE_AT(q::Prepared<REMOVE_INACTIVE>::SQL, q::Prepared<REMOVE_INACTIVE>::TABLE_SLOT) == R"("people")");

  const auto bound = q::Prepared<RAISE_RETURNING>::bind(1.5, 7);
  CHECK(bound.sql() == q::Prepared<RAISE_RETURNING>::SQL);  // the schema's own table, unless aimed at another

  const auto archived = bound.on(schemacht::schema::Table<t::People>("archive"));
  CHECK(archived.sql() == R"(UPDATE "archive" SET "score" = $1, "active" = TRUE WHERE "id" = $2 RETURNING "score", "id")");
  CHECK(archived.params().size() == 2);
  CHECK(
      q::Prepared<ORDERED>::bind().on(schemacht::schema::Table<t::People>("gis", "people_2026")).sql() ==
      R"(SELECT "name", "id" FROM "gis"."people_2026" WHERE "active" = TRUE ORDER BY "score" DESC, "id" LIMIT 10)"
  );
}

TEST_CASE("updates and deletes", "[sql][command]") {
  STATIC_REQUIRE(q::Prepared<RAISE>::SQL == R"(UPDATE "people" SET "score" = $1, "active" = TRUE WHERE "id" = $2)");
  STATIC_REQUIRE(q::Prepared<WHERE_FIRST>::SQL == R"(UPDATE "people" SET "id" = -1 WHERE "active" = FALSE)");
  STATIC_REQUIRE(q::Prepared<FORGET_NICKS>::SQL == R"(UPDATE "people" SET "nick" = NULL)");
  STATIC_REQUIRE(q::Prepared<REMOVE_INACTIVE>::SQL == R"(DELETE FROM "people" WHERE "active" = FALSE OR "name" = ANY($1::text[]))");
  STATIC_REQUIRE(q::Prepared<REMOVE_ALL>::SQL == R"(DELETE FROM "people")");

  STATIC_REQUIRE(std::same_as<q::Prepared<RAISE>::result_type, std::tuple<>>);
  STATIC_REQUIRE(q::Prepared<RAISE>::ARG_COUNT == 2);
  STATIC_REQUIRE(q::Prepared<RAISE>::SLOTS.info.at(0).column == 3);  // the score
  STATIC_REQUIRE(q::Prepared<REMOVE_INACTIVE>::SLOTS.info.at(0).is_list);
  STATIC_REQUIRE(q::Prepared<FORGET_NICKS>::ARG_COUNT == 0);
}

TEST_CASE("updates and deletes return the columns they are asked for", "[sql][command]") {
  STATIC_REQUIRE(q::Prepared<RAISE_RETURNING>::SQL == R"(UPDATE "people" SET "score" = $1, "active" = TRUE WHERE "id" = $2 RETURNING "score", "id")");
  STATIC_REQUIRE(std::same_as<q::Prepared<RAISE_RETURNING>::result_type, std::tuple<t::Score, t::Id>>);
  STATIC_REQUIRE(q::Prepared<RAISE_RETURNING>::ARG_COUNT == 2);

  // Asked for before the rest, it is kept.
  STATIC_REQUIRE(
      q::Prepared<RETURNING_FIRST>::SQL == R"(UPDATE "people" SET "active" = FALSE WHERE "id" = $1 RETURNING "id", "name", "nick", "score", "active")"
  );
  STATIC_REQUIRE(std::same_as<q::Prepared<RETURNING_FIRST>::result_type, t::People::row_type>);

  STATIC_REQUIRE(q::Prepared<REMOVE_RETURNING>::SQL == R"(DELETE FROM "people" RETURNING "id", "name", "nick", "score", "active")");
  STATIC_REQUIRE(q::Prepared<REMOVE_TILES>::SQL == R"(DELETE FROM "tiles" WHERE "id" = $1 RETURNING ST_AsBinary("rast") AS "rast")");
  STATIC_REQUIRE(std::same_as<q::Prepared<REMOVE_TILES>::result_type, std::tuple<t::Raster>>);
}

TEST_CASE("which columns an update can set", "[sql][command]") {
  using NameRef = decltype(People::col<"name">());
  using NickRef = decltype(People::col<"nick">());
  using Generated = decltype(q::On<t::WithGenerated>::col<"doubled">());

  STATIC_REQUIRE(q::SettableColumn<NameRef, t::People>);
  STATIC_REQUIRE_FALSE(q::NullableColumn<NameRef, t::People>);
  STATIC_REQUIRE(q::NullableColumn<NickRef, t::People>);
  STATIC_REQUIRE_FALSE(q::SettableColumn<Generated, t::WithGenerated>);  // the database computes it
  STATIC_REQUIRE_FALSE(q::ColumnOf<NameRef, t::Counted>);                // another schema's column
}
