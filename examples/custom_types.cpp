// Column types of your own: storing a C++ type in a PostgreSQL column and reading it back.
//
// A type of your own becomes a column type by specializing `schemacht::ColumnType` once, next to the type
// (schemacht/column_type.hpp). Three kinds, from least to most work:
//
// - OrderId: a strong type over a built-in one (a `bigint`). `WrappedColumn` writes the whole specialization, binary form included.
// - Mood: a C++ enum stored as a PostgreSQL enum. `EnumColumn` writes it from the labels.
// - Money: exact cents stored as `numeric(12,2)`, with a text form written by hand. It has no binary form, so rows holding it
//   are read as text, which needs nothing more.
//
// The library's own date, time and UUID types sit beside them: `std::chrono::system_clock::time_point` is a `timestamptz`, and
// `schemacht::Uuid` a `uuid`. Give a type `operator<=>` if queries should compare with it (`<`, `>`) or order by it.
//
// Run with a libpq connection string: schemacht_example_custom_types "host=localhost dbname=app user=app"
// It creates the type schemacht_example_mood and the table schemacht_example_orders, and drops them when it is done.

#include <charconv>
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
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
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

/** @brief A `Mood` is stored as its label; the labels are in the order of `Mood`'s values. */
template <>
struct schemacht::ColumnType<shop::Mood> : schemacht::EnumColumn<shop::Mood, "schemacht_example_mood", "sad", "ok", "happy"> {};

/** @brief `Money` is written as PostgreSQL prints a `numeric(12,2)`: `19.99`, `-0.05`. */
template <>
struct schemacht::ColumnType<shop::Money> {
  static constexpr std::string_view SQL_TYPE = "numeric(12,2)";  // a type with modifiers is fine

  /** @throws std::invalid_argument for anything but an amount with exactly two decimals, such as `NaN`. */
  static shop::Money parse(std::string_view text) {
    const std::string_view whole = text;
    const bool             negative = text.starts_with('-');
    if ( negative ) {
      text.remove_prefix(1);
    }
    const std::size_t point = text.find('.');
    if ( point == std::string_view::npos || text.size() - point != 3 ) {
      throw std::invalid_argument("not an amount with two decimals: '" + std::string(whole) + "'");
    }
    const std::optional<std::int64_t> units = digits(text.substr(0, point));
    const std::optional<std::int64_t> hundredths = digits(text.substr(point + 1));
    if ( ! units || ! hundredths ) {
      throw std::invalid_argument("not an amount: '" + std::string(whole) + "'");
    }
    const std::int64_t cents = (*units * CENTS_PER_UNIT) + *hundredths;
    return {negative ? -cents : cents};
  }

  static std::string format(const shop::Money& amount) {
    const std::int64_t magnitude = amount.cents < 0 ? -amount.cents : amount.cents;
    return std::format("{}{}.{:02}", amount.cents < 0 ? "-" : "", magnitude / CENTS_PER_UNIT, magnitude % CENTS_PER_UNIT);
  }

 private:
  static constexpr std::int64_t CENTS_PER_UNIT = 100;

  /** @return `text` as a number, if it is digits and nothing else. */
  static std::optional<std::int64_t> digits(std::string_view text) {
    std::uint64_t value = 0;                        // unsigned: `from_chars` then refuses a sign
    const char*   end = text.data() + text.size();  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage) -- the end is passed: from_chars needs no terminator
    const auto [last, error] = std::from_chars(text.data(), end, value);
    if ( text.empty() || error != std::errc{} || last != end ) {
      return std::nullopt;
    }
    return static_cast<std::int64_t>(value);
  }
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
constexpr auto BY_MOOD = q::select(OrderColumns::col<"mood">().in(q::arg<0>())).order_by(OrderColumns::col<"total">().desc());

// Who spent more than an amount, when, and how they felt. None of the returned columns is a `Money`, so this result is read in
// binary; `BY_MOOD`'s rows hold one, so they are read as text.
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

    // Out: each column through its `ColumnType::parse` (text) or `parse_binary` (binary), read by name.
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
