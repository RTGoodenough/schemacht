#pragma once

#include <concepts>
#include <cstdint>
#include <future>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "schemacht/postgres/concepts/row_reader.hpp"
#include "schemacht/postgres/concepts/table.hpp"  // IWYU pragma: export
#include "schemacht/postgres/key_lookup.hpp"
#include "schemacht/postgres/statement_runner.hpp"
#include "schemacht/postgres/statements.hpp"
#include "schemacht/schema/schema.hpp"
#include "schemacht/schema/table_name.hpp"
#include "schemacht/util/compile_time.hpp"
#include "schemacht/util/futures.hpp"

namespace schemacht::postgres {

/**
 * @brief A table of `schema_t`'s shape whose name is chosen while the program runs: the schema's statements, run on it
 * instead of on the schema's own table (`Schema`'s name).
 *
 * For tables a program can't name while compiling: one per tenant or per layer, a dated copy, a name from configuration or
 * from another language's bindings. Each call is the `StatementRunner` call it names, on this table; a query, update or
 * delete written against `schema_t` runs here as it is, its columns checked while compiling as ever. As there, `insert`,
 * `execute` and `stream` each have a form that takes a completion instead (`insert_into`, `execute_into`, `stream_into`), for
 * chaining one step onto another without blocking a worker; and no error escapes a call.
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
  [[nodiscard]] std::future<void> create() { return _runner->run_without_rows<detail::CreateTableSql<schema_t>>(&_name); }

  /** @return As `StatementRunner::drop_table`, for this table. */
  [[nodiscard]] std::future<void> drop() { return _runner->run_without_rows<detail::DropTableSql<schema_t>>(&_name); }

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

  /** @brief As `StatementRunner::insert_into`, into this table. */
  template <detail::StreamCallback done_t>
  void insert_into(const std::vector<row_type>& rows, done_t done) noexcept {
    _runner->insert_rows_into<schema_t, row_type>(rows, &_name, std::move(done));
  }

  /** @brief As above, for a span. */
  template <detail::StreamCallback done_t>
  void insert_into(std::span<const row_type> rows, done_t done) noexcept {
    _runner->insert_rows_into<schema_t, row_type>(rows, &_name, std::move(done));
  }

  /** @brief As above, for rows without the generated columns (`Schema::make_insert_row`). */
  template <detail::StreamCallback done_t>
    requires schema::HasGeneratedColumn<schema_t>
  void insert_into(const std::vector<typename schema_t::insert_row_type>& rows, done_t done) noexcept {
    _runner->insert_rows_into<schema_t, typename schema_t::insert_row_type>(rows, &_name, std::move(done));
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
    return _runner->select_by_key_values_on<schema_t, key_col>(keys, &_name);
  }

  /**
   * @return As `StatementRunner::select_by_key`, from this table. A template only so that its key type is formed when it is
   * called: a schema whose key is not one column (or that has none) has no `primary_key_type`, and a `Table` of it must
   * still compile.
   */
  template <schema::IsSchema key_schema_t = schema_t>
    requires std::same_as<key_schema_t, schema_t> && schema::HasSingleColumnKey<key_schema_t>
  [[nodiscard]] std::future<std::vector<row_type>> select_by_key(std::span<const primary_key_type<key_schema_t>> keys) {
    return select_by_key_values<schema::primary_key_field<schema_t>::COLUMN_NAME>(keys);
  }

  /** @brief As `StatementRunner::execute_into`, for a query (or an update or delete with `returning`) on this table. */
  template <RunnableStatement statement_t, detail::ExecuteCallback<typename statement_t::result_type> done_t>
  void execute_into(const statement_t& statement, done_t done, const std::optional<ReadOptions>& read = std::nullopt) noexcept {
    if constexpr ( check_schema<statement_t>() ) {
      _runner->execute_on(statement, &_name, std::move(done), read);
    }
  }

  /** @return As `StatementRunner::execute`, for a query (or an update or delete with `returning`) on this table. */
  template <RunnableStatement statement_t>
  [[nodiscard]] std::future<std::vector<typename statement_t::result_type>> execute(
      const statement_t& statement, const std::optional<ReadOptions>& read = std::nullopt
  ) {
    return util::future_from<std::vector<typename statement_t::result_type>>([&](auto done) { execute_into(statement, std::move(done), read); });
  }

  /** @brief As `StatementRunner::execute_into`, for an update or delete on this table: `done` gets the count of rows it changed. */
  template <CommandStatement statement_t, detail::CommandCallback done_t>
  void execute_into(const statement_t& command, done_t done) noexcept {
    if constexpr ( check_schema<statement_t>() ) {
      _runner->command_on(command, &_name, std::move(done));
    }
  }

  /** @return As `StatementRunner::execute`, for an update or delete on this table: the count of rows it changed. */
  template <CommandStatement statement_t>
  [[nodiscard]] std::future<std::uint64_t> execute(const statement_t& command) {
    return util::future_from<std::uint64_t>([&](auto done) { execute_into(command, std::move(done)); });
  }

  /** @brief As `StatementRunner::stream_into`, for a query on this table. */
  template <RunnableStatement statement_t, RowCallback<typename statement_t::result_type> handler_t, detail::StreamCallback done_t>
  void stream_into(const statement_t& statement, handler_t on_row, done_t done, const std::optional<ReadOptions>& read = std::nullopt) noexcept {
    if constexpr ( check_schema<statement_t>() ) {
      _runner->stream_on(statement, &_name, std::move(on_row), std::move(done), read);
    }
  }

  /** @return As `StatementRunner::stream`, for a query on this table. */
  template <RunnableStatement statement_t, RowCallback<typename statement_t::result_type> handler_t>
  [[nodiscard]] std::future<void> stream(const statement_t& statement, handler_t on_row, const std::optional<ReadOptions>& read = std::nullopt) {
    return util::future_from<void>([&](auto done) {
      if constexpr ( check_schema<statement_t>() ) {
        _runner->stream_on(statement, &_name, std::move(on_row), std::move(done), read);  // the handler is moved only inside the guard
      }
    });
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
