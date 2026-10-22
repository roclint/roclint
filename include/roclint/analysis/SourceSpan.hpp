#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

namespace roclint::core::analysis 
{

/// Represents a half-open byte range [startByte, endByte) within a UTF-8 prompt buffer.
struct SourceSpan {
    std::size_t startByte = 0;
    std::size_t endByte = 0;

    [[nodiscard]] constexpr std::size_t length() const noexcept 
    {
        if (endByte >= startByte) 
        {
            return endByte - startByte;
        }
        return 0;
    }
    
    [[nodiscard]] constexpr bool empty() const noexcept 
    {
        return startByte == endByte;
    }

    bool operator==(const SourceSpan&) const = default;
};

/// Status result when resolving a model excerpt against the prompt.
enum class SpanMatchStatus {
    ExactMatch,       // Appears exactly once in the buffer.
    AmbiguousMultiple,// Appears multiple times; cannot invent an authoritative span.
    NotFound          // Excerpt does not exist in the prompt buffer.
};

struct SpanMatchResult {
    SpanMatchStatus status{SpanMatchStatus::NotFound};
    std::optional<SourceSpan> span{std::nullopt};
};

// Checks whether the given byte offset lands on a valid UTF-8 code point boundary.
[[nodiscard]] bool isUtf8Boundary(std::string_view text, std::size_t byteOffset) noexcept;

// Validates that span bounds are within the text and do not slice multi-byte UTF-8 sequences.
[[nodiscard]] bool isValidSpan(std::string_view text, const SourceSpan& span) noexcept;

// Safely extracts the substring slice if the span is valid.
[[nodiscard]] std::optional<std::string_view> extractSpan(std::string_view text, const SourceSpan& span) noexcept;

// locates an excerpt in text, strictly guarding against ambiguous repeated occurrences.
[[nodiscard]] SpanMatchResult findUniqueSpan(std::string_view text, std::string_view excerpt) noexcept;

} 