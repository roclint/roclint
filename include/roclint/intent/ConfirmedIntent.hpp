#pragma once

#include "roclint/analysis/NormalizedAnalysis.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace roclint::core::intent {

enum class IntentResolutionKind {
    SelectedAlternative,
    CustomText,
    UseOriginal,
    Withdrawn
};

struct ConfirmedItem {
    std::string itemId{};
    IntentResolutionKind kind{IntentResolutionKind::SelectedAlternative};
    std::string chosenValue{};
    std::string rationale{};

    bool operator==(const ConfirmedItem&) const = default;
};

struct ConfirmedIntent {
    std::string id{};
    std::string baseRevisionId{};
    std::string sourceSnapshotId{};
    std::uint64_t sourceSnapshotHash{0};
    std::vector<ConfirmedItem> items{};
    std::string customInstructions{};
    std::uint64_t createdAtTimestamp{0};
    bool isCurrent{true};

    [[nodiscard]] bool hasDecisionFor(const std::string& itemId) const noexcept;
    [[nodiscard]] std::optional<ConfirmedItem> getDecision(const std::string& itemId) const noexcept;

    bool operator==(const ConfirmedIntent&) const = default;
};

class IntentProvenanceBuilder {
public:
    [[nodiscard]] static std::uint64_t computeSnapshotHash(
        const analysis::NormalizedAnalysis& snapshot) noexcept;

    [[nodiscard]] static ConfirmedIntent buildConfirmedIntent(
        std::string_view baseRevisionId,
        const analysis::NormalizedAnalysis& sourceSnapshot,
        const std::vector<ConfirmedItem>& explicitItems,
        std::string_view customInstructions = "") noexcept;

    [[nodiscard]] static bool isIntentValid(
        const ConfirmedIntent& intent,
        std::string_view currentRevisionId,
        const analysis::NormalizedAnalysis& currentSnapshot) noexcept;
};

[[nodiscard]] const char* toString(IntentResolutionKind kind) noexcept;

} // namespace roclint::core::intent
