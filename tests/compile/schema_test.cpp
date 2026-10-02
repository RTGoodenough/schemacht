// Schema facts, checked while compiling.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>

#include "schemacht/schema.hpp"
#include "support/schemas.hpp"

namespace sch = schemacht::schema;
namespace t = schemacht::test;

namespace {

using FeatureId = sch::Field<std::int64_t, "id", sch::KeyRole::Primary>;
using Code = sch::Field<std::string, "code", sch::KeyRole::Unique>;
using Label = sch::Field<std::string, "label">;
using Features = sch::Schema<"features", FeatureId, Code, Label>;

using FeatureRef = sch::Field<std::int64_t, "feature_id">;
using CodeRef = sch::Field<std::string, "feature_code">;
using LabelRef = sch::Field<std::string, "feature_label">;
using Index = sch::Schema<"feature_index", FeatureRef, CodeRef, LabelRef>;

using Composite = sch::Schema<
    "composite", sch::Field<std::int64_t, "a", sch::KeyRole::Primary>, sch::Field<std::int64_t, "b", sch::KeyRole::Primary>,
    sch::Field<std::string, "c">>;

}  // namespace

TEST_CASE("column lookup", "[schema]") {
  STATIC_REQUIRE(t::People::FIELD_COUNT == 5);
  STATIC_REQUIRE(t::People::TABLE_NAME == "people");
  STATIC_REQUIRE(t::People::field_index<"score">() == 3);
  STATIC_REQUIRE(t::People::field_index<"nope">() == t::People::FIELD_COUNT);
  STATIC_REQUIRE(t::People::has_column<"nick">());
  STATIC_REQUIRE(! t::People::has_column<"nope">());
  STATIC_REQUIRE(std::same_as<t::People::field_by_name<"nick">, t::Nick>);
  STATIC_REQUIRE(t::People::column_names().at(4) == "active");
}

TEST_CASE("keys and generated columns", "[schema]") {
  STATIC_REQUIRE(t::People::HAS_PRIMARY_KEY);
  STATIC_REQUIRE(t::People::PRIMARY_KEY_FIELD_COUNT == 1);
  STATIC_REQUIRE(Composite::PRIMARY_KEY_FIELD_COUNT == 2);
  STATIC_REQUIRE(Composite::primary_key_column_names().at(1) == "b");
  STATIC_REQUIRE(! Index::HAS_PRIMARY_KEY);

  STATIC_REQUIRE(t::WithGenerated::INSERTABLE_FIELD_COUNT == 2);
  STATIC_REQUIRE(t::WithGenerated::insertable_column_names().at(1) == "name");
}

TEST_CASE("nullable fields", "[schema]") {
  STATIC_REQUIRE(t::Nick::NULLABLE);
  STATIC_REQUIRE(std::same_as<t::Nick::value_type, std::string>);
  STATIC_REQUIRE(! t::Name::NULLABLE);
}

TEST_CASE("unique columns are each unique on their own", "[schema]") {
  STATIC_REQUIRE(Features::UNIQUE_FIELD_COUNT == 1);
  STATIC_REQUIRE(Features::unique_column_names() == std::array<std::string_view, 1>{"code"});
  STATIC_REQUIRE(Index::UNIQUE_FIELD_COUNT == 0);
}

TEST_CASE("rows are made from a plain value per column", "[schema]") {
  using Row = t::People::row_type;

  STATIC_REQUIRE(sch::RowValues<Row, int, const char*, std::nullopt_t, double, bool>);
  STATIC_REQUIRE(sch::RowValues<Row, std::int64_t, std::string, std::string, float, bool>);      // a float widens to the double
  STATIC_REQUIRE_FALSE(sch::RowValues<Row, int, const char*, std::nullopt_t, double>);           // too few
  STATIC_REQUIRE_FALSE(sch::RowValues<Row, double, std::string, std::nullopt_t, double, bool>);  // a double would narrow to the id
  STATIC_REQUIRE_FALSE(sch::RowValues<Row, int, std::nullopt_t, std::nullopt_t, double, bool>);  // the name cannot be NULL
  STATIC_REQUIRE_FALSE(sch::RowValues<int, int>);
}

TEST_CASE("an insert row leaves out the generated columns", "[schema]") {
  STATIC_REQUIRE(std::same_as<t::WithGenerated::insert_row_type, std::tuple<t::Id, t::Name>>);
  STATIC_REQUIRE(t::WithGenerated::INSERTABLE_INDICES == std::array<std::size_t, 2>{0, 2});
  STATIC_REQUIRE(sch::HasGeneratedColumn<t::WithGenerated>);

  STATIC_REQUIRE(std::same_as<t::People::insert_row_type, t::People::row_type>);  // nothing generated
  STATIC_REQUIRE_FALSE(sch::HasGeneratedColumn<t::People>);

  STATIC_REQUIRE(sch::RowValues<t::WithGenerated::insert_row_type, int, const char*>);
  STATIC_REQUIRE_FALSE(sch::RowValues<t::WithGenerated::insert_row_type, int, int, const char*>);  // no value for the generated column

  const t::Numbered::insert_row_type row = t::Numbered::make_insert_row("ann", std::nullopt);
  CHECK(sch::get<"name">(row) == "ann");
  CHECK_FALSE(sch::get<"nick">(row).has_value());
}

TEST_CASE("a row's columns are found by name", "[schema]") {
  STATIC_REQUIRE(sch::HasColumnNamed<t::People::row_type, "nick">);
  STATIC_REQUIRE(sch::HasColumnNamed<std::tuple<t::Nick, t::Id>, "id">);  // a projected row
  STATIC_REQUIRE_FALSE(sch::HasColumnNamed<t::People::row_type, "nope">);
  STATIC_REQUIRE_FALSE(sch::HasColumnNamed<std::tuple<t::Id, t::Id>, "id">);  // ambiguous
  STATIC_REQUIRE_FALSE(sch::HasColumnNamed<int, "id">);
}

TEST_CASE("a single-column primary key is known by its field", "[schema]") {
  STATIC_REQUIRE(sch::HasSingleColumnKey<Features>);
  STATIC_REQUIRE_FALSE(sch::HasSingleColumnKey<Composite>);
  STATIC_REQUIRE_FALSE(sch::HasSingleColumnKey<Index>);  // no key at all
  STATIC_REQUIRE(std::same_as<sch::primary_key_field<Features>, FeatureId>);
  STATIC_REQUIRE(std::same_as<sch::primary_key_field<t::WithGenerated>, t::Id>);
}
