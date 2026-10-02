#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "schemacht/postgres/field_codec.hpp"
#include "schemacht/schema/field.hpp"
#include "schemacht/util/visit_index.hpp"

/**
 * How a backend hands query results to its caller without knowing their type.
 *
 * A query's result rows are a type that depends on the query (a projection returns fewer columns), and a virtual function can't
 * return a type that varies per call. So the result flows the other way: the caller passes a sink, and the backend pushes each
 * value into it as the text the server printed (`put_text`) or its bytes in PostgreSQL's binary form (`put_binary`), a column
 * at a time. The sink is built for the query's result row, so it knows the type of the `position`-th column and parses the
 * value itself: the backend needs no type at all.
 *
 * A sink sees rows one at a time, so it decides what happens to each: `RowCollector` keeps them all, and `RowHandler`
 * hands each to a function and keeps none. A sink can also say it wants no more rows (`wants_more`), which is how a
 * consumer stops a query early.
 */
namespace schemacht::query {

/** @brief What a backend writes query results into. */
class RowSink {
 public:
  RowSink() = default;
  RowSink(const RowSink&) = delete;
  RowSink(RowSink&&) = delete;
  RowSink& operator=(const RowSink&) = delete;
  RowSink& operator=(RowSink&&) = delete;
  virtual ~RowSink() = default;

  /** @brief A hint of how many rows are coming. */
  virtual void reserve(std::size_t rows) = 0;

  /** @brief Starts a row; its values follow as `put_text`s, `put_binary`s or `put_null`s, one per returned column, in the query's column order. */
  virtual void begin_row() = 0;

  /** @brief Receives a NULL for the `position`-th column of the row being built. */
  virtual void put_null(std::size_t position) = 0;

  /**
   * @brief Receives the text of the `position`-th column of the row being built, for the sink to parse (`postgres::FieldCodec::parse`).
   * @throws std::invalid_argument naming the column, if the text is not a value of its type.
   */
  virtual void put_text(std::size_t position, std::string_view text) = 0;

  /**
   * @brief Receives the PostgreSQL binary form of the `position`-th column of the row being built (`postgres::FieldCodec::parse_binary`).
   * @throws std::invalid_argument naming the column, if the bytes are not a value of its type.
   * @throws std::logic_error if the sink builds a row with a column that has no binary form (`BinaryReadableRow`): a caller that has
   * checked that never reaches this. `Database::execute` and `stream` check it at compile time.
   */
  virtual void put_binary(std::size_t position, std::span<const std::byte> bytes) = 0;

  /**
   * @brief Says which PostgreSQL type (`pg_type.oid`) the backend's `position`-th result column has, before any `put_binary`: a column the schema's field cannot read is refused once
   * for the whole result, not on the first value.
   * @throws std::runtime_error naming the column, if the field cannot read that type (the schema and the table disagree).
   * @throws std::logic_error as for `put_binary`.
   */
  virtual void check_binary_type(std::size_t position, std::uint32_t type_oid) = 0;

  virtual void end_row() = 0;

