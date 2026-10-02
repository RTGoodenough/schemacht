// Column types of your own: storing a C++ type in a PostgreSQL column and reading it back.
//
// A type of your own becomes a column type by specializing `schemacht::ColumnType` once, next to the type
// (schemacht/column_type.hpp). It gives the SQL type's name and the text form; a binary form is optional, and lets results be
// read in PostgreSQL's binary form, which is less to send and parse. Three kinds:
//
// - OrderId: a strong type over a built-in one (a `bigint`). `WrappedColumn` writes the whole specialization, binary form included.
// - Mood: a C++ enum stored as a PostgreSQL enum. `EnumColumn` writes it from the labels, binary form included.
// - Money: exact cents stored as `numeric(12,2)`, written by hand with the library's helpers for `numeric` (postgres/numeric.hpp)
//   and the type's OID (postgres/binary_form.hpp), in text and in binary form. `ScaledNumericColumn` writes the same in one
//   line; it is spelled out here to show what a `ColumnType` holds.
//
// The other helpers in schemacht/column_type.hpp: `EnumIntegerColumn` stores an enum as its number rather than a label, and
// `ConvertedColumn` stores any type as another the database knows, through two functions (a path as `text`, a duration as
// `bigint` milliseconds).
//
// A type without `parse_binary` works too: a row holding one is then read as text, whatever the query asks for.
//
// The library's own date, time and UUID types sit beside them: `std::chrono::system_clock::time_point` is a `timestamptz`, and
// `schemacht::Uuid` a `uuid`. Give a type `operator<=>` if queries should compare with it (`<`, `>`) or order by it.
//
// Run with a libpq connection string: schemacht_example_custom_types "host=localhost dbname=app user=app"
// It creates the type schemacht_example_mood and the table schemacht_example_orders, and drops them when it is done.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <format>
#include <iostream>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "schemacht/column_type.hpp"
#include "schemacht/schemacht.hpp"

// ---- The types, as the application defines them --------------------------------------------------------------------

namespace shop {

/** @brief An order's number: a `bigint` in the database, but not to be mixed up with other numbers in C++. */
struct OrderId {
  std::int64_t value = 0;

  auto operator<=>(const OrderId&) const = default;
};

/** @brief How the customer felt about the order: the PostgreSQL enum `schemacht_example_mood`. */
enum class Mood : std::uint8_t {
  Sad,
  Ok,
  Happy,
};

/** @brief An amount of money in exact cents: `numeric(12,2)` in the database, never a floating point number. */
struct Money {
  std::int64_t cents = 0;

  auto operator<=>(const Money&) const = default;
};

}  // namespace shop

// ---- How each is stored ---------------------------------------------------------------------------------------------

/** @brief An `OrderId` is stored as the `bigint` it wraps. */
template <>
struct schemacht::ColumnType<shop::OrderId> : schemacht::WrappedColumn<&shop::OrderId::value> {};

/**
 * @brief A `Mood` is stored as its label; the labels are in the order of `Mood`'s values. (Without a PostgreSQL enum type,
 * `EnumColumn<shop::Mood, "text", ...>` keeps the labels in a `text` column, and `EnumIntegerColumn<shop::Mood>` stores the
 * number instead, a `smallint`.)
 */
template <>
struct schemacht::ColumnType<shop::Mood> : schemacht::EnumColumn<shop::Mood, "schemacht_example_mood", "sad", "ok", "happy"> {};

/**
 * @brief `Money` is a `numeric(12,2)`: in text as PostgreSQL prints one (`19.99`, `-0.05`), and in binary form as its base-10000
 * digits, both read as a count of cents by the library's `numeric` helpers. They refuse rather than round: `NaN`, an infinity,
 * or a value with a third decimal place (a `total / 3`) is an error, not a nearby amount.
 *
 * Written out to show what a `ColumnType` holds. The one-line equivalent:
 * `template <> struct schemacht::ColumnType<shop::Money> : schemacht::ScaledNumericColumn<&shop::Money::cents, 2, "numeric(12,2)"> {};`
 */
