// The raw statement placeholder scanner, checked while compiling.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>

#include "schemacht/query/raw_statement.hpp"

namespace q = schemacht::query;

namespace {

constexpr q::PlaceholderCheck OK{};

constexpr q::PlaceholderCheck problem(q::PlaceholderProblem kind, std::size_t number = 0) { return {.problem = kind, .number = number}; }

}  // namespace

TEST_CASE("placeholders are matched against the declared arguments", "[raw]") {
  STATIC_REQUIRE(q::check_placeholders("SELECT $1, $2, $1", 2) == OK);
  STATIC_REQUIRE(q::check_placeholders("SELECT 1", 0) == OK);
  STATIC_REQUIRE(q::check_placeholders("SELECT $1", 2) == problem(q::PlaceholderProblem::Unused, 2));
  STATIC_REQUIRE(q::check_placeholders("SELECT $3", 2) == problem(q::PlaceholderProblem::Beyond, 3));
  STATIC_REQUIRE(q::check_placeholders("SELECT $0", 1) == problem(q::PlaceholderProblem::Beyond, 0));
}

TEST_CASE("strings, quoted names and comments are not code", "[raw]") {
  STATIC_REQUIRE(q::check_placeholders("SELECT '$2', $1", 1) == OK);
  STATIC_REQUIRE(q::check_placeholders("SELECT 'it''s $2', $1", 1) == OK);
  STATIC_REQUIRE(q::check_placeholders(R"(SELECT E'\'$2', $1)", 1) == OK);
  STATIC_REQUIRE(q::check_placeholders(R"(SELECT "a$2", $1)", 1) == OK);
  STATIC_REQUIRE(q::check_placeholders("SELECT $1 -- $2", 1) == OK);
  STATIC_REQUIRE(q::check_placeholders("/* /* $2 */ */ SELECT $1", 1) == OK);
  STATIC_REQUIRE(q::check_placeholders("SELECT a$2, $1", 1) == OK);  // `$` inside a name
}

TEST_CASE("what the scanner refuses", "[raw]") {
  STATIC_REQUIRE(q::check_placeholders("SELECT $$text$$", 0) == problem(q::PlaceholderProblem::DollarQuote));
  STATIC_REQUIRE(q::check_placeholders("SELECT 'open", 0) == problem(q::PlaceholderProblem::Unterminated));
  STATIC_REQUIRE(q::check_placeholders("SELECT /* open", 0) == problem(q::PlaceholderProblem::Unterminated));
}
