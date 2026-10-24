#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <exception>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "schemacht/postgres/concepts/statements.hpp"  // IWYU pragma: export
#include "schemacht/postgres/field_codec.hpp"
#include "schemacht/query/compiled_query.hpp"
#include "schemacht/query/raw_statement.hpp"
#include "schemacht/query/sql.hpp"
#include "schemacht/query/sql_type.hpp"
#include "schemacht/schema/schema.hpp"
#include "schemacht/schema/table.hpp"
#include "schemacht/schema/table_name.hpp"
#include "schemacht/util/compile_time.hpp"
#include "schemacht/util/identifier.hpp"

/**
 * @file
 * Turning bound queries and rows into what is sent to Postgres: statement text plus each argument's text. Free of libpq.
 */
namespace schemacht::postgres::detail {

/** @brief Appends a bound query argument's text, in the form Postgres reads for that type. */
class TextBinder {
 public:
  explicit TextBinder(std::vector<std::string>* texts) : _texts(texts) {}

  void operator()(std::monostate /*unset*/) const { throw std::logic_error("query argument was never set"); }

  /** @brief A list argument (a `std::vector`, other than `bytea`'s) becomes one array parameter (`= ANY($n)`), not one per element. */
  template <HasTextForm data_t>
  void operator()(const data_t& value) const {
    _texts->push_back(format_field<data_t>(value));
  }

  /** @brief An operation's argument of a type of its own: formatted when it was bound. */
  void operator()(const query::ArgumentText& value) const { _texts->push_back(value.text); }

 private:
  std::vector<std::string>* _texts;

 public:
  TextBinder(const TextBinder&) = default;
  TextBinder(TextBinder&&) noexcept = default;
  TextBinder& operator=(const TextBinder&) = default;
  TextBinder& operator=(TextBinder&&) noexcept = default;
  ~TextBinder() = default;
};

/** @return The text of each of `query`'s bound arguments, in placeholder order: element 0 is `$1`. */
template <schema::IsSchema schema_t>
[[nodiscard]] std::vector<std::string> param_texts(const query::CompiledQuery<schema_t>& query) {
  std::vector<std::string> texts;
  texts.reserve(query.params().size());

  for ( const auto& param : query.params() ) {
    std::visit(TextBinder{&texts}, param);
  }

  return texts;
}

namespace insert_detail {

template <schema::IsSchema schema_t, std::size_t... idx>
consteval bool every_insertable_has_sql_type(std::index_sequence<idx...> /*indices*/) {
  return (
      (schema_t::template field_at<idx>::GENERATED || ! query::sql::type_name<typename schema_t::template field_at<idx>::value_type>().empty()) && ...
  );
}

/** @brief The INSERT for `schema_t` (see `query::sql::insert_unnest_statement`), as a static string. */
template <schema::IsSchema schema_t>
class UnnestSql {
  static constexpr auto PARTS = [] {
    constexpr auto TEXT = query::sql::insert_unnest_statement<schema_t>();
    return std::pair{query::sql::terminated<TEXT.size()>(TEXT), TEXT.table_slot()};
  }();

 public:
  static constexpr std::string_view      VALUE{PARTS.first.data(), PARTS.first.size() - 1};
  static constexpr query::sql::TableSlot TABLE_SLOT = PARTS.second;  ///< Where `VALUE` names the table (see `query::sql::on_table`).
};

/** @return The operation that merges a column by `rule_t` (a `MergeWith`), or null for a column it keeps (a `Keep`) or a `ConflictOn`. */
template <UpsertRule rule_t>
[[nodiscard]] consteval const query::Operation* merge_operation() {
  if constexpr ( IsMergeWith<rule_t> ) {
    return &query::detail::BoundOperation<typename rule_t::merge_type>::VALUE;  // which checks its spelling
  } else {
    return nullptr;
  }
}

/** @return Whether `rule_t` merges by a function or operator spelled as it must be; a `Keep` or a `ConflictOn` names none. */
template <UpsertRule rule_t>
[[nodiscard]] consteval bool spelled_validly() {
  if constexpr ( IsMergeWith<rule_t> ) {
    return query::detail::spelled_validly<typename rule_t::merge_type>();
  } else {
    return true;
  }
}

/** @return The index of the column a `ConflictOn` in `rule_ts` names (the first, if several), or none: rows are matched on the primary key. */
template <schema::IsSchema schema_t, UpsertRule... rule_ts>
[[nodiscard]] consteval std::optional<std::size_t> matched_column() {
  std::optional<std::size_t> matched;
  (
      [&] {
        if constexpr ( IsConflictOn<rule_ts> ) {
          if ( ! matched ) {
            matched = schema_t::template checked_field_index<rule_ts::COLUMN>();
          }
        }
      }(),
      ...);
  return matched;
}

/**
 * @brief The upsert for `schema_t` that matches rows and treats columns as `rule_ts` say (see `query::sql::upsert_statement`), as
 * a static string.
 */
template <schema::IsSchema schema_t, UpsertRule... rule_ts>
class UpsertSql {
  static constexpr std::size_t CONFLICT_COUNT = (std::size_t{0} + ... + std::size_t{IsConflictOn<rule_ts>});
  static constexpr std::size_t MERGE_COUNT = sizeof...(rule_ts) - CONFLICT_COUNT;

