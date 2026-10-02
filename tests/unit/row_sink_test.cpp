#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "schemacht/query/row_sink.hpp"
#include "support/schemas.hpp"

namespace q = schemacht::query;
namespace t = schemacht::test;

namespace {

using Row = std::tuple<t::Id, t::Nick>;

void push_row(q::RowSink& sink, const char* id, std::optional<std::string> nick) {
  sink.begin_row();
  sink.put_text(0, id);
  if ( nick ) {
    sink.put_text(1, *nick);
  } else {
    sink.put_null(1);
  }
  sink.end_row();
}

}  // namespace

TEST_CASE("a collector keeps every row", "[sink]") {
  q::RowCollector<Row> collector;
  push_row(collector, "1", "ann");
  push_row(collector, "2", std::nullopt);
  const std::vector<Row> rows = collector.take();
  REQUIRE(rows.size() == 2);
  CHECK(std::get<0>(rows[0]).value == 1);
  CHECK(std::get<1>(rows[0]).value == "ann");
  CHECK_FALSE(std::get<1>(rows[1]).value.has_value());
}

TEST_CASE("a NULL in a column that cannot be NULL is an error", "[sink]") {
  q::RowCollector<Row> collector;
  collector.begin_row();
  CHECK_THROWS_AS(collector.put_null(0), std::runtime_error);
}

TEST_CASE("a value before its row began, or past the row's end, is an error", "[sink]") {
  q::RowCollector<Row> collector;
  CHECK_THROWS_AS(collector.put_text(0, "1"), std::logic_error);
  collector.begin_row();
  CHECK_THROWS_AS(collector.put_text(2, "1"), std::out_of_range);
  CHECK_THROWS_WITH(collector.put_text(0, "x"), "column 'id': parse_field: could not parse 'x' as a number of the column's type");
}

TEST_CASE("a handler that returns false stops the rows", "[sink]") {
  std::vector<std::int64_t> seen;
  const auto                handler = [&seen](const Row& row) {
    seen.push_back(std::get<0>(row).value);
    return seen.size() < 2;
  };
  q::RowHandler<Row, decltype(handler)> sink(handler);
  push_row(sink, "1", "a");
  CHECK(sink.wants_more());
  push_row(sink, "2", "b");
  CHECK_FALSE(sink.wants_more());
  push_row(sink, "3", "c");  // pushed anyway: ignored
  CHECK(seen == std::vector<std::int64_t>{1, 2});
}
