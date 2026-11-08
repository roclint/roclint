#pragma once

#include "roclint/core/scoped_constraint.hpp"
#include "roclint/core/validation_result.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace roclint::core {

enum class CitationVerdict : std::uint8_t {
    Verified,      // Evidence is valid and directly supports the derivation
    Overreached,   // Evidence exists but does not justify the claimed rule/derivation
    InvalidSpan,   // Spans are out of bounds, sliced UTF-8, or missing
    Fabricated     // Cited text does not exist or authority reference is fake
};

[[nodiscard]] const char* toString(CitationVerdict verdict) noexcept;
[[nodiscard]] Result<CitationVerdict> parseCitationVerdict(std::string_view text) noexcept;

struct CitationEvaluation {
    std::string constraintId;
    CitationVerdict verdict = CitationVerdict::InvalidSpan;
    EvidenceStatus assignedStatus = EvidenceStatus::Unverified;
    std::string explanation;

    // The adjusted constraint with authoritative provenance assigned:
    ScopedConstraint verifiedConstraint;

    bool operator==(const CitationEvaluation&) const = default;
};

[[nodiscard]] json::Value toJson(const CitationEvaluation& eval);
[[nodiscard]] Result<CitationEvaluation> citationEvaluationFromJson(const json::Value& val);

class CitationEvaluator {
public:
    CitationEvaluator() = default;

    // Evaluates a candidate constraint against a source prompt document.
    // promptId corresponds to SourceSpan::sourceId for the user prompt (default "prompt").
    [[nodiscard]] CitationEvaluation evaluate(
        std::string_view promptText,
        std::string_view promptId,
        const ScopedConstraint& candidate) const;

    // Evaluates a batch of candidate constraints against the prompt.
    [[nodiscard]] std::vector<CitationEvaluation> evaluateBatch(
        std::string_view promptText,
        std::string_view promptId,
        const std::vector<ScopedConstraint>& candidates) const;

    // Register a recognized authority reference prefix (e.g., "RFC ", "ISO ", "AST:")
    void registerAuthorityPrefix(std::string prefix);

    [[nodiscard]] bool isRecognizedAuthority(std::string_view authorityRef) const noexcept;

private:
    std::vector<std::string> authorityPrefixes_{
        "RFC ", "ISO ", "ECMA ", "IEEE ", "W3C ", "AST:"
    };
};

} // namespace roclint::core
