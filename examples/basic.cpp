// The basics: a schema, a table made from it, rows in, and a query with an argument.
//
// Run with a libpq connection string: schemacht_example_basic "host=localhost dbname=app user=app"
// It creates the table schemacht_example_employees, and drops it when it is done.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <span>
#include <string>

#include "schemacht/schemacht.hpp"

using ID = schemacht::schema::Field<std::int64_t, "id", schemacht::schema::KeyRole::Primary>;
using Name = schemacht::schema::Field<std::string, "name">;
using Salary = schemacht::schema::Field<float, "salary">;

using EmployeeSchema = schemacht::schema::Schema<"schemacht_example_employees", ID, Name, Salary>;

using Employees = schemacht::query::On<EmployeeSchema>;
constexpr auto NAME_LIKE = schemacht::query::like(Employees::col<"name">(), schemacht::query::arg<0>());

using NameLookup = schemacht::query::Prepared<NAME_LIKE>;

int main(int argc, char** argv) {
  try {
    // A libpq connection string, e.g. "host=localhost dbname=app user=app"; empty uses libpq's defaults and PG* variables.
    const std::span<char*> args(argv, static_cast<std::size_t>(argc));
    const std::string      conninfo = args.size() > 1 ? args.subspan(1).front() : "";

    schemacht::postgres::Database database(conninfo);

    database.drop_table<EmployeeSchema>().get();
    database.create_table<EmployeeSchema>().get();

    database
        .insert<EmployeeSchema>({
            EmployeeSchema::make_row(1, "john smith", 52000.0F),
            EmployeeSchema::make_row(2, "jane doe", 61000.0F),
            EmployeeSchema::make_row(3, "johnny cash", 48000.0F),
        })
        .get();

    const auto results = database.execute(NameLookup::bind("john%")).get();

    for ( const EmployeeSchema::row_type& employee : results ) {
      std::cout << "EID: " << schemacht::schema::get<"id">(employee) << " Name: " << schemacht::schema::get<"name">(employee)
                << " Salary: " << schemacht::schema::get<"salary">(employee) << '\n';
    }

    database.drop_table<EmployeeSchema>().get();

    return EXIT_SUCCESS;
  } catch ( const std::exception& error ) {
    std::cerr << "error: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
