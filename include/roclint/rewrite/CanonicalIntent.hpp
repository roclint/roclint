#pragma once

#include "roclint/analysis/SourceSpan.hpp"
#include "roclint/storage/AstSymbol.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
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

enum class IntentTargetKind { CodeEntity, GeneralConstraint };

[[nodiscard]] const char* toString(IntentAction action) noexcept;

enum class GroundingStatus : std::uint8_t {
    Ungrounded = 0,
    Verified,
    Ambiguous,
    NotFoundInScope,
    TypeMismatch
};

struct AstGroundingEvidence {
    GroundingStatus status{GroundingStatus::Ungrounded};
    std::string qualifiedTarget{};
    storage::AstNodeKind targetKind{storage::AstNodeKind::Unknown};
    std::string filePath{};
    std::string fileHash{};
    analysis::SourceSpan fileSpan{};
    std::uint32_t line{0};
    std::string signature{};
    std::vector<std::string> candidates{};

    // False when a candidate query used for this decision was truncated or
    // failed, or when the index generation changed during the batch. Truncated
    // evidence can never be Verified or prove absence.
    bool complete{true};
    // Host-set freshness metadata. Grounding leaves this true; the host sets it
    // false when validateFileHash reports that the selected file bytes changed.
    bool hashValidated{true};
    // Stable reason code for this resolution attempt: "file", "signature",
    // "parameter", "scope", "qualified", "active-file" or "unique". Empty when
    // no match strategy applies (unsupported form, ungrounded ADD, ...).
    std::string matchReason{};

    bool operator==(const AstGroundingEvidence&) const = default;
};

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
    std::optional<AstGroundingEvidence> grounding{};
    // General requirements remain source-linked intent, but are not AST symbols.
    IntentTargetKind targetKind{IntentTargetKind::CodeEntity};
    std::string sourceText{}; // Complete explicit clause, including negation/preservation.

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