  /**
   * @brief Whether the sink wants more rows: false once its consumer has stopped. A producer checks it after each `end_row`
   * and, when it is false, stops producing (rows pushed after that are ignored).
   */
  [[nodiscard]] virtual bool wants_more() const { return true; }
};

namespace detail {

template <typename result_t, std::size_t... pos>
constexpr bool every_field_has_binary_form(std::index_sequence<pos...> /*positions*/) {
  return (postgres::HasBinaryForm<typename std::tuple_element_t<pos, result_t>::value_type> && ...);
}

}  // namespace detail

/**
 * @brief A row (`std::tuple` of `Field`s, as a query returns them) whose every column's type has a binary form (`postgres::HasBinaryForm`): what a result read in
 * PostgreSQL's binary form needs. A query's `result_type` is checked against it where the query is run (`Database::execute`), so a column
 * that cannot be read that way is a compile error at the call, and only for the columns the query returns.
 */
template <typename result_t>
concept BinaryReadableRow = detail::every_field_has_binary_form<result_t>(std::make_index_sequence<std::tuple_size_v<result_t>>{});

namespace detail {

/**
 * @brief The part of a sink that builds a row of `result_t` from the values pushed into it: `put_text`, `put_binary`, `put_null`, and the
 * checks that a value fits the row.
 *
 * The derived sink says which row is being built (`current_row()`, which throws `std::logic_error` if there is none) and
 * what happens when it ends.
 *
 * @tparam derived_t The sink built on this, which has `current_row()`.
 */
template <typename result_t, typename derived_t>
// NOLINTNEXTLINE(bugprone-crtp-constructor-accessibility) -- built by its derived sinks, which are the only users
class RowFiller : public RowSink {
 public:
  /**
   * @brief Makes the `position`-th column of the current row NULL.
   * @throws std::runtime_error if that column's field is not a `std::optional`: the database holds a NULL the schema says it cannot.
   */
  void put_null(std::size_t position) override {
    result_t&  row = current();
    const bool stored = [&]<std::size_t... pos>(std::index_sequence<pos...> /*positions*/) {
      return ((position == pos && clear<pos>(row)) || ...);
    }(std::make_index_sequence<std::tuple_size_v<result_t>>{});
    if ( ! stored ) {
      throw std::logic_error("row sink: a value arrived at a position that is not in the row");
    }
  }

  /** @brief Parses `text` as the `position`-th column of the current row, which is where its type is known: `result_t`'s `position`-th field. */
  void put_text(std::size_t position, std::string_view text) override {
    result_t& row = current();
    util::visit_index<std::tuple_size_v<result_t>>(position, [&]<std::size_t pos> {
      using field_t = std::tuple_element_t<pos, result_t>;
      std::get<pos>(row).value = postgres::parse_column<field_t>(text);
    });
  }

  void put_binary(std::size_t position, std::span<const std::byte> bytes) override {
    if constexpr ( BinaryReadableRow<result_t> ) {
      result_t& row = current();
      util::visit_index<std::tuple_size_v<result_t>>(position, [&]<std::size_t pos> {
        using field_t = std::tuple_element_t<pos, result_t>;
        std::get<pos>(row).value = postgres::parse_binary_column<field_t>(bytes);
      });
    } else {
      throw_no_binary_form(position, bytes.size());
    }
  }

  void check_binary_type(std::size_t position, std::uint32_t type_oid) override {
    if constexpr ( BinaryReadableRow<result_t> ) {
      util::visit_index<std::tuple_size_v<result_t>>(position, [&]<std::size_t pos> {
        using field_t = std::tuple_element_t<pos, result_t>;
        if ( ! postgres::binary_column_accepted<field_t>(type_oid) ) {
          throw std::runtime_error("column '" + std::string(field_t::COLUMN_NAME.view()) + "': the database has the type with OID " +
                                   std::to_string(type_oid) + ", which the schema's field cannot read (the field and the table disagree)");
        }
      });
    } else {
      throw_no_binary_form(position, type_oid);
    }
  }

 private:
  result_t& current() { return static_cast<derived_t*>(this)->current_row(); }

  /** @brief What `put_binary` and `check_binary_type` do for a row with a column that has no binary form: a caller that checked `BinaryReadableRow` never gets here. */
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- both only go into the message
  [[noreturn]] static void throw_no_binary_form(std::size_t position, std::size_t value) {
    throw std::logic_error("row sink: the row has a column with no binary form (position " + std::to_string(position) + ", " + std::to_string(value) +
                           "), so it cannot be read in binary");
  }

