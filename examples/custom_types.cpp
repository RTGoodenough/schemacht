// Column types of your own: storing a C++ type in a PostgreSQL column and reading it back.
//
// Three types, from least to most work:
//
// - OrderId: a strong type over a built-in one (a `bigint`). Its codec forwards to the built-in one, binary form included.
// - Mood: a C++ enum stored as a PostgreSQL enum. Its text form is the label, and so is its binary form.
// - Money: exact cents stored as `numeric(12,2)`. It has a text form only, so rows holding it are read as text.
//
// What a type needs (see schemacht/column_type.hpp):
//
// - `postgres::FieldCodec<T>` with `parse` and `format`: its text form. Required for every column type.
// - `parse_binary` in that codec: lets results be read in binary form, the default. Optional.
// - `binary_accepts` in that codec: refuses a column of the wrong PostgreSQL type before any row is read. Optional.
// - `query::sql::SqlType<T>`: its SQL type's name, for `Database::insert` and list arguments (`.in(...)`).
// - `operator<=>`: lets queries compare with it (`<`, `>`) and `ORDER BY` it.
//
// Run with a libpq connection string: schemacht_example_custom_types "host=localhost dbname=app user=app"
// It creates the type schemacht_example_mood and the table schemacht_example_orders, and drops them when it is done.

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <format>
#include <future>
#include <iostream>
#include <optional>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
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

// ---- How each is written to and read from PostgreSQL ----------------------------------------------------------------

namespace schemacht::postgres {

/** @brief An `OrderId` is a `bigint` on the wire, in both forms: the built-in codec does the work. */
template <>
class FieldCodec<shop::OrderId> {
  using Bigint = FieldCodec<std::int64_t>;

 public:
  static shop::OrderId parse(std::string_view text) { return {Bigint::parse(text)}; }
  static std::string   format(const shop::OrderId& id) { return Bigint::format(id.value); }

  static shop::OrderId parse_binary(std::span<const std::byte> bytes) { return {Bigint::parse_binary(bytes)}; }
  static bool          binary_accepts(std::uint32_t type_oid) { return Bigint::binary_accepts(type_oid); }
};

/** @brief A `Mood` is its enum label, `sad`, `ok` or `happy`, in text and binary form alike. */
template <>
class FieldCodec<shop::Mood> {
 public:
  /** @throws std::invalid_argument for a label that is not a `Mood` (one added to the enum in the database, say). */
  static shop::Mood parse(std::string_view label) {
    const auto found = std::ranges::find(LABELS, label);
    if ( found == LABELS.end() ) {
      throw std::invalid_argument("not a mood: '" + std::string(label) + "'");
    }
    return static_cast<shop::Mood>(found - LABELS.begin());
  }

  static std::string format(const shop::Mood& mood) { return std::string(LABELS.at(static_cast<std::size_t>(mood))); }

  /** @brief PostgreSQL sends an enum in binary form as its label's bytes. */
  static shop::Mood parse_binary(std::span<const std::byte> bytes) {
    std::string label;
    for ( const std::byte byte : bytes ) {
      label += static_cast<char>(byte);
    }
    return parse(label);
  }

  // There is no `binary_accepts`: an enum's type OID is chosen when `CREATE TYPE` runs, so it differs between databases.