template <>
struct schemacht::ColumnType<shop::Money> {
  static constexpr std::string_view SQL_TYPE = "numeric(12,2)";  // a type with modifiers is fine
  static constexpr unsigned         CENT_PLACES = 2;

  static shop::Money parse(std::string_view text) { return {schemacht::postgres::parse_scaled_numeric<CENT_PLACES>(text)}; }
  static std::string format(const shop::Money& amount) { return schemacht::postgres::format_scaled_numeric<CENT_PLACES>(amount.cents); }

  static shop::Money parse_binary(std::span<const std::byte> bytes) { return {schemacht::postgres::parse_binary_scaled_numeric<CENT_PLACES>(bytes)}; }

  /** @brief Only a `numeric` column: a `double precision` one, say, has another binary form, and is refused before any row is read. */
  static constexpr bool binary_accepts(std::uint32_t type_oid) { return type_oid == schemacht::postgres::OID_NUMERIC; }
};

namespace shop {

std::ostream& operator<<(std::ostream& out, const OrderId& id) { return out << '#' << id.value; }
std::ostream& operator<<(std::ostream& out, const Mood& mood) { return out << schemacht::postgres::format_field(mood); }
std::ostream& operator<<(std::ostream& out, const Money& amount) {
  return out << (amount.cents < 0 ? "-$" : "$") << schemacht::postgres::format_field(Money{amount.cents < 0 ? -amount.cents : amount.cents});
}

}  // namespace shop

// ---- Using them: they go in a schema like any built-in type ---------------------------------------------------------

namespace sch = schemacht::schema;
namespace q = schemacht::query;
namespace pg = schemacht::postgres;

using Id = sch::Field<shop::OrderId, "id", sch::KeyRole::Primary>;
using Reference = sch::Field<schemacht::Uuid, "reference">;
using PlacedAt = sch::Field<std::chrono::system_clock::time_point, "placed_at">;
using Total = sch::Field<shop::Money, "total">;
using CustomerMood = sch::Field<shop::Mood, "mood">;
using Note = sch::Field<std::optional<std::string>, "note">;

using Orders = sch::Schema<"schemacht_example_orders", Id, Reference, PlacedAt, Total, CustomerMood, Note>;
using OrderColumns = q::On<Orders>;

// The orders with any of some moods, the largest first. The moods go as one array argument, `$1::schemacht_example_mood[]`.
// Every column's type has a binary form, so the rows are read in binary (`Database`'s default).
constexpr auto BY_MOOD = q::select(OrderColumns::col<"mood">().in(q::arg<0>())).order_by(OrderColumns::col<"total">().desc());

// Who spent more than an amount, when, and how they felt: only those columns are fetched. The amount is sent as text
// (`ColumnType::format`), as every argument is.
constexpr auto SPENT_OVER = q::select(OrderColumns::col<"total">() > q::arg<0>())
                                .order_by(OrderColumns::col<"id">().asc())
                                .project(OrderColumns::col<"id">(), OrderColumns::col<"placed_at">(), OrderColumns::col<"mood">());

// Changes, which return how many rows they changed.
constexpr auto NOTE_SMALL_ORDERS =
    q::update<Orders>().set(OrderColumns::col<"note">(), q::arg<0>()).where(OrderColumns::col<"total">() < q::arg<1>());
constexpr auto FORGET_MOOD = q::delete_where(OrderColumns::col<"mood">() == q::arg<0>());

// The enum type has to exist before the table that uses it; that, the database's own business, is plain SQL.
using CreateMood = q::RawStatement<"CREATE TYPE schemacht_example_mood AS ENUM ('sad', 'ok', 'happy')", q::RawArguments<>>;
using DropMood = q::RawStatement<"DROP TYPE IF EXISTS schemacht_example_mood", q::RawArguments<>>;

