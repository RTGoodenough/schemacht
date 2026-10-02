#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "schemacht/postgres/statements.hpp"
#include "schemacht/query.hpp"
#include "support/schemas.hpp"

namespace q = schemacht::query;
namespace t = schemacht::test;
namespace pg = schemacht::postgres;

using People = q::On<t::People>;

namespace {

constexpr auto BY_NAME_AND_SCORE = People::col<"name">() == q::arg<0>() && People::col<"score">() > q::arg<1>();
constexpr auto IDS_LIMITED = q::select(People::col<"id">().in(q::arg<0>())).limit(q::arg<1>());

}  // namespace

TEST_CASE("bind converts arguments to their column's type", "[prepared]") {
  const auto bound = q::Prepared<BY_NAME_AND_SCORE>::bind("ann", 2.0);  // a string literal for a std::string, a double for a double
  REQUIRE(bound.params().size() == 2);
  CHECK(std::get<std::string>(bound.params()[0]) == "ann");
  CHECK(std::get<double>(bound.params()[1]) == 2.0);
  CHECK(bound.sql() == q::Prepared<BY_NAME_AND_SCORE>::SQL);
}

TEST_CASE("a limit argument is a non-negative std::int64_t", "[prepared]") {
  const auto bound = q::Prepared<IDS_LIMITED>::bind(std::vector<std::int64_t>{1, 2}, 5);
  CHECK(std::get<std::int64_t>(bound.params()[1]) == 5);
  CHECK_THROWS_AS(q::Prepared<IDS_LIMITED>::bind(std::vector<std::int64_t>{1}, -1), std::out_of_range);
}

TEST_CASE("bound arguments are sent as text, a list as one array", "[prepared][statements]") {
  const auto              bound = q::Prepared<IDS_LIMITED>::bind(std::vector<std::int64_t>{1, 2}, 5);
  const pg::detail::WireStatement wired = pg::detail::wire(bound);
  CHECK(wired.params == std::vector<std::string>{R"({"1","2"})", "5"});
  CHECK(wired.column_names == std::vector<std::string_view>{"id", "name", "nick", "score", "active"});
}

TEST_CASE("a raw statement binds its declared types", "[prepared][raw]") {
  using Lookup = q::RawStatement<"SELECT name AS name FROM people WHERE id = $1 AND score > $2", q::RawArguments<std::int64_t, double>, t::Name>;
  const auto                      bound = Lookup::bind(7, 1.5);
  const pg::detail::WireStatement wired = pg::detail::wire(bound);
  CHECK(wired.params == std::vector<std::string>{"7", "1.5"});
  CHECK(wired.column_names == std::vector<std::string_view>{"name"});
}

TEST_CASE("a field reads as its value", "[schema]") {
  const t::Name      name{"ann"};
  const std::string& text = name;  // through Field's conversion to `const data_type&`
  CHECK(text == "ann");
}
