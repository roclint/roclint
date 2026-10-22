#include "roclint/analysis/SourceSpan.hpp"

namespace roclint::core::analysis {

bool isUtf8Boundary(std::string_view text, std::size_t byteOffset) noexcept {
    if (byteOffset > text.size()) {
        return false;
    }
    if (byteOffset == 0 || byteOffset == text.size()) {
        return true;
    }
    const auto byte = static_cast<unsigned char>(text[byteOffset]);
    // In UTF-8, continuation bytes have bit pattern 10xxxxxx (0x80 to 0xBF).
    // A code point start (or ASCII) does NOT have (byte & 0xC0) == 0x80.
    return (byte & 0xC0) != 0x80;
}

bool isValidSpan(std::string_view text, const SourceSpan& span) noexcept {
    if (span.startByte > span.endByte) {
        return false;
    }
    if (span.endByte > text.size()) {
        return false;
    }
    return isUtf8Boundary(text, span.startByte) && isUtf8Boundary(text, span.endByte);
}


std::optional<std::string_view> extractSpan(std::string_view text, const SourceSpan& span) noexcept {
    if (!isValidSpan(text, span)) {
        return std::nullopt;
    }
    return text.substr(span.startByte, span.length());
}

SpanMatchResult findUniqueSpan(std::string_view text, std::string_view excerpt) noexcept {
    if (excerpt.empty() || excerpt.size() > text.size()) {
        return SpanMatchResult{SpanMatchStatus::NotFound, std::nullopt};
    }

    const auto firstPos = text.find(excerpt);
    if (firstPos == std::string_view::npos) {
        return SpanMatchResult{SpanMatchStatus::NotFound, std::nullopt};
    }

    // Ambiguous repeated matches must not invent a false authoritative span
    const auto secondPos = text.find(excerpt, firstPos + 1);
    if (secondPos != std::string_view::npos) {
        return SpanMatchResult{SpanMatchStatus::AmbiguousMultiple, std::nullopt};
    }

    SourceSpan candidateSpan{
        .startByte = firstPos,
        .endByte = firstPos + excerpt.size()
    };

    if (!isValidSpan(text, candidateSpan)) {
        return SpanMatchResult{SpanMatchStatus::NotFound, std::nullopt};
    }

    return SpanMatchResult{
        .status = SpanMatchStatus::ExactMatch,
        .span = candidateSpan
    };
}

} // namespace roclint::core::analysis
