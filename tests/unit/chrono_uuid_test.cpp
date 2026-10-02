#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ratio>
#include <stdexcept>
#include <vector>

#include "schemacht/postgres/field_codec.hpp"
#include "schemacht/query/sql_type.hpp"
#include "schemacht/uuid.hpp"

namespace pg = schemacht::postgres;
namespace sql = schemacht::query::sql;

using namespace std::chrono_literals;
using std::chrono::year_month_day;

namespace {

using Micros = std::chrono::sys_time<std::chrono::microseconds>;
using Nanos = std::chrono::sys_time<std::chrono::nanoseconds>;
using Local = std::chrono::local_time<std::chrono::microseconds>;

/** @return `value` big endian in `count` bytes, as PostgreSQL sends a number in binary form. */
template <std::size_t count>
std::vector<std::byte> big_endian(std::int64_t value) {
  std::vector<std::byte> bytes(count);
  auto                   bits = std::bit_cast<std::uint64_t>(value);
  for ( std::size_t i = count; i-- > 0; ) {
    bytes.at(i) = static_cast<std::byte>(bits & 0xFFU);
    bits >>= 8U;
  }
  return bytes;
}

Micros at(std::chrono::sys_days day, std::chrono::microseconds time_of_day = {}) { return Micros{day} + time_of_day; }

}  // namespace

TEST_CASE("which durations hold every microsecond", "[chrono]") {
  STATIC_REQUIRE(pg::MicrosecondExact<std::chrono::microseconds>);
  STATIC_REQUIRE(pg::MicrosecondExact<std::chrono::nanoseconds>);
  STATIC_REQUIRE(pg::MicrosecondExact<std::chrono::system_clock::duration>);
  STATIC_REQUIRE_FALSE(pg::MicrosecondExact<std::chrono::seconds>);
  STATIC_REQUIRE_FALSE(pg::MicrosecondExact<std::chrono::duration<double, std::micro>>);
  STATIC_REQUIRE(pg::HasTextForm<std::chrono::system_clock::time_point>);
  STATIC_REQUIRE(pg::HasBinaryForm<Micros>);
  STATIC_REQUIRE_FALSE(pg::HasTextForm<std::chrono::sys_seconds>);  // a second is coarser than PostgreSQL keeps

  STATIC_REQUIRE(sql::type_name<Micros>() == "timestamptz");
  STATIC_REQUIRE(sql::type_name<Local>() == "timestamp");
  STATIC_REQUIRE(sql::type_name<year_month_day>() == "date");
  STATIC_REQUIRE(sql::type_name<schemacht::Uuid>() == "uuid");
}

TEST_CASE("a timestamptz in text", "[chrono]") {
  using Codec = pg::FieldCodec<Micros>;
  CHECK(Codec::format(at(std::chrono::sys_days{2000y / 1 / 1})) == "2000-01-01 00:00:00.000000+00");
  CHECK(Codec::format(at(std::chrono::sys_days{1970y / 1 / 1}, -1us)) == "1969-12-31 23:59:59.999999+00");
  CHECK(Codec::parse("2026-10-01 20:55:12.264-04") == at(std::chrono::sys_days{2026y / 10 / 2}, 55min + 12s + 264ms));
  CHECK(Codec::parse("2026-01-01 05:30:00+05:30") == at(std::chrono::sys_days{2026y / 1 / 1}));
  CHECK(Codec::parse("2026-01-01 00:00:00+00") == at(std::chrono::sys_days{2026y / 1 / 1}));

  const Micros caesar = at(std::chrono::sys_days{std::chrono::year{-43} / std::chrono::March / 15});  // 44 BC is year -43
  CHECK(Codec::format(caesar) == "0044-03-15 00:00:00.000000+00 BC");
  CHECK(Codec::parse("0044-03-15 00:00:00+00 BC") == caesar);

  CHECK(Codec::parse("infinity") == Micros::max());
  CHECK(Codec::parse("-infinity") == Micros::min());
  CHECK(Codec::format(Micros::max()) == "infinity");

  CHECK_THROWS_AS(Codec::parse("2026-10-01 20:55:12"), std::invalid_argument);  // no zone
  CHECK_THROWS_AS(Codec::parse("2026-02-30 00:00:00+00"), std::invalid_argument);
  CHECK_THROWS_AS(Codec::parse("yesterday"), std::invalid_argument);
}

