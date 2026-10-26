#pragma once

#include "roclint/core/Result.hpp"
#include <string_view>

namespace roclint::bridge {

inline std::string_view trimModelWhitespace(std::string_view text) {
    constexpr std::string_view whitespace = " \t\r\n\f\v";
    const auto first = text.find_first_not_of(whitespace);
    if (first == std::string_view::npos) return {};
    return text.substr(first, text.find_last_not_of(whitespace) - first + 1);
}

// Only unwrap a whole, unambiguous fence. JSON parsing still validates the entire body.
inline core::Result<std::string_view> normalizeModelResponseEnvelope(std::string_view text, std::size_t limit) {
    if (text.size() > limit)
        return core::makeError(core::ErrorCode::InvalidInput, "Model response exceeds its byte limit");
    text = trimModelWhitespace(text);
    if (!text.starts_with("```")) return text;
    const auto newline = text.find('\n');
    if (newline == std::string_view::npos)
        return core::makeError(core::ErrorCode::InvalidInput, "Incomplete model response fence");
    const auto opening = trimModelWhitespace(text.substr(0, newline));
    if (opening != "```" && opening != "```json")
        return core::makeError(core::ErrorCode::InvalidInput, "Unsupported model response fence");
    if (!text.ends_with("\n```") || text.size() < newline + 5)
        return core::makeError(core::ErrorCode::InvalidInput, "Incomplete model response fence");
    auto body = trimModelWhitespace(text.substr(newline + 1, text.size() - newline - 5));
    // Reject another fence line, while allowing backticks inside JSON strings.
    if (body.starts_with("```") || body.find("\n```") != std::string_view::npos || body.find("\n ```") != std::string_view::npos)
        return core::makeError(core::ErrorCode::InvalidInput, "Multiple model response fences");
    return body;
}

} // namespace roclint::bridge
