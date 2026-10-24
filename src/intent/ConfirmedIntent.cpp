#include "roclint/intent/ConfirmedIntent.hpp"

#include "roclint/core/crypto/Sha256.hpp"

#include <algorithm>

namespace roclint::core::intent {

namespace {

void hashList(crypto::CanonicalHasher& h, const std::vector<std::string>& values) {
    h.number(values.size());
    for (const auto& v : values) {
        h.field(v);
    }
}

[[nodiscard]] bool listContains(const std::vector<std::string>& v, const std::string& id) {
    return std::find(v.begin(), v.end(), id) != v.end();
}

} // namespace

bool ConfirmedIntent::hasDecisionFor(std::string_view itemId) const {
    return std::any_of(items.begin(), items.end(), [&](const ConfirmedItem& i) { return i.itemId == itemId; });
}

std::optional<ConfirmedItem> ConfirmedIntent::getDecision(std::string_view itemId) const {
    for (const auto& item : items) {
        if (item.itemId == itemId) {
            return item;
        }
    }
    return std::nullopt;
}

std::string IntentProvenanceBuilder::computeIntentHash(const ConfirmedIntent& intent) {
    crypto::CanonicalHasher h("roclint.confirmed_intent.v1");
    h.field(intent.sessionId).field(intent.baseRevisionId).field(intent.snapshotId).field(intent.snapshotHash);
    hashList(h, intent.visibleItemIds);
    h.number(intent.items.size());
    for (const auto& item : intent.items) {
        h.field(item.itemId)
            .field(toString(item.kind))
            .field(item.chosenAlternativeId)
            .field(item.chosenValue)
            .field(item.customText)
            .field(item.rationale);
    }
    hashList(h, intent.unresolvedItemIds);
    hashList(h, intent.rejectedItemIds);
    return h.finalizeHex();
}

Result<ConfirmedIntent> IntentProvenanceBuilder::buildConfirmedIntent(const analysis::AnalysisSnapshot& finalSnapshot,
                                                                      const analysis::OverallPolicyResult& policy,
                                                                      const WorkingIntentState& state,
                                                                      std::int64_t createdAtMs) {
    if (auto status = analysis::validateAnalysisSnapshot(finalSnapshot); !status.ok()) return status.error();
    const auto& snapshot = finalSnapshot.normalized;

    for (const auto& [groupId, altId] : state.confirmedAlternatives) {
        const auto group = std::find_if(snapshot.choiceGroups.begin(), snapshot.choiceGroups.end(),
                                        [&](const analysis::ChoiceGroup& g) { return g.id == groupId; });
        if (group == snapshot.choiceGroups.end()) {
            return makeError(ErrorCode::InvalidInput, "Confirmed group '" + groupId + "' is not in the snapshot");
        }
        const bool belongs = std::any_of(group->alternatives.begin(), group->alternatives.end(),
                                         [&](const analysis::ChoiceAlternative& a) { return a.id == altId; });
        if (!belongs) {
            return makeError(ErrorCode::InvalidInput,
                             "Alternative '" + altId + "' does not belong to group '" + groupId + "'");
        }
    }
    for (const auto& [itemId, text] : state.customResolutions) {
        if (classifyItem(snapshot, itemId) == ActiveItemKind::None) {
            return makeError(ErrorCode::InvalidInput, "Custom answer targets unknown item '" + itemId + "'");
        }
        if (text.empty()) {
            return makeError(ErrorCode::InvalidInput, "Custom answer for '" + itemId + "' is empty");
        }
    }
    for (const auto& [itemId, accepted] : state.assumptionDecisions) {
        (void)accepted;
        if (classifyItem(snapshot, itemId) != ActiveItemKind::Assumption) {
            return makeError(ErrorCode::InvalidInput, "Assumption decision targets unknown item '" + itemId + "'");
        }
    }
    for (const auto& itemId : state.keptOriginalItemIds) {
        if (classifyItem(snapshot, itemId) != ActiveItemKind::ChoiceGroup) {
            return makeError(ErrorCode::InvalidInput, "Keep-original targets unknown group '" + itemId + "'");
        }
    }

    ConfirmedIntent intent;
    intent.sessionId = snapshot.sessionId;
    intent.baseRevisionId = snapshot.baseRevisionId;
    intent.snapshotId = finalSnapshot.id;
    intent.snapshotHash = finalSnapshot.snapshotHash;
    intent.createdAtMs = createdAtMs;

    for (const auto& id : state.presentedItemIds) {
        if (classifyItem(snapshot, id) != ActiveItemKind::None && !listContains(intent.visibleItemIds, id)) {
            intent.visibleItemIds.push_back(id);
        }
    }

    auto resolveGeneric = [&](const std::string& id, bool material) {
        if (const auto custom = state.customResolutions.find(id); custom != state.customResolutions.end()) {
            intent.items.push_back(ConfirmedItem{id, IntentResolutionKind::CustomText, "", custom->second,
                                                 custom->second, ""});
        } else if (material) {
            intent.unresolvedItemIds.push_back(id);
        }
    };

    for (const auto& group : snapshot.choiceGroups) {
        const auto* verdict = policy.findGroup(group.id);
        const bool material =
            group.material && verdict != nullptr && verdict->verdict != analysis::PolicyVerdict::Rejected;
        if (const auto chosen = state.confirmedAlternatives.find(group.id); chosen != state.confirmedAlternatives.end()) {
            const auto alt = std::find_if(group.alternatives.begin(), group.alternatives.end(),
                                          [&](const analysis::ChoiceAlternative& a) { return a.id == chosen->second; });
            intent.items.push_back(ConfirmedItem{group.id, IntentResolutionKind::SelectedAlternative, alt->id,
                                                 alt->interpretation, "", alt->whyMaterial});
        } else if (listContains(state.keptOriginalItemIds, group.id)) {
            intent.items.push_back(ConfirmedItem{group.id, IntentResolutionKind::UseOriginal, "",
                                                 "Keep the original wording", "", ""});
        } else {
            resolveGeneric(group.id, material);
        }
    }

    for (const auto& md : snapshot.missingDecisions) {
        resolveGeneric(md.id, md.blocking);
    }

    for (const auto idx : analysis::orderedAssumptionIndices(snapshot)) {
        const auto& a = snapshot.assumptions[idx];
        if (const auto decision = state.assumptionDecisions.find(a.id); decision != state.assumptionDecisions.end()) {
            intent.items.push_back(ConfirmedItem{a.id,
                                                 decision->second ? IntentResolutionKind::AcceptedAssumption
                                                                  : IntentResolutionKind::DeclinedAssumption,
                                                 "", a.statement, "", ""});
            if (!decision->second) {
                intent.rejectedItemIds.push_back(a.id);
            }
        } else {
            resolveGeneric(a.id, a.material);
        }
    }

    for (const auto& c : snapshot.conflicts) {
        resolveGeneric(c.id, c.blocking);
    }

    intent.intentHash = computeIntentHash(intent);
    intent.id = "intent_" + intent.intentHash;
    return intent;
}

Status IntentProvenanceBuilder::isIntentValid(const ConfirmedIntent& intent,
                                              std::string_view currentRevisionId,
                                              RevisionStatus revisionStatus,
                                              const analysis::AnalysisSnapshot& currentSnapshot) {
    const std::string recomputed = computeIntentHash(intent);
    if (intent.intentHash != recomputed || intent.id != "intent_" + recomputed) {
        return makeError(ErrorCode::ValidationFailed, "Intent hash does not match its contents");
    }
    if (revisionStatus == RevisionStatus::DraftDirty) {
        return makeError(ErrorCode::StaleVersion, "The prompt has unsaved edits since the intent was confirmed");
    }
    if (intent.baseRevisionId != currentRevisionId) {
        return makeError(ErrorCode::StaleVersion, "Intent belongs to a different revision");
    }
    if (currentSnapshot.normalized.baseRevisionId != currentRevisionId) {
        return makeError(ErrorCode::StaleVersion, "Analysis belongs to a different revision");
    }
    if (!analysis::validateAnalysisSnapshot(currentSnapshot).ok()) {
        return makeError(ErrorCode::ValidationFailed, "Analysis snapshot hash does not match its contents");
    }
    if (intent.snapshotId != currentSnapshot.id || intent.snapshotHash != currentSnapshot.snapshotHash) {
        return makeError(ErrorCode::StaleVersion, "Intent belongs to a different analysis snapshot");
    }
    if (intent.sessionId != currentSnapshot.normalized.sessionId) {
        return makeError(ErrorCode::ValidationFailed, "Intent belongs to a different session");
    }
    for (const auto& c : currentSnapshot.normalized.conflicts) {
        if (c.blocking && !intent.hasDecisionFor(c.id)) {
            return makeError(ErrorCode::PreconditionFailed, "Blocking conflict '" + c.id + "' is unresolved");
        }
    }
    for (const auto& md : currentSnapshot.normalized.missingDecisions) {
        if (md.blocking && !intent.hasDecisionFor(md.id)) {
            return makeError(ErrorCode::PreconditionFailed, "Blocking decision '" + md.id + "' is unresolved");
        }
    }
    return Status::success();
}

const char* toString(IntentResolutionKind kind) noexcept {
    switch (kind) {
        case IntentResolutionKind::SelectedAlternative: return "SELECTED_ALTERNATIVE";
        case IntentResolutionKind::CustomText: return "CUSTOM_TEXT";
        case IntentResolutionKind::UseOriginal: return "USE_ORIGINAL";
        case IntentResolutionKind::AcceptedAssumption: return "ACCEPTED_ASSUMPTION";
        case IntentResolutionKind::DeclinedAssumption: return "DECLINED_ASSUMPTION";
    }
    return "UNKNOWN";
}

} // namespace roclint::core::intent