namespace {

void drop_everything(pg::Database& database) {
  database.drop_table<Orders>().get();
  database.execute(DropMood::bind()).get();
}

std::string when(std::chrono::system_clock::time_point time) { return std::format("{:%F %R}", std::chrono::floor<std::chrono::minutes>(time)); }

}  // namespace

int main(int argc, char** argv) {
  try {
    const std::span<char*> args(argv, static_cast<std::size_t>(argc));
    const std::string      conninfo = args.size() > 1 ? args.subspan(1).front() : "";

    pg::Database database(conninfo);

    drop_everything(database);
    database.execute(CreateMood::bind()).get();
    database.create_table<Orders>().get();  // from the schema: each column with its type's SQL_TYPE

    // In: one plain value per column, each through its `ColumnType::format`.
    using namespace std::chrono_literals;
    using schemacht::Uuid;
    using shop::Mood;
    const auto morning = std::chrono::sys_days{2026y / 10 / 1} + 9h;
    database
        .insert<Orders>({
            Orders::make_row(shop::OrderId{1}, Uuid::from_string("0e9cd3a8-3f43-4f4e-9b5c-6b6f8f6f0a1d"), morning, shop::Money{1999}, Mood::Happy,
                             "gift wrap"),
            Orders::make_row(shop::OrderId{2}, Uuid::from_string("51c1c4a2-77d4-4b0e-8a39-2c4b1f0de9f3"), morning + 2h, shop::Money{-500}, Mood::Sad,
                             "refund"),
            Orders::make_row(shop::OrderId{3}, Uuid::from_string("9a0b7f6e-1d2c-4e3f-8a9b-0c1d2e3f4a5b"), morning + 26h, shop::Money{120050},
                             Mood::Ok, std::nullopt),
            Orders::make_row(shop::OrderId{4}, Uuid::from_string("c3d4e5f6-a7b8-4c9d-8e0f-1a2b3c4d5e6f"), morning + 50h, shop::Money{5}, Mood::Happy,
                             std::nullopt),
        })
        .get();

    // Out: each column through its `ColumnType::parse_binary` (`parse`, for a result read as text), read by name.
    std::cout << "Happy or ok orders, the largest first:\n";
    for ( const Orders::row_type& order : database.execute(q::Prepared<BY_MOOD>::bind(std::vector{Mood::Happy, Mood::Ok})).get() ) {
      std::cout << "  " << sch::get<"id">(order) << "  " << sch::get<"total">(order) << "  " << sch::get<"mood">(order) << "  "
                << sch::get<"note">(order).value_or("-") << "  " << sch::get<"reference">(order).to_string() << '\n';
    }

    std::cout << "Orders over $10.00:\n";
    for ( const auto& order : database.execute(q::Prepared<SPENT_OVER>::bind(shop::Money{1000})).get() ) {
      std::cout << "  " << sch::get<"id">(order) << "  " << when(sch::get<"placed_at">(order)) << "  " << sch::get<"mood">(order) << '\n';
    }

    const std::uint64_t noted = database.execute(q::Prepared<NOTE_SMALL_ORDERS>::bind("small order", shop::Money{100})).get();
    const std::uint64_t forgotten = database.execute(q::Prepared<FORGET_MOOD>::bind(Mood::Sad)).get();
    std::cout << "Noted " << noted << " small orders, and forgot " << forgotten << " sad one.\n";

    // A primary key of an application's type works as any other: the keys go as one `bigint[]` argument.
    std::cout << "Orders #2 and #4:\n";
    for ( const Orders::row_type& order : database.select_by_key<Orders>(std::vector<shop::OrderId>{{2}, {4}}).get() ) {
      std::cout << "  " << sch::get<"id">(order) << "  " << sch::get<"total">(order) << "  " << sch::get<"note">(order).value_or("-") << '\n';
    }

    drop_everything(database);
    return EXIT_SUCCESS;
  } catch ( const std::exception& error ) {
    std::cerr << "error: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
