// Each block must fail to compile, with the message tests/CMakeLists.txt expects for it. Compiled one case at a time.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "schemacht/column_type.hpp"
#include "schemacht/postgres/database.hpp"
#include "schemacht/query.hpp"

namespace sch = schemacht::schema;
namespace q = schemacht::query;
namespace pg = schemacht::postgres;

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
constexpr auto        BY_RATIO = q::On<Things>::col<"ratio">() == q::arg<0>();
[[maybe_unused]] auto bound = q::Prepared<BY_RATIO>::bind(1.5);  // a double, for a float column
#endif

#if defined(CASE_NEGATIVE_FOR_UNSIGNED_BIND)
constexpr auto        BY_COUNT = q::On<Things>::col<"count">() == q::arg<0>();
[[maybe_unused]] auto bound = q::Prepared<BY_COUNT>::bind(-1);
#endif

#if defined(CASE_WRONG_ARGUMENT_COUNT)
constexpr auto        BY_ID = q::On<Things>::col<"id">() == q::arg<0>();
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

#if defined(CASE_RAW_REFERENCE_ARGUMENT)
using Bad = q::RawStatement<"SELECT $1 AS n", q::RawArguments<const int&>, sch::Field<int, "n">>;
[[maybe_unused]] Bad::row_type row;
#endif

#if defined(CASE_RAW_NOT_ARGUMENTS)
using Bad = q::RawStatement<"SELECT $1 AS n", int, sch::Field<int, "n">>;
[[maybe_unused]] Bad::row_type row;
#endif

#if defined(CASE_RAW_ARGUMENT_WITHOUT_TEXT_FORM)
struct Opaque {};
using Bad = q::RawStatement<"SELECT $1::text AS n", q::RawArguments<Opaque>, sch::Field<std::string, "n">>;
[[maybe_unused]] void run(pg::Database& database) { (void)database.execute(Bad::bind(Opaque{})); }
#endif

#if defined(CASE_INSERT_FIELD_WITHOUT_TEXT_FORM)
struct Opaque {};
using Bad = sch::Schema<"bad", Id, sch::Field<Opaque, "blob">>;
[[maybe_unused]] void run(pg::Database& database) { (void)database.insert<Bad>(std::vector<Bad::row_type>{}); }
#endif

#if defined(CASE_RAW_RESULT_WITHOUT_TEXT_FORM)
struct Opaque {};
using Bad = q::RawStatement<"SELECT 1 AS blob", q::RawArguments<>, sch::Field<Opaque, "blob">>;
[[maybe_unused]] void run(pg::Database& database) { (void)database.execute(Bad::bind()); }
#endif

#if defined(CASE_GET_UNKNOWN_COLUMN)
[[maybe_unused]] const auto& value = schemacht::schema::get<"nope">(Things::row_type{});
#endif

#if defined(CASE_SELECT_BY_KEY_COMPOSITE)
using Pairs = sch::Schema<"pairs", sch::Field<std::int64_t, "a", sch::KeyRole::Primary>, sch::Field<std::int64_t, "b", sch::KeyRole::Primary>>;
[[maybe_unused]] void run(pg::Database& database) { (void)database.select_by_key<Pairs>(std::vector<std::int64_t>{1}); }
#endif

#if defined(CASE_UPDATE_WITHOUT_WHERE)
constexpr auto        BAD = q::update<Things>().set(q::On<Things>::col<"name">(), q::arg<0>());
[[maybe_unused]] auto bound = q::Prepared<BAD>::bind("x");
#endif

#if defined(CASE_UPDATE_SET_TWICE)
constexpr auto BAD = q::update<Things>().set(q::On<Things>::col<"count">(), 1).set(q::On<Things>::col<"count">(), 2).all_rows();
#endif

#if defined(CASE_RETURNING_TWICE)
constexpr auto BAD = q::delete_all<Things>().returning(q::On<Things>::col<"id">(), q::On<Things>::col<"id">());
#endif

#if defined(CASE_UPDATE_SET_NULL_REQUIRED)
constexpr auto BAD = q::update<Things>().set_null(q::On<Things>::col<"name">()).all_rows();
#endif