  static constexpr std::array<query::sql::Merge, MERGE_COUNT> MERGES = [] {
    std::array<query::sql::Merge, MERGE_COUNT> merges{};
    std::size_t                                next = 0;
    (
        [&] {
          if constexpr ( ! IsConflictOn<rule_ts> ) {
            merges.at(next++) = query::sql::Merge{
                .column = schema_t::template checked_field_index<rule_ts::COLUMN>(),
                .operation = merge_operation<rule_ts>(),
            };
          }
        }(),
        ...);
    return merges;
  }();

  static constexpr std::optional<std::size_t> MATCHED = matched_column<schema_t, rule_ts...>();

  [[nodiscard]] static constexpr bool is_key(std::size_t column) { return schema_t::KEY_ROLES.at(column) == schema::KeyRole::Primary; }

  [[nodiscard]] static constexpr bool is_generated(std::size_t column) { return query::sql::detail::COLUMN_GENERATED<schema_t>.at(column); }

  [[nodiscard]] static constexpr bool is_matched(std::size_t column) { return column == MATCHED; }

  /** @return Whether the update may set `column`: neither a primary key column, nor generated, nor the one matched on. */
  [[nodiscard]] static constexpr bool is_updatable(std::size_t column) { return ! is_key(column) && ! is_generated(column) && ! is_matched(column); }

  [[nodiscard]] static consteval bool key_generated() {
    for ( std::size_t column = 0; column < schema_t::FIELD_COUNT; ++column ) {
      if ( is_key(column) && is_generated(column) ) {
        return true;
      }
    }
    return false;
  }

  /** @return How many columns the update sets when no rule keeps any. */
  [[nodiscard]] static consteval std::size_t updatable_count() {
    std::size_t count = 0;
    for ( std::size_t column = 0; column < schema_t::FIELD_COUNT; ++column ) {
      if ( is_updatable(column) ) {
        ++count;
      }
    }
    return count;
  }

  /** @return How many of the columns the update may set a `Keep` keeps (a `Keep` on another column has its own error). */
  [[nodiscard]] static consteval std::size_t kept_count() {
    return std::ranges::count_if(MERGES, [](const query::sql::Merge& merge) { return merge.operation == nullptr && is_updatable(merge.column); });
  }

  [[nodiscard]] static consteval bool distinct_columns() {
    std::array<std::size_t, MERGE_COUNT> columns{};
    std::ranges::transform(MERGES, columns.begin(), &query::sql::Merge::column);
    std::ranges::sort(columns);

    return std::ranges::adjacent_find(columns) == columns.end();
  }

  // A rule for a column the schema doesn't have, or a misspelt function or operator, fails on its own (`checked_field_index`,
  // `BoundOperation`); no second error follows.
  static constexpr bool KNOWN_COLUMNS = (schema_t::template has_column<rule_ts::COLUMN>() && ...);
  static constexpr bool SPELLED = (insert_detail::spelled_validly<rule_ts>() && ...);
  static constexpr bool CHECKED = KNOWN_COLUMNS && SPELLED;  // the rules themselves are then checked

