// Each block must fail to compile, with the message tests/CMakeLists.txt expects for it. Compiled one case at a time.

#include <cstdint>
#include <string>
#include <string_view>

#include "schemacht/query.hpp"

namespace sch = schemacht::schema;
namespace q = schemacht::query;

using Id = sch::Field<std::int64_t, "id", sch::KeyRole::Primary>;
using Name = sch::Field<std::string, "name">;
using Count = sch::Field<std::uint32_t, "count">;
using Ratio = sch::Field<float, "ratio">;
using Things = sch::Schema<"things", Id, Name, Count, Ratio>;

#if defined(CASE_BAD_TABLE_NAME)
using Bad = sch::Schema<"Things", Id>;
[[maybe_unused]] Bad::row_type row;
#endif

#if defined(CASE_BAD_COLUMN_NAME)
using Bad = sch::Field<int, "first name">;
[[maybe_unused]] Bad field;
#endif

#if defined(CASE_DUPLICATE_COLUMN)
using Bad = sch::Schema<"things", sch::Field<int, "a">, sch::Field<long, "a">>;
[[maybe_unused]] Bad::row_type row;
#endif

#if defined(CASE_UINT64_FIELD)
using Bad = sch::Field<std::uint64_t, "big">;
[[maybe_unused]] Bad field;
#endif

#if defined(CASE_UNKNOWN_COL)
constexpr auto BAD = q::On<Things>::col<"nope">() == q::arg<0>();
#endif

#if defined(CASE_UNKNOWN_FIELD_BY_NAME)
using Bad = Things::field_by_name<"nope">;
[[maybe_unused]] Bad field;
#endif

#if defined(CASE_NARROWING_BIND)
constexpr auto BY_RATIO = q::On<Things>::col<"ratio">() == q::arg<0>();
[[maybe_unused]] auto bound = q::Prepared<BY_RATIO>::bind(1.5);  // a double, for a float column
#endif

#if defined(CASE_NEGATIVE_FOR_UNSIGNED_BIND)
constexpr auto BY_COUNT = q::On<Things>::col<"count">() == q::arg<0>();
[[maybe_unused]] auto bound = q::Prepared<BY_COUNT>::bind(-1);
#endif

#if defined(CASE_WRONG_ARGUMENT_COUNT)
constexpr auto BY_ID = q::On<Things>::col<"id">() == q::arg<0>();
[[maybe_unused]] auto bound = q::Prepared<BY_ID>::bind(1, 2);
#endif

#if defined(CASE_BAD_OPERATOR)
struct Commented {
  static constexpr std::string_view OPERATOR = "--";
};
constexpr auto BAD = q::On<Things>::col<"name">().apply<Commented>(q::arg<0>());
#endif

#if defined(CASE_BAD_FUNCTION)
struct Injected {
  static constexpr std::string_view FUNCTION = "f(); DROP TABLE things; --";
};
constexpr auto BAD = q::On<Things>::col<"name">().apply<Injected>(q::arg<0>());
#endif

#if defined(CASE_RAW_UNUSED_ARGUMENT)
using Bad = q::RawStatement<"SELECT 1 AS n", q::RawArguments<int>, sch::Field<int, "n">>;
[[maybe_unused]] auto bound = Bad::bind(1);
#endif

#if defined(CASE_RAW_PLACEHOLDER_BEYOND)
using Bad = q::RawStatement<"SELECT $2 AS n", q::RawArguments<int>, sch::Field<int, "n">>;
[[maybe_unused]] auto bound = Bad::bind(1);
#endif

#if defined(CASE_RAW_DOLLAR_QUOTE)
using Bad = q::RawStatement<"SELECT $$text$$ AS n", q::RawArguments<>, sch::Field<std::string, "n">>;
[[maybe_unused]] auto bound = Bad::bind();
#endif

#if defined(CASE_RAW_NARROWING_BIND)
using Bad = q::RawStatement<"SELECT $1::real AS n", q::RawArguments<float>, sch::Field<float, "n">>;
[[maybe_unused]] auto bound = Bad::bind(1.5);
#endif

int main() {}