#if defined(CASE_UPDATE_NARROWING_SET)
constexpr auto        BAD = q::update<Things>().set(q::On<Things>::col<"ratio">(), q::arg<0>()).all_rows();
[[maybe_unused]] auto bound = q::Prepared<BAD>::bind(1.5);  // a double, for a float column
#endif

#if defined(CASE_DELETE_WHERE_EVERYTHING)
constexpr auto BAD = q::delete_where(q::match_all<Things>());
#endif

#if defined(CASE_CREATE_TABLE_GENERATED)
// Generated, but not the key: create_table can't know the expression that fills it in.
using Bad = sch::Schema<"bad", Id, sch::Field<std::string, "label", sch::KeyRole::None, true>>;
[[maybe_unused]] void run(pg::Database& database) { (void)database.create_table<Bad>(); }
#endif

#if defined(CASE_CREATE_TABLE_WITHOUT_SQL_TYPE)
// A codec of its own (a FieldCodec specialization, not a ColumnType), and so no SQL type name.
struct Unnamed {
  std::string text;
};
template <>
class schemacht::postgres::FieldCodec<Unnamed> {
 public:
  static Unnamed     parse(std::string_view text) { return {std::string(text)}; }
  static std::string format(const Unnamed& value) { return value.text; }
};
using Bad = sch::Schema<"bad", Id, sch::Field<Unnamed, "unnamed">>;
[[maybe_unused]] void run(pg::Database& database) { (void)database.create_table<Bad>(); }
#endif

#if defined(CASE_IN_ON_ARRAY_COLUMN)
using Tagged = sch::Schema<"tagged", Id, sch::Field<std::vector<std::string>, "tags">>;
constexpr auto                              ANY_TAGS = q::On<Tagged>::col<"tags">().in(q::arg<0>());
[[maybe_unused]] constexpr std::string_view SQL = q::Prepared<ANY_TAGS>::SQL;
#endif

#if defined(CASE_COLUMN_TYPE_WITHOUT_FORMAT)
struct Half {};
template <>
struct schemacht::ColumnType<Half> {
  static constexpr std::string_view SQL_TYPE = "half";
  static Half                       parse(std::string_view /*text*/) { return {}; }
};
[[maybe_unused]] const std::string text = pg::format_field(Half{});
#endif

#if defined(CASE_OPERATION_ARGUMENT_TYPE) || defined(CASE_OPERATION_ARGUMENT_OTHER_TYPE) || defined(CASE_OPERATION_ARGUMENT_DISAGREES) || \
    defined(CASE_OPERATION_ARGUMENT_NOT_COLUMN_TYPE)
struct Key {
  std::string value;
};
template <>
struct schemacht::ColumnType<Key> : schemacht::WrappedColumn<&Key::value> {};
struct HasKey {
  static constexpr std::string_view OPERATOR = "?";
  using argument_type = Key;
};
#endif

#if defined(CASE_OPERATION_ARGUMENT_TYPE)
constexpr auto        NAMED = q::On<Things>::col<"name">().apply<HasKey>(q::arg<0>());
[[maybe_unused]] auto bound = q::Prepared<NAMED>::bind(std::string("name"));  // the column's type, not the operation's
#endif

#if defined(CASE_OPERATION_ARGUMENT_OTHER_TYPE)
struct Label {  // the same SQL type as `Key`, text, but another C++ type
  std::string value;
};
template <>
struct schemacht::ColumnType<Label> : schemacht::WrappedColumn<&Label::value> {};
constexpr auto        NAMED = q::On<Things>::col<"name">().apply<HasKey>(q::arg<0>());
[[maybe_unused]] auto bound = q::Prepared<NAMED>::bind(Label{"name"});
#endif

#if defined(CASE_OPERATION_ARGUMENT_DISAGREES)
constexpr auto BOTH = q::On<Things>::col<"name">().apply<HasKey>(q::arg<0>()) || q::On<Things>::col<"name">() == q::arg<0>();
[[maybe_unused]] constexpr std::string_view SQL = q::Prepared<BOTH>::SQL;
#endif

#if defined(CASE_OPERATION_ARGUMENT_NOT_COLUMN_TYPE)
struct Counts {
  static constexpr std::string_view OPERATOR = "@>";
  using argument_type = int;
};
constexpr auto BAD = q::On<Things>::col<"name">().apply<Counts>(q::arg<0>());
#endif

int main() {}