  static_assert(CONFLICT_COUNT <= 1, "upsert: two ConflictOn; rows are matched on one column, or on the primary key");
  static_assert(
      ! CHECKED || ! MATCHED || schema_t::KEY_ROLES.at(*MATCHED) == schema::KeyRole::Unique,
      "upsert: ConflictOn names a column that is not KeyRole::Unique; rows are matched on one Unique column, or on the primary key without a "
      "ConflictOn"
  );
  static_assert(
      ! CHECKED || ! MATCHED || ! is_generated(*MATCHED),
      "upsert: the database computes the ConflictOn column, so a row inserted never carries one to match"
  );
  static_assert(
      MATCHED || schema_t::HAS_PRIMARY_KEY,
      "upsert: the schema has no primary key, which is what a row is matched on to the one already there, unless a ConflictOn names a Unique column"
  );
  static_assert(
      MATCHED || ! key_generated(),
      "upsert: the database numbers the primary key, so a row inserted never carries one to match; match on a Unique column with ConflictOn"
  );
  static_assert(
      ! CHECKED || updatable_count() > 0,
      "upsert: every column is matched on, part of the primary key or generated, so a row already there has nothing to update"
  );
  static_assert(
      ! CHECKED || std::ranges::none_of(MERGES, [](const query::sql::Merge& merge) { return is_key(merge.column); }),
      "upsert: a MergeWith or Keep names a primary key column, which is never updated"
  );
  static_assert(
      ! CHECKED || ! MATCHED || std::ranges::none_of(MERGES, [](const query::sql::Merge& merge) { return merge.column == *MATCHED; }),
      "upsert: a MergeWith or Keep names the ConflictOn column, which rows are matched on and is never updated"
  );
  static_assert(
      ! CHECKED || std::ranges::none_of(MERGES, [](const query::sql::Merge& merge) { return is_generated(merge.column); }),
      "upsert: a MergeWith or Keep names a GENERATED column, which the database computes"
  );
  static_assert(! CHECKED || distinct_columns(), "upsert: two of its MergeWith and Keep name the same column");
  static_assert(
      ! CHECKED || ! distinct_columns() || updatable_count() == 0 || updatable_count() > kept_count(),
      "upsert: every column besides those matched on is kept (Keep), so a row already there has nothing to update"
  );

  [[nodiscard]] static consteval auto text() {
    if constexpr ( SPELLED ) {
      return query::sql::upsert_statement<schema_t>(MERGES, MATCHED);
    } else {
      return query::sql::upsert_statement<schema_t>(std::array<query::sql::Merge, 0>{}, MATCHED);  // after the spelling's error
    }
  }

  static constexpr auto PARTS = [] {
    constexpr auto TEXT = text();
    return std::pair{query::sql::terminated<TEXT.size()>(TEXT), TEXT.table_slot()};
  }();

 public:
  static constexpr std::string_view      VALUE{PARTS.first.data(), PARTS.first.size() - 1};
  static constexpr query::sql::TableSlot TABLE_SLOT = PARTS.second;  ///< Where `VALUE` names the table (see `query::sql::on_table`).
};

/** @brief Where the `sent`-th inserted column is in a `row_t`: an `insert_row_type` holds only those columns, a `row_type` every one. */
template <schema::IsSchema schema_t, schema::InsertRowOf<schema_t> row_t, std::size_t sent>
inline constexpr std::size_t POSITION = std::same_as<row_t, typename schema_t::insert_row_type> ? sent : schema_t::INSERTABLE_INDICES.at(sent);

/** @brief The field of the `sent`-th inserted column. */
template <schema::IsSchema schema_t, schema::InsertRowOf<schema_t> row_t, std::size_t sent>
using sent_field = std::tuple_element_t<POSITION<schema_t, row_t, sent>, row_t>;

/**
 * @return The array literal of the `sent`-th inserted column's value in every row (`NULL` for a NULL), for the INSERT. A value that
 * is itself an array is one element: its own array literal, quoted.
 */
template <schema::IsSchema schema_t, schema::InsertRowOf<schema_t> row_t, std::size_t sent>
[[nodiscard]] std::string column_array(std::span<const row_t> rows) {
  using field_t = sent_field<schema_t, row_t, sent>;
  std::string literal = "{";
  bool        first = true;

  for ( const row_t& row : rows ) {
    if ( ! first ) {
      literal += ',';
    }
    first = false;

    if ( const auto* value = schema::value_if_present(std::get<POSITION<schema_t, row_t, sent>>(row)); value != nullptr ) {
      postgres::detail::append_array_element(literal, format_field<typename field_t::value_type>(*value));

    } else {
      literal += "NULL";
    }
  }

  literal += '}';

  return literal;
}

/** @return The arguments of the INSERT of `rows`: the array literal of each inserted column. */
template <schema::IsSchema schema_t, schema::InsertRowOf<schema_t> row_t, std::size_t... sent>
[[nodiscard]] std::vector<std::string> column_arrays(std::span<const row_t> rows, std::index_sequence<sent...> /*columns*/) {
  return {column_array<schema_t, row_t, sent>(rows)...};
}

}  // namespace insert_detail

/** @brief `CREATE TABLE` for `schema_t` (see `query::sql::create_table_statement`), as a static string. */
template <schema::IsSchema schema_t>
class CreateTableSql {
  static constexpr auto PARTS = [] {
    constexpr auto TEXT = query::sql::create_table_statement<schema_t>();
    return std::pair{query::sql::terminated<TEXT.size()>(TEXT), TEXT.table_slot()};
  }();

