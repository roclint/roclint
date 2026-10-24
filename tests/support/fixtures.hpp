#pragma once

#include "roclint/analysis/MaterialityPolicy.hpp"
#include "roclint/analysis/NormalizedAnalysis.hpp"
#include "roclint/bridge/GraniteAdapter.hpp"
#include "roclint/bridge/GraniteRewriteAdapter.hpp"
#include "roclint/bridge/S1TriageAdapter.hpp"
#include "roclint/core/Json.hpp"
#include "roclint/review/ApprovalRecord.hpp"
#include "roclint/rewrite/RewriteProfile.hpp"
#include "roclint/rewrite/RewriteProposal.hpp"
#include "roclint/rewrite/RewriteValidator.hpp"
#include "roclint/core/prompt_state/PromptRevision.hpp"
#include "roclint/core/prompt_state/RevisionState.hpp"
#include "roclint/intent/ConfirmedIntent.hpp"
#include "roclint/intent/InterventionStateMachine.hpp"
#include "support/check.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fx {

namespace an = roclint::core::analysis;

// Build the post-scorer artifact used by intent/proposal/approval fixtures.
inline an::AnalysisSnapshot finalSnapshot(const an::NormalizedAnalysis& normalized, std::string_view text,
                                          std::optional<roclint::bridge::SnapshotEvaluationResponse> evaluation = std::nullopt) {
    auto scored = evaluation ? *evaluation : roclint::bridge::S1TriageAdapter::degradedResponse(normalized, "test: scorer not configured");
    const bool ranked = scored.isRanked();
    auto result = an::buildAnalysisSnapshot(normalized, std::move(scored), text, "fixture_discovery_" + normalized.id,
                                           ranked ? "fixture_scoring_" + normalized.id : "");
    ROCLINT_CHECK(result.ok());
    return std::move(result).value();
}

// Span of the unique occurrence of excerpt in text; fails the test if it is absent.
inline an::SourceSpan spanOf(std::string_view text, std::string_view excerpt) {
    const auto pos = text.find(excerpt);
    ROCLINT_CHECK(pos != std::string_view::npos);
    return an::SourceSpan{pos, pos + excerpt.size()};
}

// Builds sealed analyses whose IDs and evidence follow the host-minted conventions.
class AnalysisBuilder {
public:
    AnalysisBuilder(std::string sessionId, std::string revisionId, std::string text)
        : text_(std::move(text)) {
        a_.sessionId = std::move(sessionId);
        a_.baseRevisionId = std::move(revisionId);
    }

    // Each alternative is grounded on the same ambiguous excerpt.
    AnalysisBuilder& group(std::string question,
                           std::vector<std::string> labels,
                           std::string_view excerpt,
                           bool material = true) {
        an::ChoiceGroup g;
        const std::size_t gi = a_.choiceGroups.size();
        g.id = an::mintGroupId(gi);
        g.dimension = "scope";
        g.question = std::move(question);
        g.material = material;
        for (std::size_t i = 0; i < labels.size(); ++i) {
            an::ChoiceAlternative alt;
            alt.id = an::mintAlternativeId(gi, i);
            alt.interpretation = labels[i];
            alt.whyMaterial = "Leads to a different result: " + labels[i];
            if (!excerpt.empty()) {
                alt.evidence.push_back(spanOf(text_, excerpt));
            }
            g.alternatives.push_back(std::move(alt));
        }
        a_.choiceGroups.push_back(std::move(g));
        return *this;
    }

    AnalysisBuilder& assumption(std::string statement, std::string_view excerpt, bool material = true) {
        an::Assumption a;
        a.id = an::mintAssumptionId(a_.assumptions.size());
        a.statement = std::move(statement);
        a.material = material;
        if (!excerpt.empty()) {
            a.evidence.push_back(spanOf(text_, excerpt));
        }
        a_.assumptions.push_back(std::move(a));
        return *this;
    }

    AnalysisBuilder& missing(std::string question, bool blocking) {
        an::MissingDecision m;
        m.id = an::mintMissingDecisionId(a_.missingDecisions.size());
        m.question = std::move(question);
        m.blocking = blocking;
        a_.missingDecisions.push_back(std::move(m));
        return *this;
    }

    AnalysisBuilder& conflict(std::string description, bool blocking) {
        an::Conflict c;
        c.id = an::mintConflictId(a_.conflicts.size());
        c.description = std::move(description);
        c.blocking = blocking;
        a_.conflicts.push_back(std::move(c));
        return *this;
    }

    [[nodiscard]] an::NormalizedAnalysis build() {
        an::sealSnapshot(a_);
        return a_;
    }

    [[nodiscard]] const std::string& text() const noexcept { return text_; }

private:
    std::string text_;
    an::NormalizedAnalysis a_;
};

// Everything the intervention state machine needs for one analysis.
struct Pipeline {
    std::string text;
    an::NormalizedAnalysis analysis;
    an::OverallPolicyResult policy;
    roclint::bridge::SnapshotEvaluationResponse eval;
    roclint::core::intent::WorkingIntentState state;
    roclint::core::intent::InterventionDecision decision;

    Pipeline(std::string promptText, an::NormalizedAnalysis a)
        : text(std::move(promptText)), analysis(std::move(a)) {
        policy = an::MaterialityPolicyV1::evaluatePrompt(analysis, text);
        eval = roclint::bridge::S1TriageAdapter::degradedResponse(analysis, "test: scorer not configured");
        reevaluate();
    }

