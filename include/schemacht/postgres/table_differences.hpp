#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "schemacht/column_type.hpp"
#include "schemacht/postgres/statements.hpp"
#include "schemacht/query/sql.hpp"
#include "schemacht/schema/field.hpp"
#include "schemacht/schema/schema.hpp"

/**
 * @file
 * Checking a table against its schema: `table_differences_statement` asks the database how the table differs from what the
 * schema says, for a program to check when it starts, before the first query fails on a table that is missing or was changed.
 */
namespace schemacht::postgres {

/** @brief A way a table differs from its schema (see `table_differences_statement`). */
enum class TableProblem : std::uint8_t {
  MissingTable,         ///< No table of that name is in the database (nor a view). `expected` is its name; nothing else is reported.
  MissingColumn,        ///< The table has no column of this name. `expected` is the type the schema gives it.
  UnknownType,          ///< The database has no type of the name the schema gives the column (`expected`): an extension not installed, say.
  WrongType,            ///< The column is of the type `found`, where the schema's is `expected` (both as the database spells them).
  Nullability,          ///< The column is `NOT NULL` where the schema's field is a `std::optional`, or the other way: `expected` and `found`.
  NoDefault,            ///< The schema's field is `GENERATED`, so never inserted, but the column is `NOT NULL` with nothing to fill it in.
  AlwaysGenerated,      ///< The database always computes the column (an identity `ALWAYS`, a stored expression), but an insert gives it a value.
  PrimaryKey,           ///< The table's primary key is the columns `found` (none, if empty), where the schema's is `expected`.
  ExtraRequiredColumn,  ///< A column the schema lacks that is `NOT NULL` with no default, of the type `found`: an insert would fail.
};

}  // namespace schemacht::postgres

/** @brief A `TableProblem` is read from the label the check's query gives it. */
template <>
struct schemacht::ColumnType<schemacht::postgres::TableProblem>
    : schemacht::EnumColumn<
          schemacht::postgres::TableProblem, "text", "missing_table", "missing_column", "unknown_type", "wrong_type", "nullability", "no_default",
          "always_generated", "primary_key", "extra_required_column"> {};

namespace schemacht::postgres {

/**
 * @brief One difference between a table and its schema: what is wrong (`problem`), with which column (`column`; empty for the
 * table itself and its primary key), and what the schema says against what the table has (`expected`, `found`; see
 * `TableProblem` for each). Read with `schema::get<"problem">(difference)` and so on; `describe` puts it in words.
 */
using TableDifference = std::tuple<
    schema::Field<TableProblem, "problem">, schema::Field<std::string, "column">, schema::Field<std::string, "expected">,
    schema::Field<std::string, "found">>;

namespace detail {

/** @brief The table check for `schema_t` (see `query::sql::table_differences_statement`), as a static string. */
template <schema::IsSchema schema_t>
class TableDifferencesSql {
  static constexpr auto PARTS = [] {
    constexpr auto TEXT = query::sql::table_differences_statement<schema_t>();
    return std::pair{query::sql::terminated<TEXT.size()>(TEXT), TEXT.table_slot()};
  }();

 public:
  static constexpr std::string_view      VALUE{PARTS.first.data(), PARTS.first.size() - 1};
  static constexpr query::sql::TableSlot TABLE_SLOT = PARTS.second;  ///< Where `VALUE` names the table (see `query::sql::on_table`).
};

}  // namespace detail

/**
 * @return The statement that asks the database how `schema_t`'s table differs from the schema: `execute` gives a
 * `TableDifference` for each difference, and none if the table is as the schema says. Aimed with `on`, it checks a table named
 * while the program runs. A difference is data, not an error, so a program can gather those of all its tables when it starts,
 * and say them all at once.
 *
 * What counts as a difference is what makes the schema's statements fail, or read or write the wrong thing:
 * - the table is not there (and then nothing else is reported). A name that is something else's, such as an index's or a
 *   sequence's, is no table;
 * - a column of the schema is missing, or of another type, or its type is one the database does not have. Types are compared
 *   as the database spells them, so `int8` is `bigint`, and with their modifiers, so `varchar(3)` is not `varchar(4)`. A
 *   column of a domain counts as the type the domain is over (but an array of a domain is its own type);
 * - a column is `NOT NULL` and its field a `std::optional`, or the other way;
 * - a `GENERATED` field's column is `NOT NULL` with nothing to fill it in (no default, identity or expression), or a field that
 *   is inserted has a column the database always computes;
 * - the table's primary key is not the schema's columns (if the schema has a key: one without says nothing of the table's);
 * - the table has a column the schema lacks that is `NOT NULL` with no default, so that an insert would fail.
 *
 * Not differences: any other column the schema lacks, the order of the columns, and indexes, `UNIQUE` and other constraints. A
 * table `create_table_statement` made has none. Of a view, a materialized view or a foreign table, only the columns and their
 * types are compared: it has no constraints of its own to compare. What a trigger does is not seen: a column one fills in still
 * counts as having nothing to fill it.
 *
 * The differences come by kind, as `TableProblem` lists them, and within a kind in the schema's column order.
 *
 * It needs PostgreSQL 17 or newer (for `to_regtypemod`); on an older server it fails with a `QueryError`. So does a column type
 * the database can't read as a type's name at all (`text(5)`, say): only a name that is well formed, for a type that is not
 * there, is a difference.
 */
template <schema::IsSchema schema_t>
[[nodiscard]] SchemaStatement<schema_t, TableDifference> table_differences_statement() {
  using sql_t = detail::TableDifferencesSql<schema_t>;
  return detail::SchemaStatementMaker::make<schema_t, TableDifference>(std::string(sql_t::VALUE), sql_t::TABLE_SLOT, std::vector<std::string>{});
}

/**
 * @return `difference` in words, for a log or an error's message:
 * `the column "geom" is of type geometry(Point,4326), not geometry(LineString,4326)`.
 */
[[nodiscard]] inline std::string describe(const TableDifference& difference) {
  const std::string& expected = schema::get<"expected">(difference);
  const std::string& found = schema::get<"found">(difference);
  const std::string  column = "the column \"" + schema::get<"column">(difference) + "\"";

  switch ( schema::get<"problem">(difference) ) {
    case TableProblem::MissingTable:
      return "the table " + expected + " is not in the database";
    case TableProblem::MissingColumn:
      return column + " (" + expected + ") is missing";
    case TableProblem::UnknownType:
      return column + " is of type " + found + ", and the database has no type " + expected;
    case TableProblem::WrongType:
      return column + " is of type " + found + ", not " + expected;
    case TableProblem::Nullability:
      return column + " is " + found + ", not " + expected;
    case TableProblem::NoDefault:
      return column + " has no default, identity or expression to fill it in, and is never inserted";
    case TableProblem::AlwaysGenerated:
      return column + " is always generated by the database, and is inserted";
    case TableProblem::PrimaryKey:
      return found.empty() ? "the table has no primary key, not (" + expected + ")" : "the primary key is (" + found + "), not (" + expected + ")";
    case TableProblem::ExtraRequiredColumn:
      return column + " (" + found + ") is NOT NULL with no default, and not in the schema";
  }

  return "an unknown difference";  // unreachable: every TableProblem is handled above
}

}  // namespace schemacht::postgres
