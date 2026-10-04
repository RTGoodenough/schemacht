// The public helpers in schemacht/util: hex, all_distinct, SQL function names, and completions (settle, future_from, complete).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "schemacht/util/compile_time.hpp"
#include "schemacht/util/futures.hpp"
#include "schemacht/util/hex.hpp"
#include "schemacht/util/identifier.hpp"

namespace util = schemacht::util;

TEST_CASE("hex is read in either case and written in lowercase", "[util][hex]") {
  const std::vector<std::byte> bytes{std::byte{0x00}, std::byte{0x0a}, std::byte{0xff}, std::byte{0x7e}};

  CHECK(util::format_hex(bytes) == "000aff7e");
  CHECK(util::parse_hex("000aff7e") == bytes);
  CHECK(util::parse_hex("000AfF7E") == bytes);

  CHECK(util::parse_hex("").empty());
  CHECK(util::format_hex(std::span<const std::byte>{}).empty());

  // A fixed array, or part of one, without a copy.
  const std::array<std::byte, 3> fixed{std::byte{0x01}, std::byte{0x23}, std::byte{0x45}};
  CHECK(util::format_hex(fixed) == "012345");
  CHECK(util::format_hex(std::span(fixed).subspan(1)) == "2345");

  // At compile time too.
  STATIC_REQUIRE(util::format_hex(std::array{std::byte{0xbe}, std::byte{0xef}}) == "beef");
  STATIC_REQUIRE(util::parse_hex("BeEf").size() == 2);
}

TEST_CASE("hex is refused for an odd length, a non-digit, or the wrong size", "[util][hex]") {
  CHECK_THROWS_AS(util::parse_hex("abc"), std::invalid_argument);
  CHECK_THROWS_AS(util::parse_hex("0g"), std::invalid_argument);
  CHECK_THROWS_AS(util::parse_hex("0 "), std::invalid_argument);

  std::array<std::byte, 2> out{};
  util::parse_hex_into("cafe", out);
  CHECK(out == std::array{std::byte{0xca}, std::byte{0xfe}});

  CHECK_THROWS_WITH(util::parse_hex_into("caf", out), "hex: 3 digits for 2 bytes, which take 4");
  CHECK_THROWS_AS(util::parse_hex_into("cafeba", out), std::invalid_argument);
  CHECK_THROWS_WITH(util::parse_hex("00ff0z"), "hex: the character at index 5 is not a hex digit");
}

TEST_CASE("all_distinct finds a value given twice", "[util]") {
  STATIC_REQUIRE(util::all_distinct(std::array<std::string_view, 3>{"a", "b", "c"}));
  STATIC_REQUIRE_FALSE(util::all_distinct(std::array<std::string_view, 3>{"a", "b", "a"}));
  STATIC_REQUIRE(util::all_distinct(std::array<int, 0>{}));

  // Only the part in use: the repeat beyond it does not count.
  static constexpr std::array<std::size_t, 4> COLUMNS{2, 0, 2, 2};
  STATIC_REQUIRE(util::all_distinct(std::span(COLUMNS).first(2)));
  STATIC_REQUIRE_FALSE(util::all_distinct(std::span(COLUMNS).first(3)));

  // A view walked only when not const is refused by the concept, not deep inside the body.
  using Filtered = decltype(COLUMNS | std::views::filter([](std::size_t column) { return column != 0; }));
  STATIC_REQUIRE_FALSE(util::EqualityComparableRange<Filtered>);
}

TEST_CASE("a SQL function name may be named with its schema, and nothing else", "[util]") {
  STATIC_REQUIRE(util::is_sql_function_name("similarity"));
  STATIC_REQUIRE(util::is_sql_function_name("ST_AsBinary"));
  STATIC_REQUIRE(util::is_sql_function_name("miniverse.merge_raster"));

  STATIC_REQUIRE_FALSE(util::is_sql_function_name(""));
  STATIC_REQUIRE_FALSE(util::is_sql_function_name("1st"));
  STATIC_REQUIRE_FALSE(util::is_sql_function_name("gis.1st"));
  STATIC_REQUIRE_FALSE(util::is_sql_function_name("a.b.c"));
  STATIC_REQUIRE_FALSE(util::is_sql_function_name(".merge"));
  STATIC_REQUIRE_FALSE(util::is_sql_function_name("gis."));
  STATIC_REQUIRE_FALSE(util::is_sql_function_name(R"("gis".merge)"));
  STATIC_REQUIRE_FALSE(util::is_sql_function_name("f(); DROP TABLE t; --"));
}

TEST_CASE("settle completes a promise with the error, or with the value", "[util]") {
  std::promise<void> done;
  util::settle(done, nullptr);
  CHECK_NOTHROW(done.get_future().get());

  std::promise<void> failed;
  util::settle(failed, std::make_exception_ptr(std::runtime_error("no")));
  CHECK_THROWS_AS(failed.get_future().get(), std::runtime_error);

  std::promise<std::int64_t> counted;
  util::settle(counted, nullptr, 3);  // the value takes the promise's type
  CHECK(counted.get_future().get() == 3);

  // A reference can't be moved into a promise: refused by the concept, not deep inside `settle`.
  STATIC_REQUIRE_FALSE(util::PromiseValue<std::string&>);

  std::promise<std::string> refused;
  util::settle(refused, std::make_exception_ptr(std::runtime_error("no")), std::string("unused"));
  CHECK_THROWS_AS(refused.get_future().get(), std::runtime_error);
}

TEST_CASE("future_from makes a future of a call that takes a completion", "[util]") {
  std::future<std::int64_t> counted = util::future_from<std::int64_t>([](const util::Settler<std::int64_t>& done) { done(3, nullptr); });
  CHECK(counted.get() == 3);

  std::future<void> failed =
      util::future_from<void>([](const util::Settler<void>& done) { done(std::make_exception_ptr(std::runtime_error("no"))); });
  CHECK_THROWS_AS(failed.get(), std::runtime_error);
}

TEST_CASE("a settler fails its future with an error alone, with no value to pass", "[util]") {
  struct NoDefault {  // no value to make up for the error path
    explicit NoDefault(int value) : value(value) {}
    int value;
  };

  std::future<NoDefault> refused =
      util::future_from<NoDefault>([](const util::Settler<NoDefault>& done) { done.fail(std::make_exception_ptr(std::runtime_error("no"))); });
  CHECK_THROWS_AS(refused.get(), std::runtime_error);

  std::future<void> failed =
      util::future_from<void>([](const util::Settler<void>& done) { done.fail(std::make_exception_ptr(std::runtime_error("no"))); });
  CHECK_THROWS_AS(failed.get(), std::runtime_error);
}

TEST_CASE("complete gives a future for UseFuture, and hands any other completion on", "[util]") {
  std::future<std::int64_t> counted =
      util::complete<std::int64_t>(util::UseFuture{}, [](const util::Settler<std::int64_t>& done) { done(3, nullptr); });
  CHECK(counted.get() == 3);

  std::int64_t got = 0;
  util::complete<std::int64_t>([&got](std::int64_t value, const std::exception_ptr& /*error*/) { got = value; }, [](auto done) { done(4, nullptr); });
  CHECK(got == 4);
}
