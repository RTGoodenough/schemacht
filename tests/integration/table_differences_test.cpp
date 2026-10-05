// Against a real PostgreSQL (see database_test.cpp): how a table differs from its schema. The tests create (and drop) tables
// whose names start with `schemacht_it_check_`, and the schema `schemacht_it_check`.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "schemacht/column_type.hpp"
#include "schemacht/schemacht.hpp"
#include "support/integration.hpp"
#include "support/schemas.hpp"

namespace pg = schemacht::postgres;
namespace sch = schemacht::schema;
namespace t = schemacht::test;

using schemacht::test::test_db;

namespace {

/** @brief A code of at most three characters: `varchar(3)`, a type with a modifier. */
struct Code {
  std::string text;
};

/** @brief A value of a type the database does not have: an extension that is not installed, say. */
struct Unknown {
  std::string text;
};

}  // namespace

template <>
struct schemacht::ColumnType<Code> {
  static constexpr std::string_view SQL_TYPE = "varchar(3)";
  static Code                       parse(std::string_view text) { return Code{std::string(text)}; }
  static std::string                format(const Code& code) { return code.text; }
};

template <>
struct schemacht::ColumnType<Unknown> {
  static constexpr std::string_view SQL_TYPE = "schemacht_it_no_such_type(Point,4326)";
  static Unknown                    parse(std::string_view text) { return Unknown{std::string(text)}; }
  static std::string                format(const Unknown& unknown) { return unknown.text; }
};

namespace {

using Coded = sch::Schema<"schemacht_it_check_coded", t::Id, sch::Field<Code, "code">, sch::Field<std::optional<std::vector<Code>>, "codes">>;
using Strange = sch::Schema<"schemacht_it_check_strange", t::Id, sch::Field<Unknown, "shape">>;

/** @brief A difference as (problem, column, expected, found), to compare. */
using Found = std::tuple<pg::TableProblem, std::string, std::string, std::string>;

/** @return How `table` differs from `schema_t`. */
template <sch::IsSchema schema_t>
std::vector<Found> differences(pg::StatementRunner& runner, const sch::Table<schema_t>& table) {
  std::vector<Found> found;
  for ( const pg::TableDifference& difference : runner.execute(pg::table_differences_statement<schema_t>().on(table)).get() ) {
    found.emplace_back(
        sch::get<"problem">(difference), sch::get<"column">(difference), sch::get<"expected">(difference), sch::get<"found">(difference)
    );
  }
  return found;
}

/** @brief Makes `table` with the columns `columns` (SQL, as written between a `CREATE TABLE`'s parentheses), in place of any there. */
template <sch::IsSchema schema_t>
void make(pg::Database& database, const sch::Table<schema_t>& table, const std::string& columns) {
  database.execute(pg::drop_table_statement<schema_t>().on(table)).get();
  database.execute(pg::unchecked_sql("CREATE TABLE " + table.name().quoted() + " (" + columns + ")")).get();
}

/** @brief Makes `table` from its schema, and checks that it then has no differences. */
template <sch::IsSchema schema_t>
void check_made_from_schema(pg::Database& database, const sch::Table<schema_t>& table) {
  database.execute(pg::drop_table_statement<schema_t>().on(table)).get();
  database.execute(pg::create_table_statement<schema_t>().on(table)).get();
  CHECK(differences(database, table).empty());
  database.execute(pg::drop_table_statement<schema_t>().on(table)).get();
}

}  // namespace

TEST_CASE("integration: a table made from its schema has no differences from it", "[integration][table_differences]") {
  pg::Database database(test_db());

  check_made_from_schema(database, sch::Table<t::People>("schemacht_it_check_people"));
  check_made_from_schema(database, sch::Table<t::Counted>("schemacht_it_check_counted"));    // a jsonb, and an unsigned as a bigint
  check_made_from_schema(database, sch::Table<t::Tagged>("schemacht_it_check_tagged"));      // an array
  check_made_from_schema(database, sch::Table<t::Numbered>("schemacht_it_check_numbered"));  // an identity key
  check_made_from_schema(database, sch::Table<t::Pairs>("schemacht_it_check_pairs"));        // a key of two columns
  check_made_from_schema(database, sch::Table<t::Keyless>("schemacht_it_check_keyless"));    // no key
  check_made_from_schema(database, sch::Table<Coded>("schemacht_it_check_coded_copy"));      // a type with a modifier, and an array of it

  // The convenience, on the schema's own table, and in a transaction.
  database.drop_table<Coded>().get();
  database.create_table<Coded>().get();
  CHECK(database.table_differences<Coded>().get().empty());

  pg::Transaction transaction = database.begin().get();
  CHECK(transaction.table_differences<Coded>().get().empty());
  transaction.commit().get();
  database.drop_table<Coded>().get();
}

