#include "roclint/analysis/MaterialityPolicy.hpp"
#include "roclint/analysis/NormalizedAnalysis.hpp"
#include "roclint/bridge/GraniteAdapter.hpp"
#include "roclint/core/prompt_state/RevisionState.hpp"
#include "support/check.hpp"

#include <iostream>
#include <string>

using namespace roclint::core;
using namespace roclint::core::analysis;
using namespace roclint::bridge;

namespace {

void loadModel(GraniteAdapter& adapter) {
    ROCLINT_CHECK(adapter.beginLoading());
    ROCLINT_CHECK(adapter.completeLoading(GraniteModelInfo{"granite4.2:3b-q8_0", "sha256:00ff", "Q8_0",
                                                           kLoopbackOllamaTransport}));
}

void testEndToEndAnalysisPipeline() {
    const SessionId session("sess_ckpt");
    RevisionState revisions(PromptRevision(session, RevisionState::makeRevisionId(session, 1), std::nullopt, "draft"));
    // The draft is frozen as a committed revision before analysis (CRLF from a Windows paste).
    auto frozen = revisions.commitEdit(1, "Update the timeout handling\r\nin the session layer.", 1000);
    ROCLINT_CHECK(frozen.ok());
    const auto& rev = revisions.currentRevision();
    ROCLINT_CHECK(rev.text() == "Update the timeout handling\nin the session layer.");

    GraniteAdapter adapter;
    loadModel(adapter);
    std::string seenInstruction;
    auto resp = adapter.discoverOptions(
        GraniteOptionDiscoveryRequest{session.value(), rev.revisionId().value(), rev.text(), "CODE_EDITOR"},
        [&](std::string_view instruction) -> Result<std::string> {
            seenInstruction = std::string(instruction);
            return std::string(R"({"status": "options_generated", "choice_groups": [
                {"dimension": "target", "question": "Which timeout should change?", "material": true,
                 "alternatives": [
                   {"label": "Connect timeout", "why_material": "Changes how long dialing waits", "evidence": ["timeout handling"]},
                   {"label": "Idle timeout", "why_material": "Changes when idle sessions close", "evidence": ["timeout handling"]}]}]})");
        });
    ROCLINT_CHECK(resp.ok());
    ROCLINT_CHECK(seenInstruction.find(rev.revisionId().value()) != std::string::npos);

    const auto& analysis = resp.value().analysis;
    ROCLINT_CHECK(analysis.baseRevisionId == rev.revisionId().value());
    ROCLINT_CHECK(isSnapshotSealed(analysis));

    const auto validation = validateNormalizedAnalysis(analysis, rev.text());
    ROCLINT_CHECK(validation.isValid());

    const auto policy = MaterialityPolicyV1::evaluatePrompt(analysis, rev.text());
    ROCLINT_CHECK(policy.isEligible());
    ROCLINT_CHECK(policy.groupResults.size() == 1);
    ROCLINT_CHECK(policy.groupResults[0].isEligible());
    std::cout << "[Analysis Checkpoint] prompt -> frozen revision -> Granite -> sealed analysis -> policy OK\n";
}

void testUnboundedProviderCardinalityRoutesToManualReview() {
    const std::string text = "Deploy \xf0\x9f\x9a\x80 to production";
    GraniteAdapter adapter;
    loadModel(adapter);
    auto resp = adapter.discoverOptions(
        GraniteOptionDiscoveryRequest{"sess_ckpt", "rev_sess_ckpt_1", text, "TERMINAL"},
        [](std::string_view) -> Result<std::string> {
            return std::string(R"({"status": "options_generated", "choice_groups": [
                {"question": "Which environment?", "material": true, "alternatives": [
                  {"label": "Option 1", "why_material": "w", "evidence": ["production"]},
                  {"label": "Option 2", "why_material": "w", "evidence": ["production"]},
                  {"label": "Option 3", "why_material": "w", "evidence": ["production"]},
                  {"label": "Option 4", "why_material": "w", "evidence": ["production"]}]}]})");
        });
    ROCLINT_CHECK(resp.ok());
    const auto& analysis = resp.value().analysis;
    ROCLINT_CHECK(validateNormalizedAnalysis(analysis, text).requiresManualReview());
    const auto policy = MaterialityPolicyV1::evaluatePrompt(analysis, text);
    ROCLINT_CHECK(policy.groupResults[0].verdict == PolicyVerdict::NeedsManualReview);
    ROCLINT_CHECK(policy.groupResults[0].reasonCode == ReasonCode::ExcessiveAlternatives);
}

void testUngroundedProviderGroupIsRejected() {
    const std::string text = "Make the dashboard nicer.";
    auto parsed = parseDiscoveryResponse(R"({"status": "options_generated", "choice_groups": [
        {"question": "Which colors?", "alternatives": [
          {"label": "Dark", "evidence": ["the colour palette"]},
          {"label": "Light", "evidence": ["the colour palette"]}]}]})",
                                         "sess_ckpt", "rev_sess_ckpt_1", text);
    ROCLINT_CHECK(parsed.ok());
    ROCLINT_CHECK(parsed.value().ungroundedEvidenceCount == 2);
    const auto policy = MaterialityPolicyV1::evaluatePrompt(parsed.value().analysis, text);
    ROCLINT_CHECK(policy.groupResults[0].verdict == PolicyVerdict::Rejected);
}

} // namespace

int main() {
    std::cout << "[Test Analysis Checkpoint] Starting...\n";
    testEndToEndAnalysisPipeline();
    testUnboundedProviderCardinalityRoutesToManualReview();
    testUngroundedProviderGroupIsRejected();
    std::cout << "[Test Analysis Checkpoint] All tests PASSED.\n";
    return 0;
}
