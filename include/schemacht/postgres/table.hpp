#pragma once

#include <cstdint>
#include <future>
#include <optional>
#include <span>
#include <vector>

#include "schemacht/postgres/concepts/table.hpp"  // IWYU pragma: export
#include "schemacht/postgres/key_lookup.hpp"
#include "schemacht/postgres/statement_runner.hpp"
#include "schemacht/postgres/statements.hpp"
#include "schemacht/schema/schema.hpp"
#include "schemacht/schema/table_name.hpp"
#include "schemacht/util/compile_time.hpp"

namespace schemacht::postgres {

/**
 * @brief A table of `schema_t`'s shape whose name is chosen while the program runs: the schema's statements, run on it
 * instead of on the schema's own table (`Schema`'s name).
 *
 * For tables a program can't name while compiling: one per tenant or per layer, a dated copy, a name from configuration or
 * from another language's bindings. Each call is the `StatementRunner` call it names, on this table; a query, update or
 * delete written against `schema_t` runs here as it is, its columns checked while compiling as ever.
 *
 * It runs on a `Database` (the pool) or a `Transaction` (its connection), and must not outlive it, nor be used once a
 * transaction it runs on has been moved from.
 */
template <TextSchema schema_t>
class Table {
 public:
  using schema_type = schema_t;
  using row_type = schema_t::row_type;

  Table(StatementRunner& runner, schema::TableName name) : _runner(&runner), _name(std::move(name)) {}

  [[nodiscard]] const schema::TableName& name() const noexcept { return _name; }

  /** @return As `StatementRunner::create_table`, for this table. */
  [[nodiscard]] std::future<void> create() { return _runner->run_without_rows(detail::sql_on<detail::CreateTableSql<schema_t>>(&_name)); }

  /** @return As `StatementRunner::drop_table`, for this table. */
  [[nodiscard]] std::future<void> drop() { return _runner->run_without_rows(detail::sql_on<detail::DropTableSql<schema_t>>(&_name)); }

  /** @return As `StatementRunner::insert`, into this table. */
  [[nodiscard]] std::future<void> insert(const std::vector<row_type>& rows) { return _runner->insert_rows<schema_t, row_type>(rows, &_name); }

  /** @brief As above, for a span. */
  [[nodiscard]] std::future<void> insert(std::span<const row_type> rows) { return _runner->insert_rows<schema_t, row_type>(rows, &_name); }

  /** @brief As above, for rows without the generated columns (`Schema::make_insert_row`). */
  [[nodiscard]] std::future<void> insert(const std::vector<typename schema_t::insert_row_type>& rows)
    requires schema::HasGeneratedColumn<schema_t>
  {
    return _runner->insert_rows<schema_t, typename schema_t::insert_row_type>(rows, &_name);
  }

  /** @return As `StatementRunner::insert_returning`, into this table. */
  [[nodiscard]] std::future<std::vector<row_type>> insert_returning(
      const std::vector<row_type>& rows, const std::optional<ReadOptions>& read = std::nullopt
  ) {
    return _runner->insert_rows_returning<schema_t, row_type>(rows, read, &_name);
  }

  /** @brief As above, for rows without the generated columns. */
  [[nodiscard]] std::future<std::vector<row_type>> insert_returning(
      const std::vector<typename schema_t::insert_row_type>& rows, const std::optional<ReadOptions>& read = std::nullopt
  )
    requires schema::HasGeneratedColumn<schema_t>
  {
    return _runner->insert_rows_returning<schema_t, typename schema_t::insert_row_type>(rows, read, &_name);
  }

  /** @return As `StatementRunner::select_by_key_values`, from this table. */
  template <util::CTString key_col>
  [[nodiscard]] std::future<std::vector<row_type>> select_by_key_values(std::span<const key_type<schema_t, key_col>> keys) {
    if ( keys.empty() ) {
      return detail::ready_future(std::vector<row_type>{});
    }

    return _runner->execute(by_key_values<schema_t, key_col>(keys).on(_name));  // moved: the keys are copied once
  }

  /** @return As `StatementRunner::select_by_key`, from this table. */
  [[nodiscard]] std::future<std::vector<row_type>> select_by_key(std::span<const primary_key_type<schema_t>> keys)
    requires schema::HasSingleColumnKey<schema_t>
  {
    return select_by_key_values<schema::primary_key_field<schema_t>::COLUMN_NAME>(keys);
  }

  /** @return As `StatementRunner::execute`, for a query (or an update or delete with `returning`) on this table. */
  template <RunnableStatement statement_t>
  [[nodiscard]] std::future<std::vector<typename statement_t::result_type>> execute(
      const statement_t& statement, const std::optional<ReadOptions>& read = std::nullopt
  ) {
    if constexpr ( check_schema<statement_t>() ) {
      return _runner->execute(statement.on(_name), read);
    } else {
      return {};  // after the assertion fails, so no second error follows
    }
  }

  /** @return As `StatementRunner::execute`, for an update or delete on this table: the count of rows it changed. */
  template <CommandStatement statement_t>
  [[nodiscard]] std::future<std::uint64_t> execute(const statement_t& command) {
    if constexpr ( check_schema<statement_t>() ) {
      return _runner->execute(command.on(_name));
    } else {
      return {};
    }
  }

  /** @return As `StatementRunner::stream`, for a query on this table. */
  template <RunnableStatement statement_t, RowCallback<typename statement_t::result_type> handler_t>
  [[nodiscard]] std::future<void> stream(const statement_t& statement, handler_t on_row, const std::optional<ReadOptions>& read = std::nullopt) {
    if constexpr ( check_schema<statement_t>() ) {
      return _runner->stream(statement.on(_name), std::move(on_row), read);
    } else {
      return {};
    }
  }

 private:
  StatementRunner*  _runner;
  schema::TableName _name;

  /** @return Whether `statement_t` can run on this table; a compile error that says why if not. */
  template <typename statement_t>
  static consteval bool check_schema() {
    static_assert(
        StatementOn<statement_t, schema_t>,
        "a Table runs only queries, updates and deletes written against its own schema (a raw statement names its own tables)"
    );

    return StatementOn<statement_t, schema_t>;
  }
};

}  // namespace schemacht::postgres