 public:
  static constexpr std::string_view      VALUE{PARTS.first.data(), PARTS.first.size() - 1};
  static constexpr query::sql::TableSlot TABLE_SLOT = PARTS.second;  ///< Where `VALUE` names the table (see `query::sql::on_table`).
};

/** @brief ` RETURNING <every column>` for `schema_t`, as a static string: what `insert_returning_statement` adds to its INSERT. */
template <schema::IsSchema schema_t>
class ReturningSql {
  static constexpr auto CHARS = [] {
    constexpr auto TEXT = query::sql::returning_every_column<schema_t>();
    return query::sql::terminated<TEXT.size()>(TEXT);
  }();

 public:
  static constexpr std::string_view VALUE{CHARS.data(), CHARS.size() - 1};
};

/** @brief `DROP TABLE IF EXISTS` for `schema_t`, as a static string. */
template <schema::IsSchema schema_t>
class DropTableSql {
  static constexpr auto PARTS = [] {
    constexpr auto TEXT = query::sql::drop_table_statement<schema_t>();
    return std::pair{query::sql::terminated<TEXT.size()>(TEXT), TEXT.table_slot()};
  }();

 public:
  static constexpr std::string_view      VALUE{PARTS.first.data(), PARTS.first.size() - 1};
  static constexpr query::sql::TableSlot TABLE_SLOT = PARTS.second;  ///< Where `VALUE` names the table (see `query::sql::on_table`).
};

}  // namespace schemacht::postgres::detail

namespace schemacht::postgres {

namespace detail {

struct SchemaStatementMaker;  // below

}  // namespace detail

/**
 * @brief A statement made from a schema rather than written as a query: its `CREATE TABLE`, `DROP TABLE`, `INSERT` or upsert
 * (see `create_table_statement`, `drop_table_statement`, `insert_statement`, `upsert_statement`). It runs as a query does
 * (`execute`), on the schema's own table or, aimed with `on`, on another of its shape.
 *
 * One that could not be made (a value of an insert that its codec refused) holds why, and fails with that when it runs: the
 * error reaches the completion, or the future, as any error of a statement does.
 * @tparam result_t What each row it returns is: `std::tuple<>` for none (`execute` gives the count of rows it affected), or the
 * schema's `row_type` for an insert that gives back what it wrote.
 */
template <schema::IsSchema schema_t, typename result_t>
class SchemaStatement {
 public:
  using schema_type = schema_t;
  using result_type = result_t;

  [[nodiscard]] std::string_view sql() const noexcept { return _sql; }

  /** @return The text of each argument, in placeholder order: `params().front()` is `$1`. */
  [[nodiscard]] std::span<const std::string> params() const noexcept { return _params; }

  /** @return The arguments, taken out of this statement (which is then left without them): to send them without a copy. */
  [[nodiscard]] std::vector<std::string> take_params() && noexcept { return std::move(_params); }

  /** @return Why the statement could not be made, or null. */
  [[nodiscard]] const std::exception_ptr& error() const noexcept { return _error; }

  /** @return This statement on `table` instead of its schema's own: the same statement and arguments, naming `table`. */
  [[nodiscard]] SchemaStatement on(const schema::Table<schema_t>& table) const& {
    SchemaStatement aimed = *this;
    aimed.aim(table.name());
    return aimed;
  }

  /** @brief As above, moving the arguments rather than copying them. */
  [[nodiscard]] SchemaStatement on(const schema::Table<schema_t>& table) && {
    aim(table.name());
    return std::move(*this);
  }

  /** @brief Refused: a table of another schema's shape. */
  template <schema::IsSchema other_t>
  void on(const schema::Table<other_t>& /*table*/) const {
    static_assert(
        std::same_as<other_t, schema_t>,
        "a statement runs only on a table of its own schema's shape: aim it at a schema::Table of the schema it was written against"
    );
  }

 private:
  friend struct detail::SchemaStatementMaker;

  std::string              _sql;
  query::sql::TableSlot    _table_slot;
  std::vector<std::string> _params;
  std::exception_ptr       _error;

  /**
   * @param sql        The statement, with `$n` placeholders for `params`.
   * @param table_slot Where `sql` names the schema's table (see `query::sql::on_table`).
   * @param params     The text of each argument, in placeholder order.
   */
  SchemaStatement(std::string sql, query::sql::TableSlot table_slot, std::vector<std::string> params)
      : _sql(std::move(sql)), _table_slot(table_slot), _params(std::move(params)) {}

