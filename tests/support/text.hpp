#pragma once

#include <string_view>

namespace schemacht::test {

/** @return Whether `text` has `part` in it: `std::string_view::contains`, which is C++23. */
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- the text and a part of it, in that order as `contains` reads
[[nodiscard]] inline bool contains(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

}  // namespace schemacht::test
