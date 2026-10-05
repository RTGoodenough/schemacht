// An extension's operators and functions in queries, an operation whose argument is not of the column's type, and one that
// tests an expression of its column.
//
// An operation is a type that names its SQL (see `query::Operation`); `col.apply<Op>(arg)` tests a column with it, and
// `col.ranked_by<Op>(arg)` sorts by it. Here:
//
// - pg_trgm's similarity of texts. `Similar` is a test written as an operator (`%`), `Distance` a ranking (`<->`, so a
//   `limit` gives "the closest N"), and `SimilarAbove` a test that also takes a number, which makes it a function: one of
//   the application's own, since pg_trgm keeps its threshold in a setting.
// - jsonb's own operators. `json::contains` (`@>`) is the library's. `HasKey` (`?`) is declared here, and its argument is a
//   key, not a document: `using argument_type = JsonKey;` makes the argument a `JsonKey`, sent as `text`. A PostGIS test of a
//   line column against a polygon is declared the same way.
// - A test of an expression of the column. `SameIgnoringCase` writes `lower(` and `)` around the column (`COLUMN_PREFIX`,
//   `COLUMN_SUFFIX`) and around the argument, which is the form an index on `lower("name")` answers. A PostGIS test of a
//   raster's outline (`ST_ConvexHull("rast")`), or of a geometry as a geography (`"geom"::geography`), is declared the same way.
//
// Run with a libpq connection string: schemacht_example_extensions "host=localhost dbname=app user=app"
// It installs pg_trgm if it can (and leaves it installed), creates the function similar_above and the table
// schemacht_example_streets, and drops those two when it is done. Without pg_trgm, it says so and leaves its part out.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

#include "schemacht/column_type.hpp"
#include "schemacht/schemacht.hpp"

namespace sch = schemacht::schema;
namespace q = schemacht::query;
namespace pg = schemacht::postgres;
namespace json = schemacht::json;

// ---- The operations, as an application declares them -----------------------------------------------------------------

namespace trgm {

/** @brief `"column" % $1::text`: similar enough, by the `pg_trgm.similarity_threshold` setting. */
struct Similar {
  static constexpr std::string_view OPERATOR = "%";
};

/** @brief `"column" <-> $1::text`: 1 minus the similarity, so the most similar sort first. */
struct Distance {
  static constexpr std::string_view OPERATOR = "<->";
};

/** @brief `similar_above("column", $1::text, $2::double precision)`: more similar than the number (`CreateSimilarAbove`). */
struct SimilarAbove {
  static constexpr std::string_view FUNCTION = "similar_above";
  static constexpr bool             TAKES_NUMBER = true;
};

}  // namespace trgm

/** @brief A key of a JSON object: the argument of `HasKey`. */
struct JsonKey {
  std::string value;
};

/** @brief A `JsonKey` is sent as the `text` it wraps. */
template <>
struct schemacht::ColumnType<JsonKey> : schemacht::WrappedColumn<&JsonKey::value> {};

/** @brief `"column" ? $1::text`: the document has the key. The column is `jsonb`; the argument is not. */
struct HasKey {
  static constexpr std::string_view OPERATOR = "?";
  using argument_type = JsonKey;
};

/** @brief `lower("column") = lower($1::text)`: the same text, whatever the case of either. */
struct SameIgnoringCase {
  static constexpr std::string_view OPERATOR = "=";
  static constexpr std::string_view COLUMN_PREFIX = "lower(";
  static constexpr std::string_view COLUMN_SUFFIX = ")";
  static constexpr std::string_view ARGUMENT_PREFIX = "lower(";
  static constexpr std::string_view ARGUMENT_SUFFIX = ")";
};

// ---- Using them --------------------------------------------------------------------------------------------------------

using Id = sch::Field<std::int64_t, "id", sch::KeyRole::Primary>;
using Name = sch::Field<std::string, "name">;
using Properties = sch::Field<json::Json, "properties">;
using Streets = sch::Schema<"schemacht_example_streets", Id, Name, Properties>;
using StreetColumns = q::On<Streets>;

// The three streets named most like an argument, the most alike first. One argument serves the test and the ranking.
constexpr auto NAMED_LIKE = q::select(StreetColumns::col<"name">().apply<trgm::Similar>(q::arg<0>()))
                                .order_by(StreetColumns::col<"name">().ranked_by<trgm::Distance>(q::arg<0>()))
                                .limit(3)
                                .project(StreetColumns::col<"name">());