  /** @brief A statement that could not be made, for `error`: running it fails with that. */
  SchemaStatement(std::string sql, query::sql::TableSlot table_slot, std::exception_ptr error)
      : _sql(std::move(sql)), _table_slot(table_slot), _error(std::move(error)) {}

  void aim(const schema::TableName& table) {
    _sql = query::sql::on_table(_sql, _table_slot, table);
    _table_slot.size = table.quoted().size();
  }
};

namespace detail {

/**
 * @brief Makes `SchemaStatement`s, whose constructors are private: only schemacht's own statement makers (`insert_statement`,
 * `create_table_statement`, ...) make one, so its SQL and where that names the table always agree.
 */
struct SchemaStatementMaker {
  template <schema::IsSchema schema_t, typename result_t, typename arguments_t>
  [[nodiscard]] static SchemaStatement<schema_t, result_t> make(std::string sql, query::sql::TableSlot table_slot, arguments_t arguments) {
    return SchemaStatement<schema_t, result_t>(std::move(sql), table_slot, std::move(arguments));  // the arguments, or the error
  }
};

/** @return The arguments of the INSERT of `rows`: the array literal of each inserted column. */
template <TextSchema schema_t, std::ranges::contiguous_range rows_t>
  requires schema::InsertRowOf<std::ranges::range_value_t<rows_t>, schema_t>
[[nodiscard]] std::vector<std::string> insert_params(const rows_t& rows) {
  static_assert(schema_t::INSERTABLE_FIELD_COUNT > 0, "insert: the schema has no column that is not GENERATED");
  static_assert(
      insert_detail::every_insertable_has_sql_type<schema_t>(std::make_index_sequence<schema_t::FIELD_COUNT>{}),
      "insert: every inserted column's type needs a SQL type name (it is sent as an array of that type): give it "
      "one (ColumnType::SQL_TYPE, or a query::sql::SqlType specialization)"
  );

  using row_t = std::ranges::range_value_t<rows_t>;
  return insert_detail::column_arrays<schema_t, row_t>(std::span<const row_t>(rows), std::make_index_sequence<schema_t::INSERTABLE_FIELD_COUNT>{});
}

namespace insert_detail {

/**
 * @return The insert `sql` of `rows`, which names its table at `table_slot`; or, if their arguments could not be made (a value its
 * codec refused), a statement that fails with that when it runs. The arguments are made first, so that `sql` is still whole for
 * the one that fails.
 */
template <TextSchema schema_t, typename result_t, std::ranges::contiguous_range rows_t>
[[nodiscard]] SchemaStatement<schema_t, result_t> made(std::string sql, query::sql::TableSlot table_slot, const rows_t& rows) {
  std::vector<std::string> params;
  try {
    params = insert_params<schema_t>(rows);
  } catch ( ... ) {
    return SchemaStatementMaker::make<schema_t, result_t>(std::move(sql), table_slot, std::current_exception());
  }

  return SchemaStatementMaker::make<schema_t, result_t>(std::move(sql), table_slot, std::move(params));
}

}  // namespace insert_detail

}  // namespace detail

/**
 * @return The statement that inserts `rows` (`row_type`s, or `insert_row_type`s without the generated columns): one statement,
 * `INSERT ... FROM unnest(...)` with one array argument per column, whatever the schema and however many rows, inserted
 * atomically. Generated columns are never sent: the database fills them in. `execute` gives the count of rows inserted.
 */
template <TextSchema schema_t, std::ranges::contiguous_range rows_t>
  requires schema::InsertRowOf<std::ranges::range_value_t<rows_t>, schema_t>
[[nodiscard]] SchemaStatement<schema_t, std::tuple<>> insert_statement(const rows_t& rows) {
  using sql_t = detail::insert_detail::UnnestSql<schema_t>;
  return detail::insert_detail::made<schema_t, std::tuple<>>(std::string(sql_t::VALUE), sql_t::TABLE_SLOT, rows);
}

/**
 * @return As `insert_statement`, giving back every column of each row it writes (`RETURNING`), generated ones included: the key
 * the database chose, say. PostgreSQL returns one row per row inserted, but does not promise their order: match several by a
 * column they were given (a unique one), not by position.
 */
template <TextSchema schema_t, std::ranges::contiguous_range rows_t>
  requires schema::InsertRowOf<std::ranges::range_value_t<rows_t>, schema_t>
[[nodiscard]] SchemaStatement<schema_t, typename schema_t::row_type> insert_returning_statement(const rows_t& rows) {
  using sql_t = detail::insert_detail::UnnestSql<schema_t>;
  return detail::insert_detail::made<schema_t, typename schema_t::row_type>(
      std::string(sql_t::VALUE) + std::string(detail::ReturningSql<schema_t>::VALUE), sql_t::TABLE_SLOT, rows
  );
}

/**
 * @brief How `upsert_statement` sets the column `column` of a row already in the table: merged with the value that would have
 * gone in, by the SQL function or operator `merge_t` names (`FUNCTION` or `OPERATOR`, one of the two, as an operation names its
 * test). A function is called `FUNCTION("current"."column", excluded."column")`, the row there first; an operator is written
 * `"current"."column" OPERATOR excluded."column"`. The function may be the application's own (`CREATE FUNCTION`), and must
 * give the column's type. Named alone, it is found through the search path; named with its schema (`stats.add_counts`), it is
 * called there whatever the search path is, and whichever schema the table is in.
 *
 * In a column that can be NULL, a NULL on either side makes an operator's result NULL, and a function's if it is `STRICT`; a
 * function that keeps the value that isn't NULL handles it (`coalesce`, or a `CASE`).
 *
 * @code
 * struct KeepLonger {
 *   static constexpr std::string_view FUNCTION = "keep_longer";  // keep_longer(current text, incoming text) returns text
 * };
 * struct Add {
 *   static constexpr std::string_view OPERATOR = "+";
 * };
 * db.execute(pg::upsert_statement<Notes, pg::MergeWith<"body", KeepLonger>, pg::MergeWith<"hits", Add>>(rows));
 * @endcode
 */
template <util::CTString column, MergeOperation merge_t>
struct MergeWith {
  static constexpr util::CTString COLUMN = column;  ///< The column merged.
  using merge_type = merge_t;                       ///< What merges it.
};

/**
 * @brief Leaves the column `column` of a row already in the table as it is, when `upsert_statement` updates that row: the value
 * that would have gone in is used only for a row inserted. For what is set once, such as when a row was first made.
 */
template <util::CTString column>
struct Keep {
  static constexpr util::CTString COLUMN = column;  ///< The column kept.
};

/**
 * @brief Matches the rows of an `upsert_statement` on the column `column`, a `KeyRole::Unique` field, instead of on the primary
 * key (`ON CONFLICT ("column")`): a row whose value for it is already in the table updates that row. For a table whose primary
 * key the database numbers (an identity column) and whose rows are told apart by another column: an email, a natural key.
 *
 * The matched row keeps its primary key, whatever the row that would have gone in carried, and the matched column is never set
 * (as the primary key never is). The database must have the matching unique constraint or index on that column alone;
 * `create_table_statement` writes one for each `KeyRole::Unique` field.
 *
 * @code
 * using Id = sch::Field<std::int64_t, "id", sch::KeyRole::Primary, true>;  // numbered by the database
 * using Email = sch::Field<std::string, "email", sch::KeyRole::Unique>;
 * using Users = sch::Schema<"users", Id, Email, Name, Seen>;
 * db.upsert<Users, pg::ConflictOn<"email">, pg::Keep<"name">>({Users::make_insert_row("ann@example.com", "ann", today)});
 * @endcode
 */
template <util::CTString column>
struct ConflictOn {
  static constexpr util::CTString COLUMN = column;  ///< The column rows are matched on.
};

/**
 * @return The statement that inserts `rows` as `insert_statement` does, and, for a row that is already in the table, updates
 * that row instead (`ON CONFLICT (<matched columns>) DO UPDATE`). A row is matched on its primary key, or, if a `ConflictOn` in
 * `rule_ts` names a `KeyRole::Unique` column, on that column alone. Each column a `MergeWith` in `rule_ts` names is merged as
 * it says, each a `Keep` names is left as it is, and every other column that is neither matched on, nor part of the primary
 * key, nor generated takes the value that would have gone in. One statement, atomic; `execute` gives the count of rows
 * inserted or updated.
 *
 * Without a `ConflictOn`, the schema must have a primary key that the database does not number (a row is matched on it). With
 * one, the primary key may be generated, or absent, and is never set. Either way there must be a column left to update. Two
 * rows with the same matched value in one statement fail it (PostgreSQL updates a row once per statement): the error is the
 * database's, through the completion or future. So is a row that collides on anything but what it is matched on: with a
 * `ConflictOn`, a row whose primary key is already there under another matched value. A NULL in a matched column (a
 * `std::optional` field) matches nothing, so such a row is always inserted.
 */
template <TextSchema schema_t, UpsertRule... rule_ts, std::ranges::contiguous_range rows_t>
  requires schema::InsertRowOf<std::ranges::range_value_t<rows_t>, schema_t>
[[nodiscard]] SchemaStatement<schema_t, std::tuple<>> upsert_statement(const rows_t& rows) {
  using sql_t = detail::insert_detail::UpsertSql<schema_t, rule_ts...>;
  return detail::insert_detail::made<schema_t, std::tuple<>>(std::string(sql_t::VALUE), sql_t::TABLE_SLOT, rows);
}

/**
 * @return `schema_t`'s `CREATE TABLE`: each column's SQL type, `NOT NULL` unless its field is a `std::optional`, the primary key,
 * and a `UNIQUE` constraint per unique column. A generated key (the one primary key column, a signed integer) is an identity
 * column the database numbers. For tests, examples and a first start; a table with more to it (indexes, other constraints, any
 * other generated column) is created with SQL.
 */
template <schema::IsSchema schema_t>
[[nodiscard]] SchemaStatement<schema_t, std::tuple<>> create_table_statement() {
  return detail::SchemaStatementMaker::make<schema_t, std::tuple<>>(
      std::string(detail::CreateTableSql<schema_t>::VALUE), detail::CreateTableSql<schema_t>::TABLE_SLOT, std::vector<std::string>{}
  );
}

/** @return `schema_t`'s `DROP TABLE IF EXISTS`. */
template <schema::IsSchema schema_t>
[[nodiscard]] SchemaStatement<schema_t, std::tuple<>> drop_table_statement() {
  return detail::SchemaStatementMaker::make<schema_t, std::tuple<>>(
      std::string(detail::DropTableSql<schema_t>::VALUE), detail::DropTableSql<schema_t>::TABLE_SLOT, std::vector<std::string>{}
  );
}

/**
 * @brief A statement given as text while the program runs, with its arguments (see `unchecked_sql`). It returns no rows: `execute`
 * gives the count of rows it affected.
 *
 * One whose arguments could not be made (a value its codec refused) holds why, and fails with that when it runs, as a
 * `SchemaStatement` does.
 */
class UncheckedSql {
 public:
  using result_type = std::tuple<>;

