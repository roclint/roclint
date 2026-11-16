#pragma once

// Revision-bound intent brief shared by the word companion, adaptive interview
// and comprehension review.
//
// The brief is a derived projection, never an authority. Raw user answers and
// verbatim source excerpts remain the only authoritative wording; the display
// summary may never be fed back as truth. Every item records the analysis item
// identity and the revision/snapshot it belongs to so a stale brief cannot be
// mistaken for the current understanding.
//
// This header is intentionally header-only: the coordinator owns CMake source
// registration for this mission, and a self-contained value builder keeps the
// contract testable in the existing core test target.

#include "roclint/analysis/AnalysisSnapshot.hpp"
#include "roclint/analysis/MaterialityPolicy.hpp"
#include "roclint/core/crypto/Sha256.hpp"
#include "roclint/core/scoped_constraint.hpp"
#include "roclint/intent/ConfirmedIntent.hpp"
#include "roclint/intent/InterventionStateMachine.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace roclint::core::intent {

enum class BriefItemKind : std::uint8_t {
    SourceRequirement,
    ConfirmedDecision,
    Preference,
    Prohibition,
    Evidence,
    ProposedAssumption,
    OpenChoice,
    SupersededDecision
};

enum class BriefItemStatus : std::uint8_t {
    Confirmed,
    Proposed,
    Open,
    Superseded
};

[[nodiscard]] constexpr const char* toString(BriefItemKind kind) noexcept {
    switch (kind) {
        case BriefItemKind::SourceRequirement: return "source_requirement";
        case BriefItemKind::ConfirmedDecision: return "confirmed_decision";
        case BriefItemKind::Preference: return "preference";
        case BriefItemKind::Prohibition: return "prohibition";
        case BriefItemKind::Evidence: return "evidence";
        case BriefItemKind::ProposedAssumption: return "proposed_assumption";
        case BriefItemKind::OpenChoice: return "open_choice";
        case BriefItemKind::SupersededDecision: return "superseded_decision";
    }
    return "source_requirement";
}

[[nodiscard]] constexpr const char* toString(BriefItemStatus status) noexcept {
    switch (status) {
        case BriefItemStatus::Confirmed: return "confirmed";
        case BriefItemStatus::Proposed: return "proposed";
        case BriefItemStatus::Open: return "open";
        case BriefItemStatus::Superseded: return "superseded";
    }
    return "open";
}

struct BriefItem {
    std::string id{};
    BriefItemKind kind{BriefItemKind::SourceRequirement};
    BriefItemStatus status{BriefItemStatus::Open};
    std::string text{};          // display-only wording; not authority
    std::string sourceExcerpt{}; // verbatim source text, empty when none
    std::string sourceRef{};     // revision span, evidence reference or item identity
    std::string rawAnswer{};     // verbatim user answer, empty for a menu selection
    bool consequential{false};
    std::string analysisItemId{}; // host-minted analysis/follow-up item identity

    bool operator==(const BriefItem&) const = default;
};

// One explicit interview choice. Labels/consequences are model-proposed wording;
// selecting a choice is an explicit user decision, not a confirmation by the model.
struct InterviewChoice {
    std::string id{};
    std::string label{};
    std::string consequence{};

    bool operator==(const InterviewChoice&) const = default;
};

// A question actually shown in the substantial interview. `analysisItemId` is set
// only when the question came from the sealed analysis snapshot; follow-up and
// review questions are interview-owned and are merged into ConfirmedIntent through
// explicit supplemental decisions. Raw answers stay verbatim for provenance.
struct InterviewRecord {
    std::string id{};
    std::string analysisItemId{};
    std::string question{};
    std::string consequence{};
    std::vector<InterviewChoice> choices{};
    std::string sourceExcerpt{};
    std::string sourceRef{};
    std::string origin{}; // "analysis" | "model_followup" | "review_finding"
    std::string disposition{"asked"}; // asked|answer|custom|unknown|skip|rejected
    std::string rawAnswer{};
    std::string selectedChoiceId{};
    std::vector<std::string> previousAnswers{};
    bool asked{true};

    [[nodiscard]] bool isDecision() const noexcept {
        return disposition == "answer" || disposition == "custom";
    }
    [[nodiscard]] bool isOpen() const noexcept {
        return !isDecision() || disposition == "rejected";
    }

    bool operator==(const InterviewRecord&) const = default;
};

