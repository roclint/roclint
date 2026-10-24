#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace roclint::runtime {

// Only the pinned converter's outside-string whitespace rule is replaced.
// String/escape rules and the actual JSON schema remain byte-for-byte intact.
// A converter change must be reviewed; never silently use an unknown grammar.
inline std::optional<std::string> compactGraniteJsonGrammar(std::string grammar) {
    constexpr std::string_view expected = R"(space ::= | " " | "\n"{1,2} [ \t]{0,20})";
    constexpr std::string_view replacement = R"(space ::= "")";
    std::size_t position = 0;
    std::optional<std::size_t> found;
    while (position < grammar.size()) {
        const auto end = grammar.find('\n', position);
        const auto length = end == std::string::npos ? grammar.size() - position : end - position;
        const auto line = std::string_view(grammar).substr(position, length);
        if (line.starts_with("space ::=")) {
            if (found || line != expected) return std::nullopt;
            found = position;
        }
        if (end == std::string::npos) break;
        position = end + 1;
    }
    if (!found) return std::nullopt;
    grammar.replace(*found, expected.size(), replacement);
    return grammar;
}

} // namespace roclint::runtime
