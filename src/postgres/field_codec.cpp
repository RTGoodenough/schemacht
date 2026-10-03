#include "schemacht/postgres/field_codec.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace schemacht::postgres {

namespace {

constexpr std::size_t CHAR_VALUES = 256;
constexpr std::size_t MESSAGE_TEXT_LIMIT = 80;  // how much of an array's text an error message quotes

/** @brief The characters that end an unquoted array element, or cannot be inside one: `,` `"` `{` `}`. */
constexpr std::array<bool, CHAR_VALUES> make_bare_element_ends() {
  std::array<bool, CHAR_VALUES> ends{};
  for ( const char chr : {',', '"', '{', '}'} ) {
    ends.at(static_cast<unsigned char>(chr)) = true;
  }
  return ends;
}

constexpr std::array<bool, CHAR_VALUES> ENDS_BARE_ELEMENT = make_bare_element_ends();

constexpr bool is_space(char chr) { return chr == ' ' || chr == '\t' || chr == '\n' || chr == '\r' || chr == '\v' || chr == '\f'; }

std::string_view trimmed(std::string_view text) {
  while ( ! text.empty() && is_space(text.front()) ) {
    text.remove_prefix(1);
  }

  while ( ! text.empty() && is_space(text.back()) ) {
    text.remove_suffix(1);
  }
  return text;
}

bool is_null_word(std::string_view token) {
  constexpr std::string_view NULL_WORD = "null";
  return token.size() == NULL_WORD.size() &&
         std::ranges::equal(token, NULL_WORD, [](char left, char right) { return left == right || left == static_cast<char>(right - 'a' + 'A'); });
}

}  // namespace

void ArrayReader::fail(std::string_view reason) const {
  std::string text(_text.substr(0, MESSAGE_TEXT_LIMIT));
  if ( _text.size() > MESSAGE_TEXT_LIMIT ) {
    text += "...";
  }

  throw std::invalid_argument("parse_field: the array " + std::string(reason) + ": '" + text + "'");
}

ArrayReader::ArrayReader(std::string_view text) : _text(text) {
  const std::string_view whole = trimmed(text);
  if ( ! whole.empty() && whole.front() == '[' ) {
    fail("has a dimension prefix ([lower:upper]=), which is not supported");
  }

  if ( whole.empty() || whole.front() != '{' ) {
    fail("does not start with '{'");
  }

  if ( whole.back() != '}' ) {
    fail("does not end with '}'");
  }

  _body = trimmed(whole.substr(1, whole.size() - 2));
  if ( ! _body.empty() ) {
    _estimate = static_cast<std::size_t>(std::ranges::count(_body, ',')) + 1;
  }
}

void ArrayReader::skip_spaces() {
  while ( _at < _body.size() && is_space(_body.at(_at)) ) {
    ++_at;
  }
}

std::string_view ArrayReader::bare_element() {
  // One pass to the comma that ends the element; a quote or a brace on the way is not allowed in an unquoted element.
  const std::string_view rest = _body.substr(_at);
  const auto* const      stop = std::ranges::find_if(rest, [](char chr) { return ENDS_BARE_ELEMENT.at(static_cast<unsigned char>(chr)); });
  const auto             length = static_cast<std::size_t>(stop - rest.begin());
  std::string_view       token = rest.substr(0, length);
  _at += length;  // at the comma, if there is one: `next` looks at it

  if ( stop != rest.end() && *stop != ',' ) {
    fail(*stop == '"' ? "has a quote inside an unquoted element" : "is nested or has a brace inside an unquoted element");
  }

  if ( ! token.empty() && (is_space(token.front()) || is_space(token.back())) ) {
    token = trimmed(token);
  }

  if ( token.empty() ) {
    fail("has an empty element (an empty string is written \"\")");
  }

  if ( is_null_word(token) ) {
    fail("has a NULL element, which a std::vector cannot hold");
  }

  return token;
}

std::string_view ArrayReader::quoted_element() {
  const std::size_t start = _at + 1;  // past the opening quote
  std::size_t       end = start;
  bool              escaped = false;
  while ( end < _body.size() && _body.at(end) != '"' ) {
    if ( _body.at(end) == '\\' ) {
      escaped = true;
      ++end;  // the character after a backslash is data, even a quote
    }
    ++end;
  }

  if ( end >= _body.size() ) {
    fail("has a string that is never closed");
  }

  const std::string_view raw = _body.substr(start, end - start);
  _at = end + 1;

  if ( ! escaped ) {
    return raw;
  }

  _unescaped.clear();
  for ( std::size_t i = 0; i < raw.size(); ++i ) {
    if ( raw.at(i) == '\\' && i + 1 < raw.size() ) {
      ++i;
    }
    _unescaped += raw.at(i);
  }

  return _unescaped;
}

std::optional<std::string_view> ArrayReader::next() {
  skip_spaces();
  if ( _at >= _body.size() ) {
    if ( _after_comma ) {
      fail("has a comma with no element after it");
    }
    return std::nullopt;
  }

  _after_comma = false;
  const std::string_view element = _body.at(_at) == '"' ? quoted_element() : bare_element();

  skip_spaces();
  if ( _at < _body.size() ) {
    if ( _body.at(_at) != ',' ) {
      fail("has text after a string that is not a comma");
    }
    ++_at;
    _after_comma = true;
  }

  return element;
}

}  // namespace schemacht::postgres