struct BriefEvidenceRef {
    std::string ref{};
    std::string excerpt{};
    std::string detail{};

    bool operator==(const BriefEvidenceRef&) const = default;
};

// A host-supplied synonym read from the source text by deterministic clause
// splitting. This is not a model inference; it is verbatim text with offsets.
struct SourceClause {
    std::string excerpt{};
    std::size_t startByte{0};
    std::size_t endByte{0};
    bool prohibition{false};
    bool preference{false};

    bool operator==(const SourceClause&) const = default;
};

struct IntentBriefInputs {
    std::string sessionId{};
    std::string revisionId{};
    std::string revisionText{};
    std::string workspaceIdentity{};
    std::uint64_t workspaceEpoch{0};
    std::uint64_t stateVersion{0};
    std::string snapshotId{};
    std::string snapshotHash{};
    const analysis::NormalizedAnalysis* analysis{nullptr};
    const analysis::OverallPolicyResult* policy{nullptr};
    const ConfirmedIntent* confirmedIntent{nullptr};
    const WorkingIntentState* workingIntent{nullptr};
    std::vector<core::ScopedConstraint> sourceConstraints{};
    std::vector<BriefEvidenceRef> evidence{};
    std::vector<InterviewRecord> interviewRecords{};
};

struct IntentBrief {
    std::string sessionId{};
    std::string revisionId{};
    std::string revisionTextHash{};
    std::string workspaceIdentity{};
    std::uint64_t workspaceEpoch{0};
    std::uint64_t stateVersion{0};
    std::string snapshotId{};
    std::string snapshotHash{};
    std::vector<BriefItem> items{};
    std::string displaySummary{}; // display-only; never authority

    [[nodiscard]] bool empty() const noexcept { return items.empty(); }

    [[nodiscard]] const BriefItem* find(std::string_view id) const noexcept {
        for (const auto& item : items) {
            if (item.id == id) return &item;
        }
        return nullptr;
    }

    [[nodiscard]] bool hasKind(BriefItemKind kind) const noexcept {
        return std::any_of(items.begin(), items.end(), [kind](const BriefItem& item) { return item.kind == kind; });
    }

    [[nodiscard]] std::vector<std::string> openChoiceIds() const {
        std::vector<std::string> ids;
        for (const auto& item : items) {
            if (item.kind == BriefItemKind::OpenChoice && item.status == BriefItemStatus::Open) {
                ids.push_back(item.id);
            }
        }
        return ids;
    }

    [[nodiscard]] std::vector<const BriefItem*> confirmedDecisions() const {
        std::vector<const BriefItem*> result;
        for (const auto& item : items) {
            if ((item.kind == BriefItemKind::ConfirmedDecision || item.kind == BriefItemKind::Preference) &&
                item.status == BriefItemStatus::Confirmed) {
                result.push_back(&item);
            }
        }
        return result;
    }

    bool operator==(const IntentBrief&) const = default;
};

namespace detail {

[[nodiscard]] inline std::string lowerAscii(std::string_view text) {
    std::string lower;
    lower.reserve(text.size());
    for (const char c : text) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return lower;
}

[[nodiscard]] inline bool hasMarker(std::string_view lower,
                                    std::initializer_list<std::string_view> markers) {
    return std::any_of(markers.begin(), markers.end(),
                       [&](std::string_view marker) { return lower.find(marker) != std::string_view::npos; });
}

[[nodiscard]] inline std::string bounded(std::string_view text, std::size_t maximum) {
    if (text.size() <= maximum) return std::string(text);
    std::size_t end = maximum;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) --end;
    return std::string(text.substr(0, end));
}

[[nodiscard]] inline std::string resolutionLabel(IntentResolutionKind kind) {
    switch (kind) {
        case IntentResolutionKind::SelectedAlternative: return "Selected";
        case IntentResolutionKind::CustomText: return "Answered";
        case IntentResolutionKind::UseOriginal: return "Kept original wording";
        case IntentResolutionKind::AcceptedAssumption: return "Accepted assumption";
        case IntentResolutionKind::DeclinedAssumption: return "Rejected assumption";
    }
    return "Decision";
}

[[nodiscard]] inline bool isPreferenceWording(std::string_view raw) {
    const auto lower = lowerAscii(raw);
    static constexpr std::string_view kMarkers[] = {
        "prefer", "i'd like", "i would like", "rather", "i want", "i love", "i like", "favor", "favour"
    };
    return std::any_of(std::begin(kMarkers), std::end(kMarkers), [&](std::string_view marker) {
        return lower.find(marker) != std::string_view::npos;
    });
}

