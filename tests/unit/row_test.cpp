#include <catch2/catch_test_macros.hpp>

#include <concepts>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

#include "schemacht/schema.hpp"
#include "support/schemas.hpp"

namespace sch = schemacht::schema;
namespace t = schemacht::test;

TEST_CASE("make_row builds a row from plain values", "[row]") {
  const t::People::row_type row = t::People::make_row(7, "ann", std::nullopt, 1.5, true);
  CHECK(std::get<t::Id>(row).value == 7);
  CHECK(std::get<t::Name>(row).value == "ann");
  CHECK_FALSE(std::get<t::Nick>(row).value.has_value());
  CHECK(std::get<t::Score>(row).value == 1.5);
  CHECK(std::get<t::Active>(row).value);

  const auto with_nick = t::People::make_row(8, std::string("bo"), "b", 0.0, false);
  CHECK(std::get<t::Nick>(with_nick).value == "b");
}

TEST_CASE("get reads a column by its name", "[row]") {
  t::People::row_type row = t::People::make_row(7, "ann", "a", 1.5, true);
  CHECK(sch::get<"id">(row) == 7);
  CHECK(sch::get<"nick">(row) == "a");

  sch::get<"score">(row) = 2.5;  // a reference into the row
  CHECK(std::get<t::Score>(row).value == 2.5);

  const std::tuple<t::Nick, t::Id> projected{t::Nick{std::nullopt}, t::Id{3}};
  CHECK(sch::get<"id">(projected) == 3);
  CHECK_FALSE(sch::get<"nick">(projected).has_value());

  const std::string taken = sch::get<"name">(std::move(row));  // moved out of an rvalue row
  CHECK(taken == "ann");
}

TEST_CASE("get keeps the row's value category", "[row]") {
  using Row = t::People::row_type;
  STATIC_REQUIRE(std::same_as<decltype(sch::get<"name">(std::declval<Row&>())), std::string&>);
  STATIC_REQUIRE(std::same_as<decltype(sch::get<"name">(std::declval<const Row&>())), const std::string&>);
  STATIC_REQUIRE(std::same_as<decltype(sch::get<"name">(std::declval<Row&&>())), std::string&&>);
}
