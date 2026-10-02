// Schema and relation facts, checked while compiling.

#include <catch2/catch_test_macros.hpp>

#include <concepts>
#include <cstdint>
#include <string>

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

using Composite = sch::Schema<"composite", sch::Field<std::int64_t, "a", sch::KeyRole::Primary>, sch::Field<std::int64_t, "b", sch::KeyRole::Primary>,
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

TEST_CASE("which relations can be formed", "[schema]") {
  STATIC_REQUIRE(sch::CanFormRelation<Features, Index, "id", "feature_id">);
  STATIC_REQUIRE(sch::CanFormRelation<Features, Index, "code", "feature_code">);   // a unique key will do
  STATIC_REQUIRE(! sch::CanFormRelation<Features, Index, "label", "feature_label">);  // not a key
  STATIC_REQUIRE(! sch::CanFormRelation<Features, Index, "id", "feature_code">);   // the types differ
  STATIC_REQUIRE(! sch::CanFormRelation<Features, Index, "nope", "feature_id">);   // no such column: false, not a compile error
  STATIC_REQUIRE(! sch::CanFormRelation<Features, Index, "id", "nope">);
  STATIC_REQUIRE(! sch::CanFormRelation<int, Index, "id", "feature_id">);  // not a schema

  using FeatureIndex = sch::Relation<Features, Index, "id", "feature_id">;
  STATIC_REQUIRE(sch::IsRelation<FeatureIndex>);
}