  /**
   * @param sql    The statement, with `$n` placeholders for `params`.
   * @param params The text of each argument, in placeholder order.
   */
  UncheckedSql(std::string sql, std::vector<std::string> params) : _sql(std::move(sql)), _params(std::move(params)) {}

  /** @brief A statement whose arguments could not be made, for `error`: running it fails with that. */
  UncheckedSql(std::string sql, std::exception_ptr error) : _sql(std::move(sql)), _error(std::move(error)) {}

  [[nodiscard]] std::string_view sql() const noexcept { return _sql; }

  /** @return The text of each argument, in placeholder order: `params().front()` is `$1`. */
  [[nodiscard]] std::span<const std::string> params() const noexcept { return _params; }

  /** @return The arguments, taken out of this statement (which is then left without them): to send them without a copy. */
  [[nodiscard]] std::vector<std::string> take_params() && noexcept { return std::move(_params); }

  /** @return Why the arguments could not be made, or null. */
  [[nodiscard]] const std::exception_ptr& error() const noexcept { return _error; }

 private:
  std::string              _sql;
  std::vector<std::string> _params;
  std::exception_ptr       _error;
};

/**
 * @return `sql`, a statement written while the program runs, with `arguments` bound to `$1`, `$2`, ...: for what can't be written
 * when compiling (a `RawStatement`), such as a `CREATE INDEX` on a table named at run time. It returns no rows; `execute` gives
 * the count of rows it affected, on a `Database` or in a `Transaction`.
 *
 * Nothing about it is checked: not its SQL, nor that it uses each argument, nor that it returns no rows. A value goes as an
 * argument, never spliced into the text; a name spliced in is quoted first (`TableName::quoted()`). It is one statement: text
 * holding two (`...; ...`) fails, since it is sent with arguments (a function body's semicolons are inside one statement). An
 * argument is text (a `std::string`, `std::string_view` or literal, sent as it is) or any type with a text form, and is never
 * NULL. An argument its codec refuses fails the statement when it runs, through its completion or future.
 */
template <UncheckedSqlArgument... argument_ts>
[[nodiscard]] UncheckedSql unchecked_sql(std::string sql, const argument_ts&... arguments) {
  std::vector<std::string> params;
  try {
    params.reserve(sizeof...(argument_ts));
    const detail::TextBinder bind(&params);
    const auto               bind_one = [&bind]<typename argument_t>(const argument_t& argument) {
      if constexpr ( std::convertible_to<const argument_t&, std::string_view> ) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay) -- a string literal, read up to its terminating null
        bind(std::string(std::string_view(argument)));  // text, as it is
      } else {
        bind(argument);
      }
    };
    (bind_one(arguments), ...);
  } catch ( ... ) {
    return {std::move(sql), std::current_exception()};
  }

  return {std::move(sql), std::move(params)};
}

}  // namespace schemacht::postgres

