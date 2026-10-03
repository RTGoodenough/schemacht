// The basics: a schema, a table made from it, rows in, queries with arguments, a statement written out in full, and the same
// schema on a table named while the program runs.
//
// Run with a libpq connection string: schemacht_example_basic "host=localhost dbname=app user=app"
// It creates the tables schemacht_example_employees and schemacht_example_employees_archive, and drops them when it is done.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
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

// The best paid at or above a salary, as many as asked for, the highest first: only their names and salaries are fetched.
constexpr auto TOP_PAID = schemacht::query::select(Employees::col<"salary">() >= schemacht::query::arg<0>())
                              .order_by(Employees::col<"salary">().desc())
                              .limit(schemacht::query::arg<1>())
                              .project(Employees::col<"name">(), Employees::col<"salary">());

// What the query builder cannot say (an aggregate, here) is written out in full; its arguments and columns are still typed,
// and checked against its `$n` placeholders while compiling.
using Headcount = schemacht::schema::Field<std::int64_t, "headcount">;
using Average = schemacht::schema::Field<std::optional<double>, "average">;  // NULL when no row matches
using PayrollSummary = schemacht::query::RawStatement<
    "SELECT count(*) AS headcount, avg(salary) AS average FROM schemacht_example_employees WHERE salary >= $1", schemacht::query::RawArguments<float>,
    Headcount, Average>;

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

    std::cout << "The two best paid from 50000:\n";
    for ( const auto& employee : database.execute(schemacht::query::Prepared<TOP_PAID>::bind(50000.0F, 2)).get() ) {
      std::cout << "  " << schemacht::schema::get<"name">(employee) << ": " << schemacht::schema::get<"salary">(employee) << '\n';
    }

    const PayrollSummary::row_type summary = database.execute(PayrollSummary::bind(50000.0F)).get().at(0);
    std::cout << schemacht::schema::get<"headcount">(summary) << " earn 50000 or more, " << schemacht::schema::get<"average">(summary).value_or(0.0)
              << " on average.\n";

    // The same schema on a table whose name is known only now: a string, as configuration, a user, or another language's
    // bindings would give it. Its name is checked once, here; the statements above run on it as they are.
    const std::string                          archive_name = std::string(EmployeeSchema::TABLE_NAME) + "_archive";
    schemacht::postgres::Table<EmployeeSchema> archive(database, schemacht::schema::TableName(archive_name));

    archive.drop().get();
    archive.create().get();
    archive.insert({EmployeeSchema::make_row(3, "johnny cash", 48000.0F)}).get();

    std::cout << "In " << archive_name << ":\n";
    for ( const EmployeeSchema::row_type& employee : archive.execute(NameLookup::bind("john%")).get() ) {
      std::cout << "  " << schemacht::schema::get<"name">(employee) << '\n';
    }

    archive.drop().get();

    database.drop_table<EmployeeSchema>().get();

    return EXIT_SUCCESS;
  } catch ( const std::exception& error ) {
    std::cerr << "error: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