// The streets whose name is more alike than a number.
constexpr auto NAMED_CLOSE_TO = q::select(StreetColumns::col<"name">().apply<trgm::SimilarAbove>(q::arg<0>(), q::arg<1>()))
                                    .order_by(StreetColumns::col<"id">().asc())
                                    .project(StreetColumns::col<"name">());

// The streets whose properties contain a document: those it has, with the same values.
constexpr auto WITH_PROPERTIES = q::select(json::contains(StreetColumns::col<"properties">(), q::arg<0>()))
                                     .order_by(StreetColumns::col<"id">().asc())
                                     .project(StreetColumns::col<"name">());

// The streets whose properties have a key, whatever its value.
constexpr auto WITH_KEY = q::select(StreetColumns::col<"properties">().apply<HasKey>(q::arg<0>()))
                              .order_by(StreetColumns::col<"id">().asc())
                              .project(StreetColumns::col<"name">());

// The streets with a name, in whatever case it was written. The rows come back as stored: only the test lowers the name.
constexpr auto NAMED = q::select(StreetColumns::col<"name">().apply<SameIgnoringCase>(q::arg<0>()))
                           .order_by(StreetColumns::col<"id">().asc())
                           .project(StreetColumns::col<"name">());

using CreateTrgm = q::RawStatement<"CREATE EXTENSION IF NOT EXISTS pg_trgm", q::RawArguments<>>;
using CreateSimilarAbove = q::RawStatement<
    "CREATE OR REPLACE FUNCTION similar_above(text, text, double precision) RETURNS boolean "
    "AS 'SELECT similarity($1, $2) > $3' LANGUAGE sql",
    q::RawArguments<>>;
using DropSimilarAbove = q::RawStatement<"DROP FUNCTION IF EXISTS similar_above(text, text, double precision)", q::RawArguments<>>;

namespace {

template <typename rows_t>
void print_names(std::string_view heading, const rows_t& rows) {
  std::cout << heading << '\n';
  for ( const auto& row : rows ) {
    std::cout << "  " << sch::get<"name">(row) << '\n';
  }
}

/** @return Whether pg_trgm is installed, installing it if it can. */
bool has_trgm(pg::Database& database) {
  try {
    database.execute(CreateTrgm::bind()).get();
    return true;
  } catch ( const pg::QueryError& error ) {
    std::cout << "pg_trgm is not available, so its part is left out: " << error.what() << '\n';
    return false;
  }
}

void drop_everything(pg::Database& database) {
  database.drop_table<Streets>().get();
  database.execute(DropSimilarAbove::bind()).get();
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const std::span<char*> args(argv, static_cast<std::size_t>(argc));
    const std::string      conninfo = args.size() > 1 ? args.subspan(1).front() : "";

    pg::Database database(conninfo);

    drop_everything(database);
    database.create_table<Streets>().get();
    database
        .insert<Streets>({
            Streets::make_row(1, "main street", json::Json(R"({"highway": "residential", "lit": true})")),
            Streets::make_row(2, "main st", json::Json(R"({"highway": "footway", "surface": "paved"})")),
            Streets::make_row(3, "maine road", json::Json(R"({"highway": "primary"})")),
            Streets::make_row(4, "mill lane", json::Json(R"({"highway": "footway", "surface": "gravel", "lit": false})")),
            Streets::make_row(5, "High Street", json::Json(R"({"highway": "primary", "lit": true})")),
        })
        .get();

    if ( has_trgm(database) ) {
      database.execute(CreateSimilarAbove::bind()).get();

      print_names("Named most like \"main street\":", database.execute(q::Prepared<NAMED_LIKE>::bind("main street")).get());
      print_names("Named more than 0.5 like \"main st\":", database.execute(q::Prepared<NAMED_CLOSE_TO>::bind("main st", 0.5)).get());
    }

    print_names(
        "Paved footways:", database.execute(q::Prepared<WITH_PROPERTIES>::bind(json::Json(R"({"highway": "footway", "surface": "paved"})"))).get()
    );
    print_names("Saying whether they are lit:", database.execute(q::Prepared<WITH_KEY>::bind(JsonKey{"lit"})).get());
    print_names("Named \"HIGH street\", in any case:", database.execute(q::Prepared<NAMED>::bind("HIGH street")).get());

    drop_everything(database);

    return EXIT_SUCCESS;
  } catch ( const std::exception& error ) {
    std::cerr << "error: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