TEST_CASE(
    "integration: a table's differences from its schema are each reported, by kind and then in the schema's order", "[integration][table_differences]"
) {
  using enum pg::TableProblem;
  pg::Database                database(test_db());
  const sch::Table<t::People> people("schemacht_it_check_differing");

  // People is: id bigint (the key), name text, nick text (may be NULL), score double precision, active boolean.
  make(
      database, people,
      "id integer NOT NULL, name text, nick text NOT NULL, score double precision NOT NULL, required integer NOT NULL, "
      "spare integer, defaulted integer NOT NULL DEFAULT 0, PRIMARY KEY (id, score)"
  );

  CHECK(
      differences(database, people) ==
      std::vector<Found>{
          {MissingColumn, "active", "boolean", ""},
          {WrongType, "id", "bigint", "integer"},
          {Nullability, "name", "NOT NULL", "nullable"},
          {Nullability, "nick", "nullable", "NOT NULL"},
          {PrimaryKey, "", "id", "id, score"},
          {ExtraRequiredColumn, "required", "", "integer"},  // not `spare` or `defaulted`: an insert goes in without them
      }
  );

  // No primary key at all.
  make(database, people, "id bigint NOT NULL, name text NOT NULL, nick text, score double precision NOT NULL, active boolean NOT NULL");
  CHECK(differences(database, people) == std::vector<Found>{{PrimaryKey, "", "id", ""}});

  // A key's INCLUDE columns are not part of it.
  make(
      database, people,
      "id bigint NOT NULL, name text NOT NULL, nick text, score double precision NOT NULL, active boolean NOT NULL, PRIMARY KEY (id) INCLUDE (name)"
  );
  CHECK(differences(database, people).empty());

  // A schema without a key says nothing of the table's.
  const sch::Table<t::Keyless> keyless("schemacht_it_check_differing");
  make(database, keyless, "id bigserial PRIMARY KEY, name text NOT NULL, score double precision NOT NULL");
  CHECK(differences(database, keyless).empty());

  database.execute(pg::drop_table_statement<t::People>().on(people)).get();
}

TEST_CASE("integration: a table that is not there is one difference, not an error", "[integration][table_differences]") {
  using enum pg::TableProblem;
  pg::Database database(test_db());

  const sch::Table<t::People> missing("schemacht_it_check_missing");
  database.execute(pg::drop_table_statement<t::People>().on(missing)).get();
  CHECK(differences(database, missing) == std::vector<Found>{{MissingTable, "", R"("schemacht_it_check_missing")", ""}});

  // Nor is a PostgreSQL schema that is not there.
  const sch::Table<t::People> nowhere("schemacht_it_check_no_schema", "people");
  CHECK(differences(database, nowhere) == std::vector<Found>{{MissingTable, "", R"("schemacht_it_check_no_schema"."people")", ""}});

  // A name that is something else's (a sequence's here) is no table.
  database.execute(pg::unchecked_sql("DROP SEQUENCE IF EXISTS schemacht_it_check_missing")).get();
  database.execute(pg::unchecked_sql("CREATE SEQUENCE schemacht_it_check_missing")).get();
  CHECK(differences(database, missing) == std::vector<Found>{{MissingTable, "", R"("schemacht_it_check_missing")", ""}});
  database.execute(pg::unchecked_sql("DROP SEQUENCE schemacht_it_check_missing")).get();
}

