#include "roclint/analysis/NormalizedAnalysis.hpp"

#include "roclint/core/crypto/Sha256.hpp"

#include <algorithm>
#include <limits>
#include <unordered_set>

namespace roclint::core::analysis {

std::string mintGroupId(std::size_t groupIndex) {
    return "grp_" + std::to_string(groupIndex);
}

std::string mintAlternativeId(std::size_t groupIndex, std::size_t altIndex) {
    return "alt_" + std::to_string(groupIndex) + "_" + std::to_string(altIndex);
}

std::string mintAssumptionId(std::size_t index) {
    return "asm_" + std::to_string(index);
}

std::string mintMissingDecisionId(std::size_t index) {
    return "mdc_" + std::to_string(index);
}

std::string mintConflictId(std::size_t index) {
    return "cfl_" + std::to_string(index);
}

namespace {

void hashSpans(crypto::CanonicalHasher& h, const std::vector<SourceSpan>& spans) {
    h.number(spans.size());
    for (const auto& span : spans) {
        h.number(span.startByte).number(span.endByte);
    }
}

} // namespace

std::string computeSnapshotHash(const NormalizedAnalysis& analysis) {
    crypto::CanonicalHasher h("roclint.analysis_snapshot.v1");
    h.field(analysis.sessionId).field(analysis.baseRevisionId);

    h.number(analysis.choiceGroups.size());
    for (const auto& group : analysis.choiceGroups) {
        h.field(group.id).field(group.dimension).field(group.question).flag(group.material);
        h.number(group.alternatives.size());
        for (const auto& alt : group.alternatives) {
            h.field(alt.id).field(alt.interpretation).field(alt.whyMaterial).field(alt.canonicalSemanticsJson);
            hashSpans(h, alt.evidence);
        }
    }

    h.number(analysis.assumptions.size());
    for (const auto& assumption : analysis.assumptions) {
        h.field(assumption.id).field(assumption.statement).flag(assumption.material);
        hashSpans(h, assumption.evidence);
    }

    h.number(analysis.missingDecisions.size());
    for (const auto& md : analysis.missingDecisions) {
        h.field(md.id).field(md.question).flag(md.blocking);
        // Empty field values are omitted so already persisted Decision Contract v2
        // snapshots keep their original canonical hash. A non-empty suggestion is
        // still bound into the snapshot and changes its hash.
        if (!md.suggestedAlternativeEvidence.empty())
            h.field("suggested_alternative_evidence").field(md.suggestedAlternativeEvidence);
    }

    h.number(analysis.conflicts.size());
    for (const auto& conflict : analysis.conflicts) {
        h.field(conflict.id).field(conflict.description).flag(conflict.blocking);
    }

    return h.finalizeHex();
}

void sealSnapshot(NormalizedAnalysis& analysis) {
    analysis.snapshotHash = computeSnapshotHash(analysis);
    analysis.id = "snap_" + analysis.snapshotHash;
}

bool isSnapshotSealed(const NormalizedAnalysis& analysis) {
    const std::string expected = computeSnapshotHash(analysis);
    return analysis.snapshotHash == expected && analysis.id == "snap_" + expected;
}

std::vector<std::size_t> orderedAssumptionIndices(const NormalizedAnalysis& analysis) {
    std::vector<std::size_t> indices(analysis.assumptions.size());
    for (std::size_t i = 0; i < indices.size(); ++i) {
        indices[i] = i;
    }
    auto anchor = [&](std::size_t idx) {
        std::size_t best = std::numeric_limits<std::size_t>::max();
        for (const auto& span : analysis.assumptions[idx].evidence) {
            best = std::min(best, span.startByte);
        }
        return best;
    };
    std::stable_sort(indices.begin(), indices.end(), [&](std::size_t a, std::size_t b) {
        const auto anchorA = anchor(a);
        const auto anchorB = anchor(b);
        if (anchorA != anchorB) {
            return anchorA < anchorB;
        }
        return analysis.assumptions[a].id < analysis.assumptions[b].id;
    });
    return indices;
}

AnalysisValidationResult validateNormalizedAnalysis(const NormalizedAnalysis& analysis, std::string_view promptText) {
    AnalysisValidationResult result{};

    auto markInvalid = [&](std::string issue) {
        result.status = AnalysisValidationStatus::Invalid;
        result.issues.push_back(std::move(issue));
    };
    auto markManual = [&](std::string issue) {
        if (result.status != AnalysisValidationStatus::Invalid) {
            result.status = AnalysisValidationStatus::RequiresManualReview;
        }
        result.issues.push_back(std::move(issue));
    };

    if (analysis.baseRevisionId.empty()) {
        markInvalid("Base revision ID is missing or empty.");
    }

    std::unordered_set<std::string> seenIds;
    auto checkId = [&](const std::string& id, const std::string& what) {
        if (id.empty()) {
            markInvalid(what + " has an empty ID.");
        } else if (!seenIds.insert(id).second) {
            markInvalid("Duplicate item ID '" + id + "' (" + what + ").");
        }
    };
    auto checkSpans = [&](const std::vector<SourceSpan>& spans, const std::string& owner) {
        for (const auto& span : spans) {
            if (span.empty() || !isValidSpan(promptText, span)) {
                markInvalid(owner + " contains an empty, invalid, or out-of-bounds SourceSpan [" +
                            std::to_string(span.startByte) + ", " + std::to_string(span.endByte) + ")");
            }
        }
    };

    for (std::size_t i = 0; i < analysis.choiceGroups.size(); ++i) {
        const auto& group = analysis.choiceGroups[i];
        checkId(group.id, "ChoiceGroup at index " + std::to_string(i));

        if (group.alternatives.size() < ChoiceGroup::kMinAlternatives ||
            group.alternatives.size() > ChoiceGroup::kMaxAlternatives) {
            markManual("ChoiceGroup '" + group.id + "' violates cardinality: must have 2 or 3 alternatives, found " +
                       std::to_string(group.alternatives.size()));
        }

        std::unordered_set<std::string> seenInterpretations;
        for (std::size_t j = 0; j < group.alternatives.size(); ++j) {
            const auto& alt = group.alternatives[j];
            checkId(alt.id, "ChoiceAlternative " + std::to_string(j) + " in group '" + group.id + "'");
            if (alt.interpretation.empty()) {
                markManual("ChoiceAlternative at index " + std::to_string(j) + " in group '" + group.id +
                           "' has an empty interpretation.");
            } else if (!seenInterpretations.insert(alt.interpretation).second) {
                markManual("ChoiceGroup '" + group.id + "' contains duplicate alternative interpretation: '" +
                           alt.interpretation + "'");
            }
            checkSpans(alt.evidence, "ChoiceAlternative '" + alt.id + "'");
        }
    }

    for (const auto& assumption : analysis.assumptions) {
        checkId(assumption.id, "Assumption");
        if (assumption.statement.empty()) {
            markInvalid("Assumption '" + assumption.id + "' has an empty statement.");
        }
        checkSpans(assumption.evidence, "Assumption '" + assumption.id + "'");
    }

    for (const auto& md : analysis.missingDecisions) {
        checkId(md.id, "MissingDecision");
        if (md.question.empty()) {
            markInvalid("MissingDecision '" + md.id + "' has an empty question.");
        }
    }

    for (const auto& conflict : analysis.conflicts) {
        checkId(conflict.id, "Conflict");
        if (conflict.description.empty()) {
            markInvalid("Conflict '" + conflict.id + "' has an empty description.");
        }
    }

    return result;
}

} // namespace roclint::core::analysis