[[nodiscard]] inline bool hasSourceSpan(const analysis::SourceSpan& span, std::string_view sourceText) {
    return span.startByte < span.endByte && span.endByte <= sourceText.size() &&
           analysis::isValidSpan(sourceText, span);
}

} // namespace detail

// Deterministic clause split over the frozen revision. It only describes the
// user's own text: no model inference, no paraphrase.
[[nodiscard]] inline std::vector<SourceClause> extractSourceClauses(std::string_view text,
                                                                   std::size_t maximumClauses = 16) {
    std::vector<SourceClause> clauses;
    std::size_t begin = 0;
    const auto pushClause = [&](std::size_t rawStart, std::size_t rawEnd) {
        if (clauses.size() >= maximumClauses) return;
        while (rawStart < rawEnd && (text[rawStart] == ' ' || text[rawStart] == '\t')) ++rawStart;
        while (rawEnd > rawStart && (text[rawEnd - 1] == ' ' || text[rawEnd - 1] == '\t')) --rawEnd;
        if (rawEnd <= rawStart || rawEnd - rawStart < 3) return;
        if (rawEnd - rawStart > 512) rawEnd = rawStart + 512;
        const std::string excerpt(text.substr(rawStart, rawEnd - rawStart));
        const auto lower = detail::lowerAscii(excerpt);
        const bool prohibition = detail::hasMarker(lower, {
            "do not", "don't", "dont", "never", "without", "must not", "should not",
            "avoid", "no ", "not ", "leave untouched", "keeping unchanged", "keep intact"
        });
        const bool preference = !prohibition && detail::isPreferenceWording(lower);
        clauses.push_back(SourceClause{excerpt, rawStart, rawEnd, prohibition, preference});
    };

    for (std::size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch == '.' || ch == ';' || ch == '\n' || ch == '!' || ch == '?') {
            pushClause(begin, i);
            begin = i + 1;
        } else if (ch == ',' && i + 4 < text.size() && text.substr(i + 1, 4) == " but") {
            pushClause(begin, i);
            begin = i + 5;
            i += 4;
        }
    }
    pushClause(begin, text.size());
    return clauses;
}

struct IntentBriefBuilder {
    [[nodiscard]] static IntentBrief build(const IntentBriefInputs& inputs);
};

