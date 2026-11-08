#pragma once

#include "roclint/core/scoped_constraint.hpp"
#include "roclint/core/validation_result.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace roclint::core::rewrite {

struct CreativeAlternative {
    std::string id{};               // e.g. "alt_direct", "alt_flow", "alt_stack"
    std::string title{};            // "Direct / Minimal", "Flow & Dynamics", "Stack & Topology"
    std::string styleDescriptor{};  // Short explanation of aesthetic/structural intent
    std::string rewrittenText{};    // Proposal text for this alternative
    std::string diffText{};         // Unified/accessible diff against original prompt
    ValidationOutcome auditOutcome{ValidationOutcome::Pass};

    bool operator==(const CreativeAlternative&) const = default;
};

class CreativeExpansionEngine {
public:
    // Expands a base rewrite proposal into 2-3 distinct aesthetic/structural alternatives
    // across orthogonal design axes, ensuring all hard invariants and prohibitions are preserved.
    [[nodiscard]] static std::vector<CreativeAlternative> expand(
        std::string_view originalPrompt,
        std::string_view baseRewrittenText,
        const std::vector<ScopedConstraint>& constraints);
};

} // namespace roclint::core::rewrite
