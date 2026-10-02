#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "schemacht/postgres/field_codec.hpp"
#include "schemacht/schema/field.hpp"

/**
 * Turning a chunk of a query's result into typed rows.
 *
 * The row type (`result_t`, a `schema::FieldTuple`) is known where a query is run, so each row is built directly: field
 * `i` of the tuple is parsed from column `i` of the chunk, with `i` a compile-time constant. Projection needs nothing
 * here: a projected query's `result_type` simply has fewer fields, in the order the statement returns them.
 */
namespace schemacht::postgres {

/** @brief The form a query's result arrives in: PostgreSQL's binary form (less to send and parse), or text. */
enum class ResultFormat : std::uint8_t {
  Binary,
  Text,
};

/**
 * @brief A chunk of a query's result, as `read_rows` reads it (`QueryResult` is one).
 *
 * `field` gives a value as its text, or as its bytes in binary form when `is_binary()`, and `std::nullopt` for NULL.
 */
template <typename chunk_t>
concept ResultChunk = requires(const chunk_t& chunk, std::size_t index) {
  { chunk.row_count() } -> std::convertible_to<std::size_t>;
  { chunk.column_count() } -> std::convertible_to<std::size_t>;
  { chunk.is_binary() } -> std::convertible_to<bool>;
  { chunk.column_type(index) } -> std::convertible_to<std::uint32_t>;
  { chunk.field(index, index) } -> std::same_as<std::optional<std::string_view>>;
};

namespace detail {

template <schema::FieldTuple result_t, std::size_t... pos>
consteval bool every_field_has_binary_form(std::index_sequence<pos...> /*positions*/) {
  return (HasBinaryForm<typename std::tuple_element_t<pos, result_t>::value_type> && ...);
}

}  // namespace detail

/**
 * @brief A row whose every column's type can be read from PostgreSQL's binary form (`HasBinaryForm`). A row that is not is
 * always read as text.
 */
template <typename result_t>
concept BinaryReadableRow =
    schema::FieldTuple<result_t> && detail::every_field_has_binary_form<result_t>(std::make_index_sequence<std::tuple_size_v<result_t>>{});

/** @brief What `read_rows` hands rows to: `take(row)` returns whether it wants more. */
template <typename consumer_t, typename result_t>
concept RowConsumer = schema::FieldTuple<result_t> && requires(consumer_t& consumer, result_t&& row) {
  { consumer.take(std::move(row)) } -> std::same_as<bool>;
};

/**
 * @brief A function a stream hands each row to (`Database::stream`): invocable with a `result_t&&`, returning `void`, or a
 * `bool` where `false` stops the stream.
 */
template <typename handler_t, typename result_t>
concept RowCallback =
    schema::FieldTuple<result_t> && std::invocable<handler_t&, result_t&&> &&
    (std::same_as<std::invoke_result_t<handler_t&, result_t&&>, void> || std::same_as<std::invoke_result_t<handler_t&, result_t&&>, bool>);

namespace detail {

/** @return The value of `field_t` from one field's text or bytes, or its NULL. @throws as `parse_column`, or if a NULL does not fit `field_t`. */
template <schema::IsField field_t, ResultFormat format>
[[nodiscard]] field_t::data_type read_field(const std::optional<std::string_view>& value) {
  if ( ! value.has_value() ) {
    if constexpr ( field_t::NULLABLE ) {
      return std::nullopt;
    } else {
      throw schema::null_in_required_field<field_t>();
    }
  } else if constexpr ( format == ResultFormat::Binary ) {
    return parse_binary_column<field_t>(std::as_bytes(std::span<const char>(value->data(), value->size())));
  } else {
    return parse_column<field_t>(*value);
  }
}

}  // namespace detail

/**
 * @return Row `row` of `chunk` as a `result_t`: field `i` read from column `i`, in `format`.
 * @pre `row < chunk.row_count()`, and the chunk has `result_t`'s columns, in `format`.
 * @throws std::invalid_argument naming the column, if a value is not one of its type.
 * @throws std::runtime_error naming the column, for a NULL in a field that is not a `std::optional`.
 */
template <schema::FieldTuple result_t, ResultFormat format, ResultChunk chunk_t>
  requires(format == ResultFormat::Text || BinaryReadableRow<result_t>)
[[nodiscard]] result_t read_row(const chunk_t& chunk, std::size_t row) {
  return [&]<std::size_t... pos>(std::index_sequence<pos...> /*positions*/) {
    return result_t{std::tuple_element_t<pos, result_t>{detail::read_field<std::tuple_element_t<pos, result_t>, format>(chunk.field(row, pos))}...};
  }(std::make_index_sequence<std::tuple_size_v<result_t>>{});
}

/**
 * @brief Checks that each column of a binary chunk has a PostgreSQL type its field can read, before any row is read.
 * @throws std::runtime_error naming the first column whose field cannot read its type (the schema and the table disagree).
 */
template <BinaryReadableRow result_t, ResultChunk chunk_t>
void check_column_types(const chunk_t& chunk) {
  [&]<std::size_t... pos>(std::index_sequence<pos...> /*positions*/) {
    const auto check = [&]<std::size_t position> {
      using field_t = std::tuple_element_t<position, result_t>;
      if ( const std::uint32_t type_oid = chunk.column_type(position); ! binary_column_accepted<field_t>(type_oid) ) {
        throw std::runtime_error("column '" + std::string(field_t::COLUMN_NAME.view()) + "': the database has the type with OID " +
                                 std::to_string(type_oid) + ", which the schema's field cannot read (the field and the table disagree)");
      }
    };
    (check.template operator()<pos>(), ...);
  }(std::make_index_sequence<std::tuple_size_v<result_t>>{});
}

/**
 * @brief Reads every row of `chunk` as a `result_t` and hands each to `consumer`, in order, until it wants no more.
 * @pre The chunk has `result_t`'s columns (see `check_result_columns`).
 * @return Whether `consumer` wants more rows (false: it stopped, and the rows after that were not read).
 * @throws as `read_row` and `check_column_types`; std::logic_error for a binary chunk of a row that is not `BinaryReadableRow`.
 */
template <schema::FieldTuple result_t, ResultChunk chunk_t, RowConsumer<result_t> consumer_t>
bool read_rows(const chunk_t& chunk, consumer_t& consumer) {
  const auto each_row = [&]<ResultFormat format> {
    for ( std::size_t row = 0; row < chunk.row_count(); ++row ) {
      if ( ! consumer.take(read_row<result_t, format>(chunk, row)) ) {
        return false;
      }
    }
    return true;
  };
  if ( ! chunk.is_binary() ) {
    return each_row.template operator()<ResultFormat::Text>();
  }
  if constexpr ( BinaryReadableRow<result_t> ) {
    check_column_types<result_t>(chunk);
    return each_row.template operator()<ResultFormat::Binary>();
  } else {
    throw std::logic_error("read_rows: a binary result for a row with a column that has no binary form (it should have been asked for as text)");
  }
}

}  // namespace schemacht::postgres