[[nodiscard]] inline IntentBrief IntentBriefBuilder::build(const IntentBriefInputs& inputs) {
    IntentBrief brief;
    brief.sessionId = inputs.sessionId;
    brief.revisionId = inputs.revisionId;
    brief.workspaceIdentity = inputs.workspaceIdentity;
    brief.workspaceEpoch = inputs.workspaceEpoch;
    brief.stateVersion = inputs.stateVersion;
    brief.snapshotId = inputs.snapshotId;
    brief.snapshotHash = inputs.snapshotHash;
    brief.revisionTextHash = crypto::Sha256::hexDigest(inputs.revisionText);

    std::size_t minted = 0;
    const auto nextId = [&](std::string_view prefix) {
        return std::string(prefix) + std::to_string(++minted);
    };

    // 1) Verbatim source clauses. Requirements and prohibitions are source truth;
    //    explicit preference wording is labelled as such rather than inferred.
    for (const auto& clause : extractSourceClauses(inputs.revisionText)) {
        BriefItem item;
        item.id = nextId("src_");
        item.kind = clause.prohibition ? BriefItemKind::Prohibition
                   : clause.preference  ? BriefItemKind::Preference
                                        : BriefItemKind::SourceRequirement;
        item.status = BriefItemStatus::Confirmed;
        item.text = detail::bounded(clause.excerpt, 320);
        item.sourceExcerpt = clause.excerpt;
        item.sourceRef = "revision:" + inputs.revisionId + "#bytes:" + std::to_string(clause.startByte) +
                         "-" + std::to_string(clause.endByte);
        item.consequential = true;
        brief.items.push_back(std::move(item));
    }

    // 2) Source-linked constraints supplied by the host (user prohibitions and
    //    accepted domain evidence). Constraints without a resolvable excerpt are
    //    still shown, but never claim a fabricated quotation.
    for (const auto& constraint : inputs.sourceConstraints) {
        if (constraint.rule.empty()) continue;
        BriefItem item;
        item.id = nextId("constraint_");
        item.kind = constraint.polarity == core::Polarity::Prohibit ? BriefItemKind::Prohibition
                                                                   : BriefItemKind::SourceRequirement;
        item.status = BriefItemStatus::Confirmed;
        item.text = detail::bounded(constraint.rule, 320);
        item.sourceRef = constraint.id;
        if (!constraint.evidence.empty()) {
            const auto& span = constraint.evidence.front();
            // core::ScopedConstraint carries byte offsets into the same frozen buffer.
            if (span.beginByte < span.endByte && span.endByte <= inputs.revisionText.size()) {
                item.sourceExcerpt = std::string(inputs.revisionText.substr(span.beginByte,
                                                                            span.endByte - span.beginByte));
            }
        }
        item.consequential = true;
        brief.items.push_back(std::move(item));
    }

    // 3) Explicit user decisions and open analysis choices. Every branch is driven
    //    by WorkingIntentState (explicit actions) or the sealed snapshot; model
    //    inference alone can only produce a proposed assumption or an open choice.
    const auto& analysis = inputs.analysis;
    const WorkingIntentState emptyState{};
    const auto& state = inputs.workingIntent != nullptr ? *inputs.workingIntent : emptyState;
    const auto policyVerdict = [&](std::string_view itemId) {
        if (inputs.policy == nullptr) return analysis::PolicyVerdict::EligibleForChoice;
        const auto* group = inputs.policy->findGroup(std::string(itemId));
        return group != nullptr ? group->verdict : analysis::PolicyVerdict::EligibleForChoice;
    };

    if (analysis != nullptr) {
        for (const auto& group : analysis->choiceGroups) {
            const auto chosen = state.confirmedAlternatives.find(group.id);
            const auto custom = state.customResolutions.find(group.id);
            const bool keptOriginal =
                std::find(state.keptOriginalItemIds.begin(), state.keptOriginalItemIds.end(), group.id) !=
                state.keptOriginalItemIds.end();
            const bool material = group.material && policyVerdict(group.id) != analysis::PolicyVerdict::Rejected;

            BriefItem item;
            item.id = group.id;
            item.analysisItemId = group.id;
            item.consequential = material;
            if (chosen != state.confirmedAlternatives.end()) {
                const auto alt = std::find_if(group.alternatives.begin(), group.alternatives.end(),
                                              [&](const analysis::ChoiceAlternative& candidate) {
                                                  return candidate.id == chosen->second;
                                              });
                item.kind = BriefItemKind::ConfirmedDecision;
                item.status = BriefItemStatus::Confirmed;
                item.text = alt != group.alternatives.end() ? alt->interpretation : chosen->second;
                item.sourceRef = group.id;
                item.sourceExcerpt = alt != group.alternatives.end() && !alt->evidence.empty() &&
                                             detail::hasSourceSpan(alt->evidence.front(), inputs.revisionText)
                                         ? std::string(inputs.revisionText.substr(alt->evidence.front().startByte,
                                                                                  alt->evidence.front().length()))
                                         : std::string();
            } else if (custom != state.customResolutions.end()) {
                item.kind = detail::isPreferenceWording(custom->second) ? BriefItemKind::Preference
                                                                       : BriefItemKind::ConfirmedDecision;
                item.status = BriefItemStatus::Confirmed;
                item.text = detail::bounded(custom->second, 320);
                item.rawAnswer = custom->second;
                item.sourceRef = group.id;
            } else if (keptOriginal) {
                item.kind = BriefItemKind::ConfirmedDecision;
                item.status = BriefItemStatus::Confirmed;
                item.text = "Keep the original wording for: " + group.question;
                item.sourceRef = group.id;
            } else {
                item.kind = BriefItemKind::OpenChoice;
                item.status = BriefItemStatus::Open;
                item.text = group.question;
                item.sourceRef = group.id;
                const bool unknown = state.hasUncertainty(group.id);
                if (const auto note = state.uncertaintyNotes.find(group.id); note != state.uncertaintyNotes.end()) {
                    item.rawAnswer = note->second;
                }
                if (unknown) item.text += " (marked unknown; still open)";
                else if (state.isSkipped(group.id)) item.text += " (skipped; still open)";
            }
            brief.items.push_back(std::move(item));
        }

        for (const auto& assumption : analysis->assumptions) {
            const auto decision = state.assumptionDecisions.find(assumption.id);
            const auto custom = state.customResolutions.find(assumption.id);
            BriefItem item;
            item.id = assumption.id;
            item.analysisItemId = assumption.id;
            item.consequential = assumption.material;
            item.sourceRef = assumption.id;
            if (!assumption.evidence.empty() &&
                detail::hasSourceSpan(assumption.evidence.front(), inputs.revisionText)) {
                item.sourceExcerpt = std::string(inputs.revisionText.substr(assumption.evidence.front().startByte,
                                                                           assumption.evidence.front().length()));
            }
            if (decision != state.assumptionDecisions.end()) {
                item.kind = decision->second ? BriefItemKind::ConfirmedDecision : BriefItemKind::Prohibition;
                item.status = BriefItemStatus::Confirmed;
                item.text = (decision->second ? "Accepted: " : "Do not assume: ") + assumption.statement;
            } else if (custom != state.customResolutions.end()) {
                item.kind = detail::isPreferenceWording(custom->second) ? BriefItemKind::Preference
                                                                       : BriefItemKind::ConfirmedDecision;
                item.status = BriefItemStatus::Confirmed;
                item.text = detail::bounded(custom->second, 320);
                item.rawAnswer = custom->second;
            } else {
                item.kind = BriefItemKind::ProposedAssumption;
                item.status = BriefItemStatus::Proposed;
                item.text = assumption.statement;
                if (state.hasUncertainty(assumption.id)) {
                    item.kind = BriefItemKind::OpenChoice;
                    item.status = BriefItemStatus::Open;
                    item.text += " (marked unknown; still open)";
                    if (const auto note = state.uncertaintyNotes.find(assumption.id);
                        note != state.uncertaintyNotes.end()) {
                        item.rawAnswer = note->second;
                    }
                } else if (state.isSkipped(assumption.id)) {
                    item.kind = BriefItemKind::OpenChoice;
                    item.status = BriefItemStatus::Open;
                    item.text += " (skipped; still open)";
                }
            }
            brief.items.push_back(std::move(item));
        }

        for (const auto& missing : analysis->missingDecisions) {
            const auto custom = state.customResolutions.find(missing.id);
            BriefItem item;
            item.id = missing.id;
            item.analysisItemId = missing.id;
            item.consequential = missing.blocking;
            item.sourceRef = missing.id;
            if (custom != state.customResolutions.end()) {
                item.kind = detail::isPreferenceWording(custom->second) ? BriefItemKind::Preference
                                                                       : BriefItemKind::ConfirmedDecision;
                item.status = BriefItemStatus::Confirmed;
                item.text = detail::bounded(custom->second, 320);
                item.rawAnswer = custom->second;
            } else {
                item.kind = BriefItemKind::OpenChoice;
                item.status = BriefItemStatus::Open;
                item.text = missing.question;
                if (state.hasUncertainty(missing.id)) item.text += " (marked unknown; still open)";
                else if (state.isSkipped(missing.id)) item.text += " (skipped; still open)";
                if (const auto note = state.uncertaintyNotes.find(missing.id); note != state.uncertaintyNotes.end()) {
                    item.rawAnswer = note->second;
                }
            }
            brief.items.push_back(std::move(item));
        }

        for (const auto& conflict : analysis->conflicts) {
            const auto custom = state.customResolutions.find(conflict.id);
            BriefItem item;
            item.id = conflict.id;
            item.analysisItemId = conflict.id;
            item.consequential = conflict.blocking;
            item.sourceRef = conflict.id;
            if (custom != state.customResolutions.end()) {
                item.kind = BriefItemKind::ConfirmedDecision;
                item.status = BriefItemStatus::Confirmed;
                item.text = detail::bounded(custom->second, 320);
                item.rawAnswer = custom->second;
            } else {
                item.kind = BriefItemKind::OpenChoice;
                item.status = BriefItemStatus::Open;
                item.text = conflict.description;
                if (state.hasUncertainty(conflict.id)) item.text += " (marked unknown; still open)";
                else if (state.isSkipped(conflict.id)) item.text += " (skipped; still open)";
                if (const auto note = state.uncertaintyNotes.find(conflict.id); note != state.uncertaintyNotes.end()) {
                    item.rawAnswer = note->second;
                }
            }
            brief.items.push_back(std::move(item));
        }
    }

    // 4) Interview-owned follow-ups and review questions. Base analysis records are
    //    already represented above and are not duplicated.
    for (const auto& record : inputs.interviewRecords) {
        if (!record.analysisItemId.empty()) continue;
        if (!record.asked) continue;
        BriefItem item;
        item.id = record.id;
        item.sourceExcerpt = record.sourceExcerpt;
        item.sourceRef = record.sourceRef.empty() ? record.id : record.sourceRef;
        item.rawAnswer = record.rawAnswer;
        item.consequential = true;
        if (record.isDecision()) {
            item.kind = detail::isPreferenceWording(record.rawAnswer) ? BriefItemKind::Preference
                                                                     : BriefItemKind::ConfirmedDecision;
            item.status = BriefItemStatus::Confirmed;
            item.text = record.selectedChoiceId.empty()
                            ? detail::bounded(record.rawAnswer, 320)
                            : [&]() {
                                  for (const auto& choice : record.choices) {
                                      if (choice.id == record.selectedChoiceId) return choice.label;
                                  }
                                  return detail::bounded(record.rawAnswer, 320);
                              }();
            if (item.text.empty()) item.text = detail::bounded(record.rawAnswer, 320);
        } else {
            item.kind = BriefItemKind::OpenChoice;
            item.status = BriefItemStatus::Open;
            item.text = record.question;
            if (record.disposition == "unknown") item.text += " (marked unknown; still open)";
            else if (record.disposition == "skip") item.text += " (skipped; still open)";
            else if (record.disposition == "rejected") item.text += " (answer not accepted)";
        }
        brief.items.push_back(std::move(item));

        for (const auto& previous : record.previousAnswers) {
            BriefItem superseded;
            superseded.id = record.id + "_superseded_" + std::to_string(brief.items.size());
            superseded.kind = BriefItemKind::SupersededDecision;
            superseded.status = BriefItemStatus::Superseded;
            superseded.text = "Previously: " + detail::bounded(previous, 240);
            superseded.rawAnswer = previous;
            superseded.sourceRef = record.id;
            superseded.analysisItemId = record.id;
            superseded.consequential = false;
            brief.items.push_back(std::move(superseded));
        }
    }

    // 5) Superseded analysis decisions retained for provenance.
    for (const auto& superseded : state.supersededDecisions) {
        BriefItem item;
        item.id = "sup_" + superseded.itemId + "_" + std::to_string(brief.items.size());
        item.kind = BriefItemKind::SupersededDecision;
        item.status = BriefItemStatus::Superseded;
        item.text = "Previously: " + detail::bounded(superseded.previousValue, 240);
        item.rawAnswer = superseded.previousValue;
        item.sourceRef = superseded.itemId;
        item.analysisItemId = superseded.itemId;
        item.consequential = false;
        brief.items.push_back(std::move(item));
    }

    // 6) Grounded workspace evidence. Display metadata only; the reference identity
    //    is what a caller may cite.
    for (const auto& evidence : inputs.evidence) {
        if (evidence.ref.empty()) continue;
        BriefItem item;
        item.id = nextId("evidence_");
        item.kind = BriefItemKind::Evidence;
        item.status = BriefItemStatus::Confirmed;
        item.text = detail::bounded(evidence.detail.empty() ? evidence.ref : evidence.detail, 320);
        item.sourceExcerpt = detail::bounded(evidence.excerpt, 320);
        item.sourceRef = evidence.ref;
        item.consequential = false;
        brief.items.push_back(std::move(item));
    }

    // 7) Display-only summary. Counts are derived; no generated wording is fed back.
    const auto count = [&](BriefItemKind kind) {
        return std::count_if(brief.items.begin(), brief.items.end(),
                             [kind](const BriefItem& item) { return item.kind == kind; });
    };
    const auto confirmedCount = static_cast<std::size_t>(count(BriefItemKind::ConfirmedDecision)) +
                                static_cast<std::size_t>(count(BriefItemKind::Preference));
    const auto openCount = static_cast<std::size_t>(count(BriefItemKind::OpenChoice));
    const auto requirementCount = static_cast<std::size_t>(count(BriefItemKind::SourceRequirement));
    const auto prohibitionCount = static_cast<std::size_t>(count(BriefItemKind::Prohibition));
    brief.displaySummary = std::to_string(confirmedCount) + " confirmed decision(s), " +
                           std::to_string(openCount) + " open choice(s), " +
                           std::to_string(requirementCount) + " source requirement(s), " +
                           std::to_string(prohibitionCount) + " prohibition(s).";
    return brief;
}

} // namespace roclint::core::intent