    void reevaluate() {
        decision = roclint::core::intent::InterventionStateMachine::evaluate(analysis, policy, eval, state);
        roclint::core::intent::InterventionStateMachine::recordPresented(decision, state);
    }
};

inline const std::string kThreeGroupPrompt =
    "Refactor the parser so it runs faster, update the docs where needed, and clean up the tests.";

// Three eligible, non-overlapping groups grounded on distinct phrases.
inline an::NormalizedAnalysis threeGroupAnalysis(const std::string& session = "sess_a",
                                                 const std::string& revision = "rev_sess_a_1") {
    return AnalysisBuilder(session, revision, kThreeGroupPrompt)
        .group("Which kind of speed matters?", {"Lower latency per call", "Higher batch throughput"}, "runs faster")
        .group("Which docs?", {"Only the README", "README and API reference"}, "where needed")
        .group("What does clean up mean?", {"Delete dead tests", "Rename and reorganize tests"}, "clean up the tests")
        .build();
}

inline const std::string kChainPrompt = "Refactor the parser so it runs faster.";
inline const std::string kChainAnswer = "Lower latency per call";

inline an::NormalizedAnalysis chainAnalysis(const std::string& session, const std::string& revision) {
    return AnalysisBuilder(session, revision, kChainPrompt)
        .group("Which kind of speed matters?", {kChainAnswer, "Higher batch throughput"}, "runs faster")
        .build();
}

inline roclint::bridge::GraniteModelInfo testModel() {
    return roclint::bridge::GraniteModelInfo{"granite4.2:3b-q8_0", "sha256:" + std::string(64, 'a'), "Q8_0",
                                             roclint::bridge::kLoopbackOllamaTransport};
}

inline std::string rewriteReply(const std::string& rewritten) {
    namespace json = roclint::core::json;
    json::Value v = json::Value::object();
    v.set("status", json::Value::string("rewritten"));
    v.set("rewritten_prompt", json::Value::string(rewritten));
    return json::serialize(v);
}

// One consistent analysis -> choice -> intent -> proposal chain for a session/revision.
// Holds pointers into itself via context(); do not copy or move after calling it.
struct Chain {
    std::string session;
    std::string revision;
    Pipeline pipe;
    an::AnalysisSnapshot snapshot;
    roclint::core::intent::ConfirmedIntent intent;
    roclint::core::rewrite::RewriteProposal proposal;

    explicit Chain(std::string sessionId = "sess_a",
                   std::uint64_t sequence = 1,
                   const std::string& rewritten = "Refactor the parser for lower latency per call.",
                   const std::string& profileId = "GENERIC@1",
                   std::uint64_t proposalGeneration = 0)
        : session(sessionId),
          revision(roclint::core::RevisionState::makeRevisionId(roclint::core::SessionId(sessionId), sequence).value()),
          pipe(kChainPrompt, chainAnalysis(session, revision)) {
        using roclint::core::intent::InterventionStateMachine;
        using roclint::core::intent::IntentProvenanceBuilder;
        ROCLINT_CHECK(InterventionStateMachine::applyChoice(pipe.analysis, pipe.decision, pipe.state, "alt_0_0").ok());
        pipe.reevaluate();
        ROCLINT_CHECK(pipe.decision.verdict == roclint::core::intent::InterventionVerdict::Quiet);
        snapshot = finalSnapshot(pipe.analysis, pipe.text, pipe.eval);
        auto built = IntentProvenanceBuilder::buildConfirmedIntent(snapshot, pipe.policy, pipe.state, 1000);
        ROCLINT_CHECK(built.ok());
        intent = std::move(built).value();

        roclint::bridge::GraniteRewriteRequest req;
        req.sessionId = session;
        req.revisionId = revision;
        req.originalText = kChainPrompt;
        req.analysis = snapshot;
        req.confirmedIntent = intent;
        req.rewriteProfileId = profileId;
        req.proposalEditGeneration = proposalGeneration;
        const auto profile = roclint::core::rewrite::ProfileRegistry::getProfile(profileId);
        ROCLINT_CHECK(profile.has_value());
        auto parsed = roclint::bridge::GraniteRewriteAdapter::parseRewriteResponse(rewriteReply(rewritten), req,
                                                                                   *profile, testModel(), 2000);
        ROCLINT_CHECK(parsed.ok());
        proposal = std::move(parsed).value();
    }

    Chain(const Chain&) = delete;
    Chain& operator=(const Chain&) = delete;

    [[nodiscard]] roclint::core::review::ApprovalContext context(std::uint64_t stateVersion = 2,
                                                                 std::uint64_t draftGeneration = 0) const {
        roclint::core::review::ApprovalContext ctx;
        ctx.sessionId = session;
        ctx.revisionId = revision;
        ctx.stateVersion = stateVersion;
        ctx.draftEditGeneration = draftGeneration;
        ctx.proposalEditGeneration = proposal.proposalEditGeneration;
        ctx.revisionStatus = roclint::core::RevisionStatus::Clean;
        ctx.analysis = &snapshot;
        ctx.intent = &intent;
        ctx.proposal = &proposal;
        ctx.validatorVersion = roclint::core::rewrite::kValidatorVersion;
        ctx.policyVersion = an::MaterialityPolicyV1::kPolicyVersion;
        return ctx;
    }

    [[nodiscard]] roclint::core::review::ApprovalRecord approve(std::uint64_t stateVersion = 2) const {
        auto a = roclint::core::review::ApprovalFactory::createApproval(context(stateVersion), 3000);
        ROCLINT_CHECK(a.ok());
        return std::move(a).value();
    }
};

} // namespace fx