  /** @return True once the `pos`-th field of the row is NULL. */
  template <std::size_t pos>
  static bool clear(result_t& row) {
    using field_t = std::tuple_element_t<pos, result_t>;
    if constexpr ( field_t::NULLABLE ) {
      std::get<pos>(row).value = std::nullopt;
      return true;
    } else {
      throw schema::null_in_required_field<field_t>();
    }
  }
};

}  // namespace detail

/**
 * @brief A `RowSink` that collects every row, of a caller-chosen type.
 *
 * @tparam result_t A `std::tuple` of `Field`s, one per returned column and in the query's column order -- the full
 *                  `row_type` for an unprojected query.
 */
template <typename result_t>
class RowCollector : public detail::RowFiller<result_t, RowCollector<result_t>> {
 public:
  RowCollector() = default;
  RowCollector(const RowCollector&) = delete;  // a sink is filled where it was made: the producer holds a pointer to it
  RowCollector(RowCollector&&) = delete;
  RowCollector& operator=(const RowCollector&) = delete;
  RowCollector& operator=(RowCollector&&) = delete;
  ~RowCollector() override = default;

  void reserve(std::size_t rows) override { _rows.reserve(rows); }
  void begin_row() override { _rows.emplace_back(); }
  void end_row() override {}

  /** @brief The row being built (the hook `RowFiller` needs). @throws std::logic_error if none was begun. */
  result_t& current_row() {
    if ( _rows.empty() ) {
      throw std::logic_error("row sink: a value arrived before its row began");
    }
    return _rows.back();
  }

  /** @return The collected rows. */
  [[nodiscard]] std::vector<result_t> take() { return std::move(_rows); }

 private:
  std::vector<result_t> _rows;
};

/**
 * @brief Whether `handler_t` can receive the rows of a `RowHandler<result_t, handler_t>` (or a `Database::stream`/`stream_into`
 * of the same result): invocable with a `result_t&&`, returning `void`, or a `bool` where `false` stops the stream.
 */
template <typename handler_t, typename result_t>
concept RowCallback = std::invocable<handler_t&, result_t&&> && (std::same_as<std::invoke_result_t<handler_t&, result_t&&>, void> ||
                                                                std::same_as<std::invoke_result_t<handler_t&, result_t&&>, bool>);

/**
 * @brief A `RowSink` that hands each row, when it is complete, to a function, and keeps none: the sink of a stream.
 *
 * `handler_t` is called with the row (a `result_t` rvalue: take it by value or `result_t&&` to keep it, or `const result_t&`
 * to look) and returns `void`, or a `bool` where `false` means "stop": the
 * sink then wants no more rows (`wants_more`) and never calls the function again, even if rows are still pushed. If the
 * function throws, the exception goes to whoever pushed the row.
 *
 * @tparam result_t  A `std::tuple` of `Field`s, as for `RowCollector`.
 * @tparam handler_t Invocable with a `result_t&&`, returning `void` or `bool` (see `RowCallback`).
 */
template <typename result_t, RowCallback<result_t> handler_t>
class RowHandler : public detail::RowFiller<result_t, RowHandler<result_t, handler_t>> {
 public:
  explicit RowHandler(std::remove_cv_t<handler_t> handler) : _handler(std::move(handler)) {}

  RowHandler(const RowHandler&) = delete;  // a sink is filled where it was made: the producer holds a pointer to it
  RowHandler(RowHandler&&) = delete;
  RowHandler& operator=(const RowHandler&) = delete;
  RowHandler& operator=(RowHandler&&) = delete;
  ~RowHandler() override = default;

  void reserve(std::size_t /*rows*/) override {}

  void begin_row() override {
    _row = result_t{};
    _building = true;
  }

  void end_row() override {
    _building = false;
    if ( _stopped ) {
      return;
    }
    if constexpr ( std::same_as<std::invoke_result_t<handler_t&, result_t&&>, bool> ) {
      _stopped = ! _handler(std::move(_row));
    } else {
      _handler(std::move(_row));
    }
  }

  [[nodiscard]] bool wants_more() const override { return ! _stopped; }

  /** @brief The row being built (the hook `RowFiller` needs). @throws std::logic_error if none was begun. */
  result_t& current_row() {
    if ( ! _building ) {
      throw std::logic_error("row sink: a value arrived before its row began");
    }
    return _row;
  }

 private:
  std::remove_cv_t<handler_t> _handler;  // the type may be const-qualified (a `const auto` lambda); the member is not
  result_t                    _row;
  bool                        _building = false;
  bool                        _stopped = false;
};

}  // namespace schemacht::query
