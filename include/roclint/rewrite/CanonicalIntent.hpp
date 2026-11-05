#pragma once

#include "roclint/analysis/SourceSpan.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace roclint::core::rewrite {

enum class IntentAction {
    ADD,
    REMOVE,
    MODIFY,
    CONSTRAIN,
    PRESERVE
};

[[nodiscard]] const char* toString(IntentAction action) noexcept;

// A deterministic, source-linked action triple. nodeId is stable for identical
// action/target/property/value/source inputs. disposition records how the triple
// entered the current turn (for example explicit_prompt).
struct CanonicalIntent {
    IntentAction action{IntentAction::MODIFY};
    std::string target{};
    std::string property{};
    std::string value{};
    std::string disposition{};
    std::string condition{};
    std::string nodeId{};
    analysis::SourceSpan source{};

    bool operator==(const CanonicalIntent&) const = default;
};

class CanonicalIntentExtractor {
public:
    // Extracts conservative action triples from explicit prompt language. Quoted
    // examples are treated as data, not instructions.
    [[nodiscard]] static std::vector<CanonicalIntent> extract(std::string_view promptText);

    // Compact JSON array suitable for embedding in discovery/rewrite instructions.
    [[nodiscard]] static std::string toJson(const std::vector<CanonicalIntent>& intents);

    [[nodiscard]] static std::string makeNodeId(const CanonicalIntent& intent);

    static constexpr std::size_t kMaxPromptBytes = 64 * 1024;
    static constexpr std::size_t kMaxIntents = 32;
};

} // namespace roclint::core::rewrite
