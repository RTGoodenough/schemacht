#include "schemacht/postgres/chrono_codec.hpp"

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include "schemacht/postgres/field_codec.hpp"

namespace schemacht::postgres::detail {

namespace {

constexpr std::int64_t MICROS_PER_SECOND = 1'000'000;
constexpr std::int64_t SECONDS_PER_DAY = 86'400;
constexpr std::int64_t MICROS_PER_DAY = MICROS_PER_SECOND * SECONDS_PER_DAY;
constexpr std::int64_t SECONDS_PER_HOUR = 3'600;
constexpr std::int64_t SECONDS_PER_MINUTE = 60;
constexpr std::int64_t DAYS_TO_POSTGRES_EPOCH = 10'957;  // 1970-01-01 to 2000-01-01, PostgreSQL's epoch
constexpr std::int64_t MICROS_TO_POSTGRES_EPOCH = DAYS_TO_POSTGRES_EPOCH * MICROS_PER_DAY;
constexpr std::size_t  MAX_FRACTION_DIGITS = 6;  // PostgreSQL keeps microseconds
constexpr std::int64_t DECIMAL_BASE = 10;
constexpr std::int64_t LAST_HOUR = 24;  // 24:00:00, which PostgreSQL accepts for the end of a day
constexpr std::int64_t LAST_MINUTE = 59;
constexpr std::int64_t LAST_SECOND = 60;  // a leap second, which PostgreSQL accepts
constexpr unsigned     MONTHS_PER_YEAR = 12;

[[noreturn]] void refuse(std::string_view what, std::string_view text) {
  throw std::invalid_argument("parse_field: '" + std::string(text) + "' is not " + std::string(what));
}

/** @return `left + right`. @throws std::invalid_argument if it overflows. */
std::int64_t checked_add(std::int64_t left, std::int64_t right) {
  if ( (right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
       (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right) ) {
    throw std::invalid_argument("the time is outside what can be held in microseconds");
  }
  return left + right;
}

/** @return `left * right`, for a positive `right`. @throws std::invalid_argument if it overflows. */
std::int64_t checked_multiply(std::int64_t left, std::int64_t right) {
  if ( left > std::numeric_limits<std::int64_t>::max() / right || left < std::numeric_limits<std::int64_t>::min() / right ) {
    throw std::invalid_argument("the time is outside what can be held in microseconds");
  }
  return left * right;
}

/** @return `numerator / denominator`, rounded toward negative infinity. */
std::int64_t floor_divide(std::int64_t numerator, std::int64_t denominator) {
  const std::int64_t quotient = numerator / denominator;
  return (numerator % denominator != 0 && (numerator < 0) != (denominator < 0)) ? quotient - 1 : quotient;
}

// The proleptic Gregorian calendar, by Howard Hinnant's algorithms (chrono's own), on 64-bit days: PostgreSQL's dates reach
// past the years `std::chrono::year` holds. Year 0 is 1 BC.
// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers) -- the algorithms' own constants (days in 400 years, ...)

struct Civil {
  std::int64_t year = 0;
  unsigned     month = 0;
  unsigned     day = 0;
};

std::int64_t days_from_civil(const Civil& date) {
  const std::int64_t  year = date.month <= 2 ? date.year - 1 : date.year;
  const std::int64_t  era = floor_divide(year, 400);
  const auto          year_of_era = static_cast<std::uint64_t>(year - (era * 400));
  const std::uint64_t shifted_month = date.month > 2 ? date.month - 3 : date.month + 9;
  const std::uint64_t day_of_year = (((153 * shifted_month) + 2) / 5) + date.day - 1;
  const std::uint64_t day_of_era = (year_of_era * 365) + (year_of_era / 4) - (year_of_era / 100) + day_of_year;
  return (era * 146'097) + static_cast<std::int64_t>(day_of_era) - 719'468;
}

Civil civil_from_days(std::int64_t days) {
  const std::int64_t  shifted = days + 719'468;
  const std::int64_t  era = floor_divide(shifted, 146'097);
  const auto          day_of_era = static_cast<std::uint64_t>(shifted - (era * 146'097));
  const std::uint64_t year_of_era = (day_of_era - (day_of_era / 1460) + (day_of_era / 36'524) - (day_of_era / 146'096)) / 365;
  const std::uint64_t day_of_year = day_of_era - ((365 * year_of_era) + (year_of_era / 4) - (year_of_era / 100));
  const std::uint64_t shifted_month = ((5 * day_of_year) + 2) / 153;
  const auto          day = static_cast<unsigned>(day_of_year - (((153 * shifted_month) + 2) / 5) + 1);
  const auto          month = static_cast<unsigned>(shifted_month < 10 ? shifted_month + 3 : shifted_month - 9);
  const std::int64_t  year = static_cast<std::int64_t>(year_of_era) + (era * 400) + (month <= 2 ? 1 : 0);
  return Civil{.year = year, .month = month, .day = day};
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

bool valid_civil(const Civil& date) {
  if ( date.month < 1 || date.month > MONTHS_PER_YEAR || date.day < 1 ) {
    return false;
  }
  const Civil after = civil_from_days(days_from_civil(date));
  return after.year == date.year && after.month == date.month && after.day == date.day;  // rejects Feb 30 and the like
}

/** @brief How many digits a number has: at least `least`, at most `most`. */
struct Width {
  std::size_t least = 0;
  std::size_t most = 0;
};

/** @brief Reads a text form from left to right; every read refuses the whole text if it does not find what it expects. */
class Reader {
 public:
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- the text, then what it should be: in that order at each of the three calls
  Reader(std::string_view text, std::string_view what) : _text(text), _rest(text), _what(what) {}

  [[nodiscard]] bool at_end() const { return _rest.empty(); }
  [[nodiscard]] bool next_is(char chr) const { return ! _rest.empty() && _rest.front() == chr; }

  void expect(char chr) {
    if ( ! next_is(chr) ) {
      refuse(_what, _text);
    }
    _rest.remove_prefix(1);
  }

  /** @return Whether the text goes on with `word`, which is then read. */
  bool take(std::string_view word) {
    if ( ! _rest.starts_with(word) ) {
      return false;
    }
    _rest.remove_prefix(word.size());
    return true;
  }

  /** @return A number of `width` digits, and how many it had. */
  std::int64_t digits(Width width, std::size_t* count = nullptr) {
    std::size_t length = 0;
    while ( length < _rest.size() && length < width.most && _rest.at(length) >= '0' && _rest.at(length) <= '9' ) {
      ++length;
    }
    if ( length < width.least ) {
      refuse(_what, _text);
    }
    std::int64_t value = 0;
    const char*  begin = _rest.data();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic) -- the end of the digits just counted
    if ( const auto [end, error] = std::from_chars(begin, begin + length, value); error != std::errc{} ) {
      refuse(_what, _text);
    }
    _rest.remove_prefix(length);
    if ( count != nullptr ) {
      *count = length;
    }
    return value;
  }

  void refuse_if_not_at_end() const {
    if ( ! at_end() ) {
      refuse(_what, _text);
    }
  }

  [[noreturn]] void refuse_here() const { refuse(_what, _text); }

 private:
  std::string_view _text;
  std::string_view _rest;
  std::string_view _what;
};

constexpr Width TWO_DIGITS{.least = 2, .most = 2};

/** @brief Reads `YYYY-MM-DD` (four or more digits of year). */
Civil read_civil(Reader& reader) {
  constexpr std::size_t MAX_YEAR_DIGITS = 7;
  Civil                 date;
  date.year = reader.digits({.least = 4, .most = MAX_YEAR_DIGITS});
  reader.expect('-');
  date.month = static_cast<unsigned>(reader.digits(TWO_DIGITS));
  reader.expect('-');
  date.day = static_cast<unsigned>(reader.digits(TWO_DIGITS));
  return date;
}

/** @brief Reads ` BC` if it is there: then the year read is before the common era (1 BC is year 0). */
void read_era(Reader& reader, Civil& date) {
  if ( reader.take(" BC") ) {
    date.year = 1 - date.year;
  }
}

/** @return The text form of `date`'s year, month and day, and whether it is BC (which goes at the end of the text). */
std::string civil_text(const Civil& date, bool& before_common_era) {
  before_common_era = date.year <= 0;
  const std::int64_t shown_year = before_common_era ? 1 - date.year : date.year;
  return std::format("{:04}-{:02}-{:02}", shown_year, date.month, date.day);
}

}  // namespace

std::string format_timestamp(std::int64_t micros, bool zoned) {
  if ( micros == INFINITE_FUTURE ) {
    return "infinity";
  }
  if ( micros == INFINITE_PAST ) {
    return "-infinity";
  }
  const std::int64_t days = floor_divide(micros, MICROS_PER_DAY);
  const std::int64_t of_day = micros - (days * MICROS_PER_DAY);
  const std::int64_t seconds = of_day / MICROS_PER_SECOND;
  bool               before_common_era = false;
  std::string        text = civil_text(civil_from_days(days), before_common_era);
  text += std::format(" {:02}:{:02}:{:02}.{:06}", seconds / SECONDS_PER_HOUR, (seconds % SECONDS_PER_HOUR) / SECONDS_PER_MINUTE,
                      seconds % SECONDS_PER_MINUTE, of_day % MICROS_PER_SECOND);
  if ( zoned ) {
    text += "+00";
  }
  if ( before_common_era ) {
    text += " BC";
  }
  return text;
}

std::int64_t parse_timestamp(std::string_view text, bool zoned) {
  if ( text == "infinity" ) {
    return INFINITE_FUTURE;
  }
  if ( text == "-infinity" ) {
    return INFINITE_PAST;
  }
  Reader reader(text, zoned ? "a timestamptz in ISO form" : "a timestamp in ISO form");
  Civil  date = read_civil(reader);
  if ( ! reader.take(" ") && ! reader.take("T") ) {
    reader.refuse_here();
  }
  const std::int64_t hours = reader.digits(TWO_DIGITS);
  reader.expect(':');
  const std::int64_t minutes = reader.digits(TWO_DIGITS);
  reader.expect(':');
  const std::int64_t seconds = reader.digits(TWO_DIGITS);
  std::int64_t       fraction = 0;
  if ( reader.take(".") ) {
    std::size_t count = 0;
    fraction = reader.digits({.least = 1, .most = MAX_FRACTION_DIGITS}, &count);
    for ( ; count < MAX_FRACTION_DIGITS; ++count ) {
      fraction *= DECIMAL_BASE;
    }
  }
  std::int64_t offset_seconds = 0;
  if ( zoned ) {
    const bool behind = reader.next_is('-');
    if ( ! reader.take("+") && ! reader.take("-") ) {
      reader.refuse_here();
    }
    offset_seconds = reader.digits(TWO_DIGITS) * SECONDS_PER_HOUR;
    if ( reader.take(":") ) {
      offset_seconds += reader.digits(TWO_DIGITS) * SECONDS_PER_MINUTE;
      if ( reader.take(":") ) {
        offset_seconds += reader.digits(TWO_DIGITS);
      }
    }
    offset_seconds = behind ? -offset_seconds : offset_seconds;
  }
  read_era(reader, date);
  reader.refuse_if_not_at_end();
  if ( ! valid_civil(date) || hours > LAST_HOUR || minutes > LAST_MINUTE || seconds > LAST_SECOND ) {
    reader.refuse_here();
  }
  const std::int64_t of_day =
      (((hours * SECONDS_PER_HOUR) + (minutes * SECONDS_PER_MINUTE) + seconds - offset_seconds) * MICROS_PER_SECOND) + fraction;
  return checked_add(checked_multiply(days_from_civil(date), MICROS_PER_DAY), of_day);
}

std::int64_t parse_binary_timestamp(std::span<const std::byte> bytes) {
  constexpr std::size_t TIMESTAMP_BYTES = 8;
  if ( bytes.size() != TIMESTAMP_BYTES ) {
    throw std::invalid_argument("parse_binary: a timestamp is 8 bytes, not " + std::to_string(bytes.size()));
  }
  const std::int64_t since_2000 = binary_integer(bytes);
  if ( since_2000 == std::numeric_limits<std::int64_t>::max() ) {
    return INFINITE_FUTURE;
  }
  if ( since_2000 == std::numeric_limits<std::int64_t>::min() ) {
    return INFINITE_PAST;
  }
  return checked_add(since_2000, MICROS_TO_POSTGRES_EPOCH);
}

std::string format_date(std::int64_t days) {
  if ( days == INFINITE_FUTURE ) {
    return "infinity";
  }
  if ( days == INFINITE_PAST ) {
    return "-infinity";
  }
  bool              before_common_era = false;
  const std::string text = civil_text(civil_from_days(days), before_common_era);
  return before_common_era ? text + " BC" : text;
}

std::int64_t parse_date(std::string_view text) {
  if ( text == "infinity" ) {
    return INFINITE_FUTURE;
  }
  if ( text == "-infinity" ) {
    return INFINITE_PAST;
  }
  Reader reader(text, "a date in ISO form");
  Civil  date = read_civil(reader);
  read_era(reader, date);
  reader.refuse_if_not_at_end();
  if ( ! valid_civil(date) ) {
    reader.refuse_here();
  }
  return days_from_civil(date);
}

std::int64_t parse_binary_date(std::span<const std::byte> bytes) {
  constexpr std::size_t DATE_BYTES = 4;
  if ( bytes.size() != DATE_BYTES ) {
    throw std::invalid_argument("parse_binary: a date is 4 bytes, not " + std::to_string(bytes.size()));
  }
  const std::int64_t since_2000 = binary_integer(bytes);
  if ( since_2000 == std::numeric_limits<std::int32_t>::max() ) {
    return INFINITE_FUTURE;
  }
  if ( since_2000 == std::numeric_limits<std::int32_t>::min() ) {
    return INFINITE_PAST;
  }
  return since_2000 + DAYS_TO_POSTGRES_EPOCH;
}

namespace {

constexpr std::chrono::year_month_day LATEST_DATE{std::chrono::year::max(), std::chrono::December, std::chrono::day{31}};
constexpr std::chrono::year_month_day EARLIEST_DATE{std::chrono::year::min(), std::chrono::January, std::chrono::day{1}};

}  // namespace

std::int64_t days_of(const std::chrono::year_month_day& date) {
  if ( date == LATEST_DATE ) {
    return INFINITE_FUTURE;
  }
  if ( date == EARLIEST_DATE ) {
    return INFINITE_PAST;
  }
  if ( ! date.ok() ) {
    throw std::invalid_argument("format_field: the date does not exist");
  }
  return std::chrono::sys_days(date).time_since_epoch().count();
}

std::chrono::year_month_day date_of(std::int64_t days) {
  if ( days == INFINITE_FUTURE ) {
    return LATEST_DATE;
  }
  if ( days == INFINITE_PAST ) {
    return EARLIEST_DATE;
  }
  const std::int64_t latest = std::chrono::sys_days(LATEST_DATE).time_since_epoch().count();
  const std::int64_t earliest = std::chrono::sys_days(EARLIEST_DATE).time_since_epoch().count();
  if ( days >= latest || days <= earliest ) {  // those two stand for infinity
    throw std::invalid_argument("the date is outside the years std::chrono::year holds");
  }
  return std::chrono::year_month_day{std::chrono::sys_days{std::chrono::days{days}}};
}

}  // namespace schemacht::postgres::detail