TEST_CASE("integration: types are compared as the database spells them, modifiers included", "[integration][table_differences]") {
  using enum pg::TableProblem;
  pg::Database database(test_db());

  // Other spellings of the same types.
  const sch::Table<t::People> people("schemacht_it_check_spelled");
  make(database, people, "id int8 PRIMARY KEY, name text NOT NULL, nick text, score float8 NOT NULL, active bool NOT NULL");
  CHECK(differences(database, people).empty());
  database.execute(pg::drop_table_statement<t::People>().on(people)).get();

  // A modifier that differs, on a column and on an array's elements.
  const sch::Table<Coded> coded("schemacht_it_check_modified");
  make(database, coded, "id bigint PRIMARY KEY, code character varying(4) NOT NULL, codes varchar(3)[]");
  CHECK(differences(database, coded) == std::vector<Found>{{WrongType, "code", "character varying(3)", "character varying(4)"}});

  make(database, coded, "id bigint PRIMARY KEY, code varchar(3) NOT NULL, codes varchar[]");
  CHECK(differences(database, coded) == std::vector<Found>{{WrongType, "codes", "character varying(3)[]", "character varying[]"}});
  database.execute(pg::drop_table_statement<Coded>().on(coded)).get();

  // A type the database does not have is reported, and fails nothing.
  const sch::Table<Strange> strange("schemacht_it_check_strange_copy");
  make(database, strange, "id bigint PRIMARY KEY, shape text NOT NULL");
  CHECK(differences(database, strange) == std::vector<Found>{{UnknownType, "shape", "schemacht_it_no_such_type(Point,4326)", "text"}});
  database.execute(pg::drop_table_statement<Strange>().on(strange)).get();
}

TEST_CASE("integration: a column the database fills in, or doesn't, against the schema's GENERATED", "[integration][table_differences]") {
  using enum pg::TableProblem;
  pg::Database database(test_db());

  // Numbered's id is GENERATED: never inserted, so the database must have something to fill it in with.
  const sch::Table<t::Numbered> numbered("schemacht_it_check_generated");
  make(database, numbered, "id integer PRIMARY KEY, name text NOT NULL, nick text");
  CHECK(differences(database, numbered) == std::vector<Found>{{NoDefault, "id", "", ""}});

  make(database, numbered, "id serial PRIMARY KEY, name text NOT NULL, nick text");  // a default
  CHECK(differences(database, numbered).empty());

  make(database, numbered, "id integer GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY, name text NOT NULL, nick text");
  CHECK(differences(database, numbered).empty());
  database.execute(pg::drop_table_statement<t::Numbered>().on(numbered)).get();

  // A column that may be NULL needs nothing to fill it in: an insert leaves it NULL.
  using Stamped = sch::Schema<"schemacht_it_check_stamped", t::Id, sch::Field<std::optional<std::int64_t>, "stamp", sch::KeyRole::None, true>>;
  const sch::Table<Stamped> stamped("schemacht_it_check_generated");
  make(database, stamped, "id bigint PRIMARY KEY, stamp bigint");
  CHECK(differences(database, stamped).empty());
  database.execute(pg::drop_table_statement<Stamped>().on(stamped)).get();

  // A stored expression fills a GENERATED column in; a plain NOT NULL column has nothing to.
  const sch::Table<t::WithGenerated> doubled("schemacht_it_check_generated");
  make(database, doubled, "id bigint PRIMARY KEY, doubled bigint NOT NULL GENERATED ALWAYS AS (id * 2) STORED, name text NOT NULL");
  CHECK(differences(database, doubled).empty());

  make(database, doubled, "id bigint PRIMARY KEY, doubled bigint NOT NULL, name text NOT NULL");
  CHECK(differences(database, doubled) == std::vector<Found>{{NoDefault, "doubled", "", ""}});
  database.execute(pg::drop_table_statement<t::WithGenerated>().on(doubled)).get();

  // People's id is inserted, which a column the database always computes refuses.
  const sch::Table<t::People> people("schemacht_it_check_generated");
  const std::string           rest = ", name text NOT NULL, nick text, score double precision NOT NULL, active boolean NOT NULL";
  make(database, people, "id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY" + rest);
  CHECK(differences(database, people) == std::vector<Found>{{AlwaysGenerated, "id", "", ""}});

  make(database, people, "id bigint GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY" + rest);  // a value may still be given
  CHECK(differences(database, people).empty());

  make(
      database, people,
      "id bigint PRIMARY KEY, name text NOT NULL, nick text, score double precision NOT NULL GENERATED ALWAYS AS (id * 2.0) STORED, "
      "active boolean NOT NULL"
  );
  CHECK(differences(database, people) == std::vector<Found>{{AlwaysGenerated, "score", "", ""}});
  database.execute(pg::drop_table_statement<t::People>().on(people)).get();
}