namespace schemacht::postgres::detail {

/** @return The names of the columns a row (a tuple of `field_ts`) is read from: its fields', in order. They point at static storage. */
template <schema::IsField... field_ts>
[[nodiscard]] constexpr std::array<std::string_view, sizeof...(field_ts)> result_column_names(const std::tuple<field_ts...>* /*row*/) {
  return {field_ts::COLUMN_NAME.view()...};
}

/** @brief A statement as it goes to the database: its text, the text of each argument, and the columns it must return (by name, in order). */
struct WireStatement {
  std::string                   sql;
  std::vector<std::string>      params;
  std::vector<std::string_view> column_names;
};

/**
 * @return `statement` as it goes to the database: a `CompiledQuery` (with the names of the columns it returns, from its schema), a
 * `SchemaStatement`, an `UncheckedSql`, or a `BoundRawStatement` (which is about no table). The column names point at static storage.
 */
template <SendableStatement statement_t>
[[nodiscard]] WireStatement wire(const statement_t& statement) {
  WireStatement wired;
  wired.sql = statement.sql();

  if constexpr ( IsSchemaStatement<statement_t> || IsUncheckedSql<statement_t> ) {
    if ( statement.error() ) {
      std::rethrow_exception(statement.error());  // it could not be made
    }

    wired.params.assign(statement.params().begin(), statement.params().end());

    const auto names = result_column_names(static_cast<const statement_t::result_type*>(nullptr));  // none, if it returns no rows
    wired.column_names.assign(names.begin(), names.end());

  } else if constexpr ( query::IsBoundRawStatement<statement_t> ) {
    wired.params.reserve(std::tuple_size_v<typename statement_t::arguments_type>);
    std::apply([&wired](const auto&... arguments) { (TextBinder{&wired.params}(arguments), ...); }, statement.arguments());

    wired.column_names.assign(statement.column_names().begin(), statement.column_names().end());

  } else {
    using schema_t = statement_t::schema_type;
    wired.params = param_texts<schema_t>(statement);

    const auto names = schema_t::column_names();
    wired.column_names.reserve(statement.columns().size());
    for ( const std::size_t column : statement.columns() ) {
      wired.column_names.push_back(names.at(column));
    }
  }

  return wired;
}

/**
 * @brief As above, for a statement that holds its arguments as text (a `SchemaStatement`, an `UncheckedSql`) given as a temporary:
 * its arguments (an insert's rows, say) are moved, not copied.
 */
template <typename statement_t>
  requires(IsSchemaStatement<statement_t> || IsUncheckedSql<statement_t>)
[[nodiscard]] WireStatement wire(statement_t&& statement) {
  std::vector<std::string> params = std::forward<statement_t>(statement).take_params();  // an rvalue: `statement_t` is no reference
  WireStatement            wired = wire(std::as_const(statement));                       // its error, its text and columns; no arguments left to copy
  wired.params = std::move(params);

  return wired;
}

/**
 * @brief Checks that `result` has the columns `column_names`, in that order: a result whose columns are not the fields' is an error, not silently the wrong values.
 * @throws std::runtime_error if the result has rows and its number of columns is not the statement's, or a column is named otherwise (the message names both).
 */
template <NamedColumns result_t>
void check_result_columns(std::span<const std::string_view> column_names, const result_t& result) {
  const std::size_t column_count = result.column_count();
  if ( column_count != column_names.size() ) {
    if ( result.row_count() > 0 ) {  // a result without rows and without columns is the end of a stream, not a mistake
      throw std::runtime_error(
          "the result has " + std::to_string(column_count) + " columns, and the statement returns " + std::to_string(column_names.size())
      );
    }
    return;
  }

  for ( std::size_t column = 0; const std::string_view expected : column_names ) {
    if ( const std::string_view name = result.column_name(column++); name != expected ) {  // `column` is now its 1-based position
      throw std::runtime_error(
          "the statement returned the column '" + std::string(name) + "' at position " + std::to_string(column) + ", where its fields have '" +
          std::string(expected) + "' (name the select list's columns with AS)"
      );
    }
  }
}

}  // namespace schemacht::postgres::detail
