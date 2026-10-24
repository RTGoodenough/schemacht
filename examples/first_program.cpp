// A first program, as README.md shows it: a schema, a table made from it, rows in with insert_returning, a query with an
// argument, and a transaction.
//
// Run with a libpq connection string: schemacht_example_first_program "host=localhost dbname=app user=app"
// It creates the table schemacht_example_people, and drops it when it is done.

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>

#include "schemacht/schemacht.hpp"

namespace sch = schemacht::schema;
namespace q = schemacht::query;
namespace pg = schemacht::postgres;

// A table, described once: each column's C++ type and name.
using Id = sch::Field<std::int64_t, "id", sch::KeyRole::Primary, true>;  // true: the database numbers it
using Name = sch::Field<std::string, "name">;
using Email = sch::Field<std::optional<std::string>, "email">;  // a std::optional column can be NULL
using Score = sch::Field<double, "score">;
using People = sch::Schema<"schemacht_example_people", Id, Name, Email, Score>;
using Col = q::On<People>;

// Statements, written in C++ and turned into SQL while compiling; arg<N>() is a value given when one runs.
constexpr auto AT_LEAST = q::select(Col::col<"score">() >= q::arg<0>()).order_by(Col::col<"score">().desc()).limit(10);
constexpr auto SET_SCORE = q::update<People>().set(Col::col<"score">(), q::arg<0>()).where(Col::col<"id">() == q::arg<1>());

int main(int argc, char** argv) {
  pg::Database database(argc > 1 ? argv[1] : "");  // a pool of connections
  database.drop_table<People>().get();             // if it is there from a run before
  database.create_table<People>().get();

  // Every call returns a std::future; insert_returning gives back what the database filled in, such as the ids.
  const auto added = database
                         .insert_returning<People>({
                             People::make_insert_row("ann", "ann@example.com", 91.5),
                             People::make_insert_row("bob", std::nullopt, 78.0),
                         })
                         .get();

  for ( const People::row_type& person : database.execute(q::Prepared<AT_LEAST>::bind(80.0)).get() ) {
    std::cout << sch::get<"name">(person) << ": " << sch::get<"score">(person) << '\n';
  }

  // Changes made together, or not at all.
  pg::Transaction transaction = database.begin().get();
  for ( const People::row_type& person : added ) {
    transaction.execute(q::Prepared<SET_SCORE>::bind(0.0, sch::get<"id">(person))).get();
  }
  transaction.commit().get();

  database.drop_table<People>().get();
}
