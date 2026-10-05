// A statement written against a schema is aimed at a table of that schema's shape, named while the program runs (`on`),
// whatever the schema's key: one column, several, or none. A table of another schema is refused, and a raw statement names
// its own tables, so it can't be aimed at all.

#include <catch2/catch_test_macros.hpp>

#include <concepts>
#include <utility>
#include <vector>

#include "schemacht/postgres/statements.hpp"
#include "schemacht/query/prepared.hpp"
#include "schemacht/query/query.hpp"
#include "schemacht/query/raw_statement.hpp"
#include "schemacht/schema/table.hpp"
#include "support/schemas.hpp"

namespace pg = schemacht::postgres;
namespace q = schemacht::query;
namespace sch = schemacht::schema;
namespace t = schemacht::test;

namespace {

template <typename statement_t, typename table_t>
concept AimsAt = requires(const statement_t& statement, const table_t& table) {
  { statement.on(table) } -> std::same_as<statement_t>;
};

constexpr auto ALL_PEOPLE = q::select<t::People>();
constexpr auto ALL_PAIRS = q::select<t::Pairs>();

using PeopleQuery = decltype(q::Prepared<ALL_PEOPLE>::bind());
using PairsQuery = decltype(q::Prepared<ALL_PAIRS>::bind());
using PairsInsert = decltype(pg::insert_statement<t::Pairs>(std::declval<const std::vector<t::Pairs::row_type>&>()));
using KeylessCreate = decltype(pg::create_table_statement<t::Keyless>());
using Raw = decltype(q::RawStatement<"SELECT 1 AS id", q::RawArguments<>, t::Id>::bind());

}  // namespace

TEST_CASE("a statement is aimed only at a table of its own schema's shape, whatever its key", "[table]") {
  STATIC_REQUIRE(AimsAt<PeopleQuery, sch::Table<t::People>>);
  STATIC_REQUIRE(AimsAt<PairsQuery, sch::Table<t::Pairs>>);  // a key of two columns
  STATIC_REQUIRE(AimsAt<PairsInsert, sch::Table<t::Pairs>>);
  STATIC_REQUIRE(AimsAt<KeylessCreate, sch::Table<t::Keyless>>);  // no key

  STATIC_REQUIRE_FALSE(AimsAt<PeopleQuery, sch::Table<t::Pairs>>);
  STATIC_REQUIRE_FALSE(AimsAt<PairsInsert, sch::Table<t::Keyless>>);
  STATIC_REQUIRE_FALSE(AimsAt<Raw, sch::Table<t::People>>);
}
