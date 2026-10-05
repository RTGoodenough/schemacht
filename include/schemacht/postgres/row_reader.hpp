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
#include <vector>

#include "schemacht/postgres/async_client.hpp"
#include "schemacht/postgres/concepts/row_reader.hpp"  // IWYU pragma: export
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
  requires((format == ResultFormat::Text && TextRow<result_t>) || (format == ResultFormat::Binary && BinaryReadableRow<result_t>))
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
        throw std::runtime_error(
            "column '" + std::string(field_t::COLUMN_NAME.view()) + "': the database has the type with OID " + std::to_string(type_oid) +
            ", which the schema's field cannot read (the field and the table disagree)"
        );
      }
    };
    (check.template operator()<pos>(), ...);
  }(std::make_index_sequence<std::tuple_size_v<result_t>>{});
}

/**
 * @brief Reads every row of `chunk` as a `result_t` and hands each to `on_row`, in order, until it wants no more.
 * @pre The chunk has `result_t`'s columns (see `check_result_columns`).
 * @return Whether `on_row` wants more rows (false: it returned `false`, and the rows after that were not read).
 * @throws as `read_row`, `check_column_types` and `on_row`; std::logic_error for a binary chunk of a row that is not `BinaryReadableRow`.
 */
template <TextRow result_t, ResultChunk chunk_t, RowCallback<result_t> handler_t>
bool read_rows(const chunk_t& chunk, handler_t& on_row) {
  const auto each_row = [&]<ResultFormat format> {
    for ( std::size_t row = 0; row < chunk.row_count(); ++row ) {
      if constexpr ( std::same_as<std::invoke_result_t<handler_t&, result_t&&>, bool> ) {
        if ( ! std::invoke(on_row, read_row<result_t, format>(chunk, row)) ) {
          return false;
        }

      } else {
        std::invoke(on_row, read_row<result_t, format>(chunk, row));
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

/**
 * @return Every row of `chunk`, read as a `result_t`, in order: what `Database::stream_chunks` hands its handler.
 * @pre The chunk has `result_t`'s columns (see `check_result_columns`).
 * @throws as `read_rows`.
 */
template <TextRow result_t, ResultChunk chunk_t>
[[nodiscard]] std::vector<result_t> read_chunk(const chunk_t& chunk) {
  std::vector<result_t> rows;
  rows.reserve(chunk.row_count());

  const auto keep = [&rows](result_t&& row) { rows.push_back(std::move(row)); };
  read_rows<result_t>(chunk, keep);

  return rows;
}

}  // namespace schemacht::postgres
