#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace roclint::core::rewrite {

// A same-target contradiction detected without invoking a model. Both requirement
// fragments are literal substrings of the analysed prompt so callers can resolve
// authoritative SourceSpans with findUniqueSpan().
struct DetectedConflict {
    std::string requirementA{};
    std::string requirementB{};
    std::string subject{};

    bool operator==(const DetectedConflict&) const = default;
};

struct PreFlightExtractionReport {
    // Explicit, syntactically visible contradictions between requirements that
    // constrain the same subject. Empty when no conservative match is found.
    std::vector<DetectedConflict> detectedExplicitConflicts{};
};

// Deterministic, model-free extraction of explicit contradictions. The detector is
// intentionally conservative: it only reports same-subject conflicts backed by the
// five contradiction patterns in the Phase 4 taxonomy, and suppresses known
// disjoint-scope pairs (for example critical/informational or desktop/mobile).
class StructuredEntityExtractor {
public:
    [[nodiscard]] static PreFlightExtractionReport analyze(std::string_view promptText);

    // Upper bound on prompt bytes inspected. Larger inputs skip extraction instead
    // of adding unbounded regex work ahead of the model call.
    static constexpr std::size_t kMaxPromptBytes = 64 * 1024;
};

} // namespace roclint::core::rewrite
