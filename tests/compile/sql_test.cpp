// The SQL that `Prepared` writes, checked while compiling: every STATIC_REQUIRE is a static_assert.

#include <catch2/catch_test_macros.hpp>

#include <concepts>
#include <tuple>

#include "schemacht/json.hpp"
#include "schemacht/query.hpp"
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
constexpr auto NEGATED = !(People::col<"id">() > -3);
constexpr auto NO_NICK = People::col<"nick">().is_null() || People::col<"nick">().is_not_null();
constexpr auto ORDERED = q::select(People::col<"active">() == true)
                             .order_by(People::col<"score">().desc(), People::col<"id">().asc())
                             .limit(10)
                             .project(People::col<"name">(), People::col<"id">());
constexpr auto LIMIT_ARG = q::select<t::People>().limit(q::arg<0>());
constexpr auto NEAREST = q::select<t::People>().order_by(People::col<"score">().ranked_by<t::Nearest>(q::arg<0>())).limit(3);
constexpr auto WITHIN = People::col<"score">().apply<t::WithinDistance>(q::arg<0>(), q::arg<1>());
constexpr auto CONTAINS = schemacht::json::contains(q::On<t::Counted>::col<"doc">(), q::arg<0>());
constexpr auto COUNT_IN = q::On<t::Counted>::col<"count">().in(q::arg<0>());
constexpr auto ALL_TILES = q::select<t::Tiles>();

}  // namespace

TEST_CASE("comparisons and lists", "[sql]") {
  STATIC_REQUIRE(q::Prepared<BY_ID>::SQL == R"(SELECT "id", "name", "nick", "score", "active" FROM "people" WHERE "id" = $1)");
  STATIC_REQUIRE(q::Prepared<NAME_IN>::SQL == R"(SELECT "id", "name", "nick", "score", "active" FROM "people" WHERE "name" = ANY($1::text[]))");
  STATIC_REQUIRE(q::Prepared<COUNT_IN>::SQL == R"(SELECT "id", "count", "doc" FROM "counted" WHERE "count" = ANY($1::bigint[]))");
}

TEST_CASE("operations are written as their operator or function", "[sql]") {
  STATIC_REQUIRE(q::Prepared<NAME_LIKE>::SQL == R"(SELECT "id", "name", "nick", "score", "active" FROM "people" WHERE "name" ~~ $1::text)");
  STATIC_REQUIRE(q::Prepared<CONTAINS>::SQL == R"(SELECT "id", "count", "doc" FROM "counted" WHERE "doc" @> $1::jsonb)");
  STATIC_REQUIRE(q::Prepared<WITHIN>::SQL ==
                 R"(SELECT "id", "name", "nick", "score", "active" FROM "people" WHERE within_distance("score", $1::double precision, $2::double precision))");
}

TEST_CASE("AND and OR are parenthesized only where needed", "[sql]") {
  STATIC_REQUIRE(q::Prepared<OR_INSIDE_AND>::SQL ==
                 R"(SELECT "id", "name", "nick", "score", "active" FROM "people" WHERE ("id" = $1 OR "name" = $2) AND "active" = TRUE)");
  STATIC_REQUIRE(q::Prepared<AND_INSIDE_OR>::SQL ==
                 R"(SELECT "id", "name", "nick", "score", "active" FROM "people" WHERE "id" = $1 OR "name" = $2 AND "active" = FALSE)");
  STATIC_REQUIRE(q::Prepared<NEGATED>::SQL == R"(SELECT "id", "name", "nick", "score", "active" FROM "people" WHERE NOT ("id" > -3))");
  STATIC_REQUIRE(q::Prepared<NO_NICK>::SQL ==
                 R"(SELECT "id", "name", "nick", "score", "active" FROM "people" WHERE "nick" IS NULL OR "nick" IS NOT NULL)");
}

TEST_CASE("ordering, limits and projection", "[sql]") {
  using Ordered = q::Prepared<ORDERED>;
  STATIC_REQUIRE(Ordered::SQL == R"(SELECT "name", "id" FROM "people" WHERE "active" = TRUE ORDER BY "score" DESC, "id" LIMIT 10)");
  STATIC_REQUIRE(std::same_as<Ordered::result_type, std::tuple<t::Name, t::Id>>);
  STATIC_REQUIRE(Ordered::ARG_COUNT == 0);

  STATIC_REQUIRE(q::Prepared<LIMIT_ARG>::SQL == R"(SELECT "id", "name", "nick", "score", "active" FROM "people" LIMIT $1)");
  STATIC_REQUIRE(q::Prepared<NEAREST>::SQL ==
                 R"(SELECT "id", "name", "nick", "score", "active" FROM "people" ORDER BY "score" <-> $1::double precision LIMIT 3)");
}

TEST_CASE("a type's SelectExpression is used for the columns it returns", "[sql]") {
  STATIC_REQUIRE(q::Prepared<ALL_TILES>::SQL == R"(SELECT "id", ST_AsBinary("rast") AS "rast" FROM "tiles")");
}

TEST_CASE("argument slots take their type from where they are used", "[sql]") {
  STATIC_REQUIRE(q::Prepared<OR_INSIDE_AND>::ARG_COUNT == 2);
  STATIC_REQUIRE(q::Prepared<NAME_IN>::SLOTS.info.at(0).is_list);
  STATIC_REQUIRE(q::Prepared<LIMIT_ARG>::SLOTS.info.at(0).is_limit);
  STATIC_REQUIRE(q::Prepared<WITHIN>::SLOTS.info.at(1).is_real);
  STATIC_REQUIRE(std::same_as<q::Prepared<BY_ID>::result_type, t::People::row_type>);
}