TEST_CASE("integration: a table in a PostgreSQL schema of its own is checked there", "[integration][table_differences]") {
  using enum pg::TableProblem;
  pg::Database database(test_db());
  database.execute(pg::unchecked_sql("DROP SCHEMA IF EXISTS schemacht_it_check CASCADE")).get();
  database.execute(pg::unchecked_sql("CREATE SCHEMA schemacht_it_check")).get();

  const sch::Table<t::Pairs> pairs("schemacht_it_check", "pairs");
  database.execute(pg::create_table_statement<t::Pairs>().on(pairs)).get();
  CHECK(differences(database, pairs).empty());

  database.execute(pg::unchecked_sql("ALTER TABLE schemacht_it_check.pairs DROP COLUMN v")).get();  // a dropped column is not there
  CHECK(differences(database, pairs) == std::vector<Found>{{MissingColumn, "v", "integer", ""}});

  database.execute(pg::unchecked_sql("DROP SCHEMA schemacht_it_check CASCADE")).get();
}

TEST_CASE("integration: a column of a domain is checked as the type the domain is over", "[integration][table_differences]") {
  using enum pg::TableProblem;
  pg::Database database(test_db());
  database.execute(pg::unchecked_sql("DROP SCHEMA IF EXISTS schemacht_it_check_domains CASCADE")).get();
  database.execute(pg::unchecked_sql("CREATE SCHEMA schemacht_it_check_domains")).get();
  for ( const std::string_view domain :
        {"short AS varchar(3)", "code AS schemacht_it_check_domains.short NOT NULL", "counted AS integer NOT NULL DEFAULT 7",
         "required AS integer NOT NULL"} ) {
    database.execute(pg::unchecked_sql("CREATE DOMAIN schemacht_it_check_domains." + std::string(domain))).get();
  }

  // `code` is a domain over a domain over varchar(3), and NOT NULL through it; a domain's default fills an extra column in.
  const sch::Table<Coded> coded("schemacht_it_check_domains", "coded");
  make(database, coded, "id bigint PRIMARY KEY, code schemacht_it_check_domains.code, codes varchar(3)[], spare schemacht_it_check_domains.counted");
  CHECK(differences(database, coded).empty());

  // A domain that is NOT NULL with no default makes an extra column one an insert fails on.
  database.execute(pg::unchecked_sql(R"(ALTER TABLE "schemacht_it_check_domains"."coded" ADD COLUMN needed schemacht_it_check_domains.required)"))
      .get();
  CHECK(differences(database, coded) == std::vector<Found>{{ExtraRequiredColumn, "needed", "", "integer"}});

  database.execute(pg::unchecked_sql("DROP SCHEMA schemacht_it_check_domains CASCADE")).get();
}

TEST_CASE("integration: of a view, only the columns and their types are checked", "[integration][table_differences]") {
  using enum pg::TableProblem;
  pg::Database                database(test_db());
  const sch::Table<t::People> people("schemacht_it_check_viewed");
  const sch::Table<t::People> view("schemacht_it_check_view");
  database.execute(pg::unchecked_sql("DROP VIEW IF EXISTS schemacht_it_check_view")).get();
  database.execute(pg::drop_table_statement<t::People>().on(people)).get();
  database.execute(pg::create_table_statement<t::People>().on(people)).get();

  // A view's columns are never NOT NULL, and it has no key: neither is a difference.
  database.execute(pg::unchecked_sql("CREATE VIEW schemacht_it_check_view AS SELECT * FROM schemacht_it_check_viewed")).get();
  CHECK(differences(database, view).empty());

  database.execute(pg::unchecked_sql("DROP VIEW schemacht_it_check_view")).get();
  database
      .execute(pg::unchecked_sql("CREATE VIEW schemacht_it_check_view AS SELECT id::integer AS id, name, nick, score FROM schemacht_it_check_viewed"))
      .get();
  CHECK(differences(database, view) == std::vector<Found>{{MissingColumn, "active", "boolean", ""}, {WrongType, "id", "bigint", "integer"}});

  database.execute(pg::unchecked_sql("DROP VIEW schemacht_it_check_view")).get();
  database.execute(pg::drop_table_statement<t::People>().on(people)).get();
}