 private:
  static constexpr std::array<std::string_view, 3> LABELS{"sad", "ok", "happy"};  // in the order of `Mood`'s values
};

/**
 * @brief `Money` is written as PostgreSQL prints a `numeric(12,2)`: `19.99`, `-0.05`. It has no `parse_binary`
 * (`numeric`'s binary form is base-10000 digits), so a row holding it is read as text, which needs nothing more.
 */
template <>
class FieldCodec<shop::Money> {
 public:
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

}  // namespace schemacht::postgres

// ---- Each one's SQL type, which `insert` and list arguments cast to ------------------------------------------------

namespace schemacht::query::sql {

template <>
class SqlType<shop::OrderId> {
 public:
  static constexpr std::string_view NAME = "bigint";
};

template <>
class SqlType<shop::Mood> {
 public:
  static constexpr std::string_view NAME = "schemacht_example_mood";
};

template <>
class SqlType<shop::Money> {
 public:
  static constexpr std::string_view NAME = "numeric";
};

}  // namespace schemacht::query::sql

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
using Total = sch::Field<shop::Money, "total">;
using CustomerMood = sch::Field<shop::Mood, "mood">;
using Note = sch::Field<std::optional<std::string>, "note">;

using Orders = sch::Schema<"schemacht_example_orders", Id, Total, CustomerMood, Note>;
using OrderColumns = q::On<Orders>;

// The orders with any of some moods, the largest first. The moods go as one array argument, `$1::schemacht_example_mood[]`,
// which is what `SqlType<Mood>` is for.
constexpr auto BY_MOOD = q::select(OrderColumns::col<"mood">().in(q::arg<0>())).order_by(OrderColumns::col<"total">().desc());

// Who spent more than an amount, and how they felt. Both returned columns have a binary form, so this result is read in
// binary; `BY_MOOD`'s rows hold a `Money`, which has none, so they are read as text.
constexpr auto SPENT_OVER = q::select(OrderColumns::col<"total">() > q::arg<0>())
                                .order_by(OrderColumns::col<"id">().asc())
                                .project(OrderColumns::col<"id">(), OrderColumns::col<"mood">());

namespace {

/** @brief Runs `sql`, which returns no rows, and waits for it: for creating and dropping the example's table and type. */
void run_sql(pg::Database& database, std::string sql) {
  std::promise<void> done;
  database.pool().run(std::move(sql), {}, [&done](const pg::QueryResult& /*result*/, const std::exception_ptr& error) {
    if ( error ) {
      done.set_exception(error);
    } else {
      done.set_value();
    }
  });
  done.get_future().get();
}

void drop_tables(pg::Database& database) {
  run_sql(database, "DROP TABLE IF EXISTS schemacht_example_orders");
  run_sql(database, "DROP TYPE IF EXISTS schemacht_example_mood");
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const std::span<char*> args(argv, static_cast<std::size_t>(argc));
    const std::string      conninfo = args.size() > 1 ? args.subspan(1).front() : "";

    pg::Database database(conninfo);

    // The application's own types need their database types to exist first.
    drop_tables(database);
    run_sql(database, "CREATE TYPE schemacht_example_mood AS ENUM ('sad', 'ok', 'happy')");
    run_sql(database,
            "CREATE TABLE schemacht_example_orders "
            "(id bigint PRIMARY KEY, total numeric(12,2) NOT NULL, mood schemacht_example_mood NOT NULL, note text)");

    // In: each column of each row through its `FieldCodec::format`.
    using shop::Mood;
    database
        .insert<Orders>(std::vector<Orders::row_type>{
            {Id{{1}}, Total{{1999}}, CustomerMood{Mood::Happy}, Note{"gift wrap"}},
            {Id{{2}}, Total{{-500}}, CustomerMood{Mood::Sad}, Note{"refund"}},
            {Id{{3}}, Total{{120050}}, CustomerMood{Mood::Ok}, Note{std::nullopt}},
            {Id{{4}}, Total{{5}}, CustomerMood{Mood::Happy}, Note{std::nullopt}},
        })
        .get();

    // Out: each column through its `FieldCodec::parse` (text) or `parse_binary` (binary).
    std::cout << "Happy or ok orders, the largest first:\n";
    for ( const auto& [id, total, mood, note] : database.execute(q::Prepared<BY_MOOD>::bind(std::vector{Mood::Happy, Mood::Ok})).get() ) {
      std::cout << "  " << id.value << "  " << total.value << "  " << mood.value << "  " << note.value.value_or("-") << '\n';
    }

    std::cout << "Orders over $10.00:\n";
    for ( const auto& [id, mood] : database.execute(q::Prepared<SPENT_OVER>::bind(shop::Money{1000})).get() ) {
      std::cout << "  " << id.value << "  " << mood.value << '\n';
    }

    // A key of the application's type works as any other: the keys go as one `bigint[]` argument.
    const std::vector<shop::OrderId> wanted{{2}, {4}};
    std::cout << "Orders #2 and #4:\n";
    for ( const auto& [id, total, mood, note] : database.select_by_key_values<Orders, "id">(wanted).get() ) {
      std::cout << "  " << id.value << "  " << total.value << '\n';
    }

    drop_tables(database);
    return EXIT_SUCCESS;
  } catch ( const std::exception& error ) {
    std::cerr << "error: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
