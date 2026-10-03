// What a call returns depends on its last argument: a `std::future` of its outcome by default, nothing when given a completion
// (`util::CompletionFor`). And `select_by_key` is there only for a schema whose key is one column.

#include <catch2/catch_test_macros.hpp>

#include <concepts>
#include <cstdint>
#include <exception>
#include <future>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "schemacht/postgres/database.hpp"
#include "schemacht/postgres/statement_runner.hpp"
#include "schemacht/postgres/transaction.hpp"
#include "schemacht/query/command.hpp"
#include "schemacht/query/prepared.hpp"
#include "schemacht/query/query.hpp"
#include "schemacht/util/futures.hpp"
#include "support/schemas.hpp"

namespace pg = schemacht::postgres;
namespace q = schemacht::query;
namespace t = schemacht::test;
namespace util = schemacht::util;

namespace {

constexpr auto ALL_PEOPLE = q::select<t::People>();
constexpr auto NO_PEOPLE = q::delete_all<t::People>();

using Query = decltype(q::Prepared<ALL_PEOPLE>::bind());
using Command = decltype(q::Prepared<NO_PEOPLE>::bind());
using Rows = std::vector<t::People::row_type>;

struct RowsDone {
  void operator()(const Rows& /*rows*/, const std::exception_ptr& /*error*/) const {}
};

struct CountDone {
  void operator()(std::uint64_t /*count*/, const std::exception_ptr& /*error*/) const {}
};

struct ErrorDone {
  void operator()(const std::exception_ptr& /*error*/) const {}
};

struct OnRow {
  void operator()(const t::People::row_type& /*row*/) const {}
};

template <typename schema_t, typename key_t>
concept SelectsByKey = requires(pg::Database& database, std::span<const key_t> keys) { database.select_by_key<schema_t>(keys); };

}  // namespace

TEST_CASE("a completion is a future by default, or a callable taking the outcome and the error", "[completions]") {
  STATIC_REQUIRE(util::CompletionFor<util::UseFuture, Rows>);
  STATIC_REQUIRE(util::CompletionFor<RowsDone, Rows>);
  STATIC_REQUIRE(util::CompletionFor<CountDone, std::uint64_t>);
  STATIC_REQUIRE(util::CompletionFor<ErrorDone, void>);

  STATIC_REQUIRE_FALSE(util::CompletionFor<ErrorDone, Rows>);  // the outcome is missing
  STATIC_REQUIRE_FALSE(util::CompletionFor<RowsDone, void>);
  STATIC_REQUIRE_FALSE(util::CompletionFor<pg::ReadOptions, Rows>);  // what tells `execute(q, read)` from `execute(q, done)`
}

TEST_CASE("a call returns a future of its outcome, or nothing when given a completion", "[completions]") {
  STATIC_REQUIRE(std::same_as<decltype(std::declval<pg::Database&>().execute(std::declval<const Query&>())), std::future<Rows>>);
  STATIC_REQUIRE(std::same_as<decltype(std::declval<pg::Database&>().execute(std::declval<const Query&>(), RowsDone{})), void>);
  STATIC_REQUIRE(std::same_as<decltype(std::declval<pg::Database&>().execute(std::declval<const Command&>())), std::future<std::uint64_t>>);
  STATIC_REQUIRE(std::same_as<decltype(std::declval<pg::Database&>().execute(std::declval<const Command&>(), CountDone{})), void>);
  STATIC_REQUIRE(std::same_as<decltype(std::declval<pg::Database&>().stream(std::declval<const Query&>(), OnRow{})), std::future<void>>);
  STATIC_REQUIRE(std::same_as<decltype(std::declval<pg::Database&>().stream(std::declval<const Query&>(), OnRow{}, ErrorDone{})), void>);
  STATIC_REQUIRE(std::same_as<decltype(std::declval<pg::Database&>().insert<t::People>(std::declval<const Rows&>())), std::future<std::uint64_t>>);
  STATIC_REQUIRE(std::same_as<decltype(std::declval<pg::Database&>().begin()), std::future<pg::Transaction>>);
  STATIC_REQUIRE(std::same_as<decltype(std::declval<pg::Transaction&>().commit()), std::future<void>>);
  STATIC_REQUIRE(std::same_as<decltype(std::declval<pg::Transaction&>().rollback(ErrorDone{})), void>);
}

TEST_CASE("select_by_key is there only for a schema whose key is one column", "[completions]") {
  STATIC_REQUIRE(SelectsByKey<t::People, std::int64_t>);
  STATIC_REQUIRE_FALSE(SelectsByKey<t::Pairs, std::int32_t>);   // a key of two columns
  STATIC_REQUIRE_FALSE(SelectsByKey<t::Keyless, std::string>);  // no key
}
