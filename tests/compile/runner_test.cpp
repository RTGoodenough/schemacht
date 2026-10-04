// What a call returns depends on its last argument: a `std::future` of its outcome by default, nothing when given a completion
// (`util::CompletionFor`). And `select_by_key` is there only for a schema whose key is one column.

#include <catch2/catch_test_macros.hpp>

#include <concepts>
#include <cstdint>
#include <exception>
#include <future>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "schemacht/postgres/database.hpp"
#include "schemacht/postgres/row_reader.hpp"
#include "schemacht/postgres/statement_runner.hpp"
#include "schemacht/postgres/statements.hpp"
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

struct Add {
  static constexpr std::string_view OPERATOR = "+";
};

struct OnRow {
  void operator()(const t::People::row_type& /*row*/) const {}
};

struct OnChunk {
  bool operator()(const std::vector<t::People::row_type>& /*rows*/) const { return true; }
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
  STATIC_REQUIRE(std::same_as<decltype(std::declval<pg::Database&>().stream(std::declval<Query>(), OnRow{})), std::future<void>>);  // a temporary
  STATIC_REQUIRE(std::same_as<decltype(std::declval<pg::Database&>().stream_chunks(std::declval<const Query&>(), OnChunk{})), std::future<void>>);
  STATIC_REQUIRE(
      std::same_as<decltype(std::declval<pg::Transaction&>().stream_chunks(std::declval<Query>(), OnChunk{}, pg::ReadOptions{}, ErrorDone{})), void>
  );
  STATIC_REQUIRE(std::same_as<decltype(std::declval<pg::Database&>().insert<t::People>(std::declval<const Rows&>())), std::future<std::uint64_t>>);
  STATIC_REQUIRE(std::same_as<decltype(std::declval<pg::Database&>().upsert<t::People>(std::declval<const Rows&>())), std::future<std::uint64_t>>);
  STATIC_REQUIRE(
      std::same_as<
          decltype(std::declval<pg::Transaction&>().upsert<t::People, pg::MergeWith<"score", Add>>(std::declval<const Rows&>(), CountDone{})), void>
  );
  STATIC_REQUIRE(
      std::same_as<
          decltype(std::declval<pg::Database&>().upsert<t::WithGenerated>(std::declval<const std::vector<t::WithGenerated::insert_row_type>&>())),
          std::future<std::uint64_t>>
  );  // rows without their generated columns
  STATIC_REQUIRE(std::same_as<decltype(std::declval<pg::Database&>().execute(pg::unchecked_sql("VACUUM"))), std::future<std::uint64_t>>);
  STATIC_REQUIRE(std::same_as<decltype(std::declval<pg::Transaction&>().execute(pg::unchecked_sql("VACUUM"), CountDone{})), void>);
  STATIC_REQUIRE(std::same_as<decltype(std::declval<pg::Database&>().begin()), std::future<pg::Transaction>>);
  STATIC_REQUIRE(std::same_as<decltype(std::declval<pg::Transaction&>().commit()), std::future<void>>);
  STATIC_REQUIRE(std::same_as<decltype(std::declval<pg::Transaction&>().rollback(ErrorDone{})), void>);
}

TEST_CASE("select_by_key is there only for a schema whose key is one column", "[completions]") {
  STATIC_REQUIRE(SelectsByKey<t::People, std::int64_t>);
  STATIC_REQUIRE_FALSE(SelectsByKey<t::Pairs, std::int32_t>);   // a key of two columns
  STATIC_REQUIRE_FALSE(SelectsByKey<t::Keyless, std::string>);  // no key
}

TEST_CASE("a stream's handler takes a row, and a chunk stream's a vector of them", "[completions]") {
  STATIC_REQUIRE(pg::RowCallback<OnRow, t::People::row_type>);
  STATIC_REQUIRE(pg::ChunkCallback<OnChunk, t::People::row_type>);

  STATIC_REQUIRE_FALSE(pg::ChunkCallback<OnRow, t::People::row_type>);
  STATIC_REQUIRE_FALSE(pg::RowCallback<OnChunk, t::People::row_type>);
  STATIC_REQUIRE_FALSE(pg::ChunkCallback<RowsDone, t::People::row_type>);  // it takes an error too

  STATIC_REQUIRE(std::same_as<decltype(std::declval<const pg::Database&>().read_options()), const pg::ReadOptions&>);
  STATIC_REQUIRE(std::same_as<decltype(std::declval<const pg::Transaction&>().read_options()), const pg::ReadOptions&>);
}
