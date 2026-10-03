// No error escapes a call that takes a completion: each is noexcept, on a database, a transaction and a table alike, also
// for a completion or row handler whose move could throw (the call reports that as an error, through the completion).

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <exception>
#include <span>
#include <utility>
#include <vector>

#include "schemacht/schemacht.hpp"
#include "support/schemas.hpp"

namespace q = schemacht::query;
namespace pg = schemacht::postgres;
namespace t = schemacht::test;

namespace {

constexpr auto ALL_PEOPLE = q::select<t::People>();
constexpr auto NO_PEOPLE = q::delete_all<t::People>();

using Query = decltype(q::Prepared<ALL_PEOPLE>::bind());
using Command = decltype(q::Prepared<NO_PEOPLE>::bind());
using Row = t::People::row_type;
using Rows = std::vector<Row>;
using NumberedRows = std::vector<t::Numbered::insert_row_type>;
using Table = pg::Table<t::People>;

struct ErrorDone {
  void operator()(const std::exception_ptr& /*error*/) const {}
};

struct RowsDone {
  void operator()(const std::vector<Row>& /*rows*/, const std::exception_ptr& /*error*/) const {}
};

struct CountDone {
  void operator()(std::uint64_t /*count*/, const std::exception_ptr& /*error*/) const {}
};

struct TransactionDone {
  void operator()(const pg::Transaction& /*transaction*/, const std::exception_ptr& /*error*/) const {}
};

struct OnRow {
  void operator()(const Row& /*row*/) const {}
};

/** @brief A completion that could throw while it is moved. */
struct ThrowingMove {
  ThrowingMove() = default;
  ThrowingMove(const ThrowingMove& /*other*/) = default;
  ThrowingMove(ThrowingMove&& /*other*/) noexcept(false) {}  // NOLINT(bugprone-unsafe-to-allow-exceptions) -- the point of it
  ThrowingMove& operator=(const ThrowingMove& /*other*/) = default;
  // NOLINTNEXTLINE(bugprone-unsafe-to-allow-exceptions) -- as above
  ThrowingMove& operator=(ThrowingMove&& /*other*/) noexcept(false) { return *this; }
  ~ThrowingMove() = default;

  void operator()(const std::exception_ptr& /*error*/) const {}
};

/** @brief A row handler that could throw while it is moved. */
struct ThrowingMoveHandler : ThrowingMove {
  using ThrowingMove::operator();
  void operator()(const Row& /*row*/) const {}
};

/** @brief A completion that can be called only as an lvalue. */
struct LvalueOnly {
  void operator()(const std::exception_ptr& /*error*/) & {}
};

}  // namespace

TEST_CASE("every call that takes a completion is noexcept", "[completions]") {
  STATIC_REQUIRE(noexcept(std::declval<pg::Database&>().execute_into(std::declval<const Query&>(), RowsDone{})));
  STATIC_REQUIRE(noexcept(std::declval<pg::Database&>().execute_into(std::declval<const Command&>(), CountDone{})));
  STATIC_REQUIRE(noexcept(std::declval<pg::Database&>().stream_into(std::declval<const Query&>(), OnRow{}, ErrorDone{})));
  STATIC_REQUIRE(noexcept(std::declval<pg::Database&>().insert_into<t::People>(std::declval<const Rows&>(), ErrorDone{})));
  STATIC_REQUIRE(noexcept(std::declval<pg::Database&>().insert_into<t::People>(std::declval<std::span<const Row>>(), ErrorDone{})));
  STATIC_REQUIRE(noexcept(std::declval<pg::Database&>().insert_into<t::Numbered>(std::declval<const NumberedRows&>(), ErrorDone{})));
  STATIC_REQUIRE(noexcept(std::declval<pg::Database&>().begin_into(TransactionDone{})));

  STATIC_REQUIRE(noexcept(std::declval<pg::Transaction&>().execute_into(std::declval<const Query&>(), RowsDone{})));
  STATIC_REQUIRE(noexcept(std::declval<pg::Transaction&>().insert_into<t::People>(std::declval<const Rows&>(), ErrorDone{})));
  STATIC_REQUIRE(noexcept(std::declval<pg::Transaction&>().commit_into(ErrorDone{})));
  STATIC_REQUIRE(noexcept(std::declval<pg::Transaction&>().rollback_into(ErrorDone{})));

  STATIC_REQUIRE(noexcept(std::declval<Table&>().execute_into(std::declval<const Query&>(), RowsDone{})));
  STATIC_REQUIRE(noexcept(std::declval<Table&>().execute_into(std::declval<const Command&>(), CountDone{})));
  STATIC_REQUIRE(noexcept(std::declval<Table&>().stream_into(std::declval<const Query&>(), OnRow{}, ErrorDone{})));
  STATIC_REQUIRE(noexcept(std::declval<Table&>().insert_into(std::declval<const Rows&>(), ErrorDone{})));
  STATIC_REQUIRE(noexcept(std::declval<Table&>().insert_into(std::declval<std::span<const Row>>(), ErrorDone{})));
}

TEST_CASE("a completion whose move could throw, or callable only as an lvalue, is taken all the same", "[completions]") {
  STATIC_REQUIRE(noexcept(std::declval<pg::Database&>().insert_into<t::People>(std::declval<const Rows&>(), ThrowingMove{})));
  STATIC_REQUIRE(noexcept(std::declval<pg::Database&>().insert_into<t::People>(std::declval<const Rows&>(), LvalueOnly{})));
  STATIC_REQUIRE(noexcept(std::declval<pg::Database&>().stream_into(std::declval<const Query&>(), ThrowingMoveHandler{}, ThrowingMove{})));
  STATIC_REQUIRE(noexcept(std::declval<Table&>().stream_into(std::declval<const Query&>(), ThrowingMoveHandler{}, LvalueOnly{})));
}
