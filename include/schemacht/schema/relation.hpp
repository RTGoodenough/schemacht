#pragma once

#include <concepts>
#include <type_traits>

#include "schemacht/schema/schema.hpp"
#include "schemacht/util/compile_time.hpp"

namespace schemacht::schema {

/**
 * @brief Whether `feature_key`/`join_key` can actually form a `Relation`.
 *
 * - Both names must resolve to real fields.
 * - Those fields' `data_type`s must match.
 * - `feature_key` must be a key on the feature schema (`KeyRole::Primary`
 *   or `KeyRole::Unique`), not just some arbitrary column that happens to
 *   share a type -- mirroring what a real SQL foreign key requires.
 *
 * A named concept (rather than inlining this as `Relation`'s requires
 * clause) so tests can assert both the positive and negative cases
 * directly -- see `relation_test.cpp`.
 *
 * @tparam feature_schema_t The referenced ("one") side's schema.
 * @tparam join_schema_t    The referencing ("many") side's schema.
 * @tparam feature_key_str  Column name on `feature_schema_t`.
 * @tparam join_key_str     Column name on `join_schema_t`.
 */
template <typename feature_schema_t, typename join_schema_t, util::CTString feature_key_str, util::CTString join_key_str>
concept CanFormRelation = IsSchema<feature_schema_t> && IsSchema<join_schema_t> &&
                          std::same_as<typename feature_schema_t::template field_by_name<feature_key_str>::data_type,
                                       typename join_schema_t::template field_by_name<join_key_str>::data_type> &&
                          (feature_schema_t::template field_by_name<feature_key_str>::KEY_ROLE == KeyRole::Primary ||
                           feature_schema_t::template field_by_name<feature_key_str>::KEY_ROLE == KeyRole::Unique);

/**
 * @brief Pairs two schemas joined by a key.
 *
 * - Typical use: a feature table and the index table that points back at it
 *   (a feature can be covered by many index entries, so that can't be a single
 *   column on the feature schema itself).
 * - Not domain-specific: any one-to-many key-based join fits this shape.
 *
 * @tparam feature_schema_t The referenced ("one") side's schema.
 * @tparam join_schema_t    The referencing ("many") side's schema.
 * @tparam feature_key_str  Column on `feature_schema_t` the join is performed on.
 * @tparam join_key_str     Column on `join_schema_t` the join is performed on.
 * @see CanFormRelation for what's checked at compile time.
 */
template <IsSchema feature_schema_t, IsSchema join_schema_t, util::CTString feature_key_str, util::CTString join_key_str>
  requires CanFormRelation<feature_schema_t, join_schema_t, feature_key_str, join_key_str>
class Relation {
 public:
  using feature_schema = feature_schema_t;
  using join_schema = join_schema_t;

  static constexpr util::CTString FEATURE_KEY{feature_key_str};
  static constexpr util::CTString JOIN_KEY{join_key_str};
};

namespace detail {
template <typename t>
struct is_relation : std::false_type {};

template <typename feature_schema_t, typename join_schema_t, util::CTString feature_key_str, util::CTString join_key_str>
struct is_relation<Relation<feature_schema_t, join_schema_t, feature_key_str, join_key_str>> : std::true_type {};
}  // namespace detail

/** @brief Detects whether `data_t` is a `Relation<...>` specialization. */
template <typename data_t>
concept IsRelation = detail::is_relation<data_t>::value;

}  // namespace schemacht::schema
