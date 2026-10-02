#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "schemacht/postgres/statements.hpp"
#include "schemacht/query.hpp"
#include "support/schemas.hpp"

namespace q = schemacht::query;
namespace sch = schemacht::schema;
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
  const auto                      bound = q::Prepared<IDS_LIMITED>::bind(std::vector<std::int64_t>{1, 2}, 5);
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

TEST_CASE("a raw statement sends a list as one array, and bytea as bytes", "[prepared][raw]") {
  using ByNicks = q::RawStatement<"SELECT name AS name FROM people WHERE nick = ANY($1) AND data = $2",
                                  q::RawArguments<std::vector<std::string>, std::vector<std::byte>>, t::Name>;
  const auto bound = ByNicks::bind(std::vector<std::string>{"a", "b"}, std::vector<std::byte>{std::byte{0x01}, std::byte{0xff}});

  CHECK(pg::detail::wire(bound).params == std::vector<std::string>{R"({"a","b"})", R"(\x01ff)"});
}

TEST_CASE("a raw statement runs only if each of its arguments and columns has a text form", "[prepared][raw]") {
  struct Opaque {};
  using Fine = q::RawStatement<"SELECT $1::bigint AS id", q::RawArguments<std::int64_t>, t::Id>;
  using Unsendable = q::RawStatement<"SELECT $1::text AS name", q::RawArguments<Opaque>, t::Name>;
  using Unreadable = q::RawStatement<"SELECT 1 AS opaque", q::RawArguments<>, sch::Field<Opaque, "opaque">>;

  STATIC_REQUIRE(pg::RunnableStatement<q::BoundRawStatement<Fine>>);
  STATIC_REQUIRE_FALSE(pg::RunnableStatement<q::BoundRawStatement<Unsendable>>);
  STATIC_REQUIRE_FALSE(pg::RunnableStatement<q::BoundRawStatement<Unreadable>>);
}

TEST_CASE("a schema is used only if each of its columns has a text form", "[prepared][schema]") {
  struct Opaque {};
  using Opaques = sch::Schema<"opaques", t::Id, sch::Field<Opaque, "opaque">>;

  STATIC_REQUIRE(pg::TextSchema<t::People>);
  STATIC_REQUIRE_FALSE(pg::TextSchema<Opaques>);
  STATIC_REQUIRE_FALSE(pg::TextSchema<int>);
}

TEST_CASE("a field reads as its value", "[schema]") {
  const t::Name      name{"ann"};
  const std::string& text = name;  // through Field's conversion to `const data_type&`

  CHECK(text == "ann");
}

TEST_CASE("a raw statement with no columns is a command", "[prepared][raw]") {
  using Touch = q::RawStatement<"UPDATE people SET score = $1", q::RawArguments<double>>;
  using Read = q::RawStatement<"SELECT 1::bigint AS id", q::RawArguments<>, t::Id>;

  STATIC_REQUIRE(pg::CommandStatement<q::BoundRawStatement<Touch>>);
  STATIC_REQUIRE_FALSE(pg::RunnableStatement<q::BoundRawStatement<Touch>>);
  STATIC_REQUIRE(pg::RunnableStatement<q::BoundRawStatement<Read>>);
  STATIC_REQUIRE_FALSE(pg::CommandStatement<q::BoundRawStatement<Read>>);

  CHECK(pg::detail::wire(Touch::bind(1.5)).column_names.empty());
}