TEST_CASE("a time finer than a microsecond is truncated toward the past", "[chrono]") {
  using Codec = pg::FieldCodec<Nanos>;
  CHECK(Codec::format(Nanos{1500ns}) == "1970-01-01 00:00:00.000001+00");
  CHECK(Codec::format(Nanos{-1ns}) == "1969-12-31 23:59:59.999999+00");
  CHECK(Codec::parse("1970-01-01 00:00:00.000001+00") == Nanos{1us});
  CHECK_THROWS_AS(Codec::parse("9999-01-01 00:00:00+00"), std::invalid_argument);  // past what 64-bit nanoseconds hold
}

TEST_CASE("a timestamptz in binary", "[chrono]") {
  using Codec = pg::FieldCodec<Micros>;
  CHECK(Codec::parse_binary(big_endian<8>(0)) == at(std::chrono::sys_days{2000y / 1 / 1}));
  CHECK(Codec::parse_binary(big_endian<8>(-1)) == at(std::chrono::sys_days{2000y / 1 / 1}, -1us));
  CHECK(Codec::parse_binary(big_endian<8>(std::numeric_limits<std::int64_t>::max())) == Micros::max());
  CHECK_THROWS_AS(Codec::parse_binary(big_endian<4>(0)), std::invalid_argument);
  CHECK(Codec::binary_accepts(pg::detail::OID_TIMESTAMPTZ));
  CHECK_FALSE(Codec::binary_accepts(pg::detail::OID_TIMESTAMP));
}

TEST_CASE("a timestamp has no zone", "[chrono]") {
  using Codec = pg::FieldCodec<Local>;
  const Local noon = Local{std::chrono::local_days{2026y / 10 / 1}} + 12h;
  CHECK(Codec::format(noon) == "2026-10-01 12:00:00.000000");
  CHECK(Codec::parse("2026-10-01 12:00:00") == noon);
  CHECK_THROWS_AS(Codec::parse("2026-10-01 12:00:00+00"), std::invalid_argument);
  CHECK(Codec::binary_accepts(pg::detail::OID_TIMESTAMP));
}

TEST_CASE("a date", "[chrono]") {
  using Codec = pg::FieldCodec<year_month_day>;
  CHECK(Codec::format(2026y / 10 / 1) == "2026-10-01");
  CHECK(Codec::parse("2026-10-01") == 2026y / 10 / 1);
  CHECK(Codec::parse("0001-01-01 BC") == year_month_day{std::chrono::year{0}, std::chrono::January, std::chrono::day{1}});
  CHECK(Codec::format(year_month_day{std::chrono::year{0}, std::chrono::January, std::chrono::day{1}}) == "0001-01-01 BC");
  CHECK(Codec::parse_binary(big_endian<4>(1)) == 2000y / 1 / 2);
  CHECK(Codec::parse("infinity") == year_month_day{std::chrono::year::max(), std::chrono::December, std::chrono::day{31}});
  CHECK(Codec::format(year_month_day{std::chrono::year::min(), std::chrono::January, std::chrono::day{1}}) == "-infinity");
  CHECK_THROWS_AS(Codec::format(2026y / 2 / 30), std::invalid_argument);
  CHECK_THROWS_AS(Codec::parse("2026-13-01"), std::invalid_argument);
  CHECK_THROWS_AS(Codec::parse("40000-01-01"), std::invalid_argument);  // PostgreSQL has it; std::chrono::year does not
}

TEST_CASE("a UUID", "[uuid]") {
  using schemacht::Uuid;
  const Uuid parsed = Uuid::from_string("0e9cd3a8-3f43-4f4e-9b5c-6b6f8f6f0a1d");
  CHECK(parsed.to_string() == "0e9cd3a8-3f43-4f4e-9b5c-6b6f8f6f0a1d");
  CHECK(Uuid::from_string("0E9CD3A8-3F43-4F4E-9B5C-6B6F8F6F0A1D") == parsed);
  CHECK(parsed.bytes().at(0) == std::byte{0x0e});
  CHECK(Uuid{}.to_string() == "00000000-0000-0000-0000-000000000000");
  CHECK(Uuid{} < parsed);
  CHECK_THROWS_AS(Uuid::from_string("0e9cd3a83f434f4e9b5c6b6f8f6f0a1d"), std::invalid_argument);
  CHECK_THROWS_AS(Uuid::from_string("0e9cd3a8-3f43-4f4e-9b5c-6b6f8f6f0a1g"), std::invalid_argument);

  STATIC_REQUIRE(Uuid::from_string("00000000-0000-0000-0000-000000000001").bytes().at(15) == std::byte{1});  // at compile time too

  using Codec = pg::FieldCodec<Uuid>;
  CHECK(Codec::parse_binary(parsed.bytes()) == parsed);
  CHECK_THROWS_AS(Codec::parse_binary(std::array<std::byte, 4>{}), std::invalid_argument);
  CHECK(Codec::binary_accepts(2950));
}
