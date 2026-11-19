#pragma once

// Shared comprehension-review contract for the Roclint companion/interview/review
// journey. Types are owned by the implementation coordinator; the Goldfish
// adapter implements the free functions, and HostController consumes them.
//
// A review request is self-contained: candidate, original source, authoritative
// confirmed decisions and verbatim source evidence. It must never contain the
// drafting conversation, hidden rationale or persuasive generation notes.

#include "roclint/core/Result.hpp"
#include "roclint/core/scoped_constraint.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace roclint::bridge {

enum class ReviewFindingSeverity : std::uint8_t {
    Advisory,
    Required,
    Blocking
};

enum class ReviewFindingKind : std::uint8_t {
    ComprehensionGap,
    MeaningDrift,
    Omission,
    UnsupportedAssumption,
    NegativeConstraint,
    Readiness
};

enum class ComprehensionReviewStatus : std::uint8_t {
    Completed,
    Unavailable,
    Malformed,
    Rejected
};

struct ComprehensionReviewRequest {
    std::string candidatePrompt{};   // Proposed prompt the reviewer interprets
    std::string originalPrompt{};    // Frozen source draft, verbatim
    std::vector<core::ScopedConstraint> constraints{}; // Source-linked constraints
    std::vector<std::string> confirmedDecisions{};     // Authoritative wording only
    std::vector<std::string> sourceExcerpts{};         // Verbatim evidence excerpts
    std::string taskKind{};          // creative | code | performance | general
    std::string sessionId{};
    std::string revisionId{};
};

struct ComprehensionFinding {
    std::string id{};
    ReviewFindingKind kind{ReviewFindingKind::ComprehensionGap};
    ReviewFindingSeverity severity{ReviewFindingSeverity::Advisory};
    std::string summary{};           // Actionable, plain language
    std::string sourceRef{};         // Constraint/item/evidence identity, may be empty
    std::string sourceExcerpt{};     // Verbatim source text, may be empty
    bool requiresUserDecision{false};
    std::string proposedRepair{};    // Bounded proposal, never auto-applied
};

struct ComprehensionReviewResult {
    ComprehensionReviewStatus status{ComprehensionReviewStatus::Unavailable};
    std::string interpretedOutcome{};                  // Reviewer's inference; not authority
    std::vector<std::string> interpretedRequirements{};
    std::vector<ComprehensionFinding> findings{};
    bool readyForTask{false};
    std::string rationale{};
};

[[nodiscard]] const char* toString(ReviewFindingSeverity severity) noexcept;
[[nodiscard]] const char* toString(ReviewFindingKind kind) noexcept;
[[nodiscard]] const char* toString(ComprehensionReviewStatus status) noexcept;

// True only for a completed review that found the candidate ready and has no
// unresolved blocking/required finding. Callers must additionally verify that
// the review is bound to the current revision/proposal.
[[nodiscard]] bool reviewAuthorizesApply(const ComprehensionReviewResult& result) noexcept;

// Fresh-context prompt and strict schema. The prompt includes the candidate,
// original, confirmed decisions and verbatim source evidence only.
[[nodiscard]] std::string buildComprehensionReviewPrompt(const ComprehensionReviewRequest& request);
[[nodiscard]] const std::string& comprehensionReviewResponseSchema();

// Strict parser. Unknown severities/kinds, missing candidate interpretation,
// duplicate findings or malformed JSON fail closed (Malformed), never Pass.
[[nodiscard]] core::Result<ComprehensionReviewResult> parseComprehensionReviewResponse(
    std::string_view modelOutput,
    const ComprehensionReviewRequest& request);

using ComprehensionReviewTransport = std::function<core::Result<std::string>(std::string_view)>;

// Builds the prompt, invokes the transport once, and parses the response.
// Empty transport or failed transport returns Unavailable; malformed output
// returns Malformed. No repair loop belongs in this call.
[[nodiscard]] ComprehensionReviewResult reviewComprehension(
    const ComprehensionReviewRequest& request,
    const ComprehensionReviewTransport& transport = {});

} // namespace roclint::bridge
