#include "roclint/intent/ConfirmedIntent.hpp"

#include <chrono>
#include <iomanip>
#include <sstream>

namespace roclint::core::intent {

bool ConfirmedIntent::hasDecisionFor(const std::string& itemId) const noexcept {
    for (const auto& item : items) {
        if (item.itemId == itemId) {
            return true;
        }
    }
    return false;
}

std::optional<ConfirmedItem> ConfirmedIntent::getDecision(const std::string& itemId) const noexcept {
    for (const auto& item : items) {
        if (item.itemId == itemId) {
            return item;
        }
    }
    return std::nullopt;
}

namespace {

constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void fnv1aUpdate(std::uint64_t& hash, std::string_view str) noexcept {
    for (char c : str) {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= kFnvPrime;
    }
}

void fnv1aUpdateByte(std::uint64_t& hash, std::uint8_t b) noexcept {
    hash ^= b;
    hash *= kFnvPrime;
}

} // namespace

std::uint64_t IntentProvenanceBuilder::computeSnapshotHash(
    const analysis::NormalizedAnalysis& snapshot) noexcept {

    std::uint64_t hash = kFnvOffsetBasis;
    fnv1aUpdate(hash, snapshot.id);
    fnv1aUpdate(hash, snapshot.baseRevisionId);

    for (const auto& group : snapshot.choiceGroups) {
        fnv1aUpdate(hash, group.id);
        fnv1aUpdate(hash, group.dimension);
        fnv1aUpdate(hash, group.question);
        fnv1aUpdateByte(hash, group.material ? 1 : 0);

        for (const auto& alt : group.alternatives) {
            fnv1aUpdate(hash, alt.id);
            fnv1aUpdate(hash, alt.interpretation);
            fnv1aUpdate(hash, alt.whyMaterial);
        }
    }

    for (const auto& assumption : snapshot.assumptions) {
        fnv1aUpdate(hash, assumption.id);
        fnv1aUpdate(hash, assumption.statement);
        fnv1aUpdateByte(hash, assumption.material ? 1 : 0);
    }

    for (const auto& conflict : snapshot.conflicts) {
        fnv1aUpdate(hash, conflict.id);
        fnv1aUpdate(hash, conflict.description);
        fnv1aUpdateByte(hash, conflict.blocking ? 1 : 0);
    }

    return hash;
}

ConfirmedIntent IntentProvenanceBuilder::buildConfirmedIntent(
    std::string_view baseRevisionId,
    const analysis::NormalizedAnalysis& sourceSnapshot,
    const std::vector<ConfirmedItem>& explicitItems,
    std::string_view customInstructions) noexcept {

    ConfirmedIntent intent;
    intent.baseRevisionId = std::string(baseRevisionId);
    intent.sourceSnapshotId = sourceSnapshot.id;
    intent.sourceSnapshotHash = computeSnapshotHash(sourceSnapshot);
    intent.items = explicitItems;
    intent.customInstructions = std::string(customInstructions);
    intent.isCurrent = true;

    const auto now = std::chrono::system_clock::now();
    intent.createdAtTimestamp = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());

    std::ostringstream ss;
    ss << "intent_" << std::hex << intent.sourceSnapshotHash << "_" << explicitItems.size();
    intent.id = ss.str();

    return intent;
}

bool IntentProvenanceBuilder::isIntentValid(
    const ConfirmedIntent& intent,
    std::string_view currentRevisionId,
    const analysis::NormalizedAnalysis& currentSnapshot) noexcept {

    if (!intent.isCurrent) {
        return false;
    }

    if (intent.baseRevisionId != currentRevisionId) {
        return false;
    }

    if (intent.sourceSnapshotId != currentSnapshot.id) {
        return false;
    }

    if (intent.sourceSnapshotHash != computeSnapshotHash(currentSnapshot)) {
        return false;
    }

    return true;
}

const char* toString(IntentResolutionKind kind) noexcept {
    switch (kind) {
        case IntentResolutionKind::SelectedAlternative: return "SELECTED_ALTERNATIVE";
        case IntentResolutionKind::CustomText: return "CUSTOM_TEXT";
        case IntentResolutionKind::UseOriginal: return "USE_ORIGINAL";
        case IntentResolutionKind::Withdrawn: return "WITHDRAWN";
    }
    return "UNKNOWN";
}

} // namespace roclint::core::intent
