// A `Table` of any schema compiles, whatever its key: one column, several, or none. Only what needs a one-column key
// (`select_by_key`) is missing where there is none.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <exception>
#include <span>
#include <string>
#include <vector>

#include "schemacht/postgres/table.hpp"
#include "support/schemas.hpp"

namespace pg = schemacht::postgres;
namespace t = schemacht::test;

// Explicit instantiation forms every member that is not a template, and the declaration of every one that is: a signature
// that can't be formed for the schema (as `select_by_key`'s once couldn't, without a one-column key) is a compile error
// here. Member templates' bodies are formed only when called, which the checks below do in part.
template class schemacht::postgres::Table<t::People>;
template class schemacht::postgres::Table<t::Pairs>;
template class schemacht::postgres::Table<t::Keyless>;
template class schemacht::postgres::Table<t::Numbered>;

namespace {

template <typename table_t, typename key_t>
concept SelectsByKey = requires(table_t& table, std::span<const key_t> keys) { table.select_by_key(keys); };

template <typename table_t>
concept InsertsWithCompletion = requires(table_t& table, const std::vector<typename table_t::row_type>& rows) {
  table.insert_into(rows, [](const std::exception_ptr& /*error*/) {});
};

}  // namespace

TEST_CASE("a Table compiles for any key, and selects by key only for a one-column one", "[table]") {
  STATIC_REQUIRE(SelectsByKey<pg::Table<t::People>, std::int64_t>);
  STATIC_REQUIRE_FALSE(SelectsByKey<pg::Table<t::Pairs>, std::int32_t>);
  STATIC_REQUIRE_FALSE(SelectsByKey<pg::Table<t::Keyless>, std::string>);

  STATIC_REQUIRE(InsertsWithCompletion<pg::Table<t::People>>);
  STATIC_REQUIRE(InsertsWithCompletion<pg::Table<t::Pairs>>);
  STATIC_REQUIRE(InsertsWithCompletion<pg::Table<t::Keyless>>);
}
