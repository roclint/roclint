#include "roclint/analysis/NormalizedAnalysis.hpp"
#include "roclint/analysis/SourceSpan.hpp"
#include "roclint/bridge/GraniteAdapter.hpp"
#include "roclint/core/prompt_state/PromptRevision.hpp"
#include "roclint/core/prompt_state/SessionId.hpp"
#include "roclint/core/prompt_state/RevisionId.hpp"
#include "support/check.hpp"

#include <iostream>
#include <string_view>

using namespace roclint::core;
using namespace roclint::core::analysis;
using namespace roclint::bridge;

void testEndToEndAnalysisPipeline() {
    
    SessionId sessionId("sess_checkpoint_001");
    RevisionId revId("rev_root_001");
    std::string promptText = "Update SessionManager timeout handling in FloatingCard.qml";
    PromptRevision promptRev(sessionId, revId, std::nullopt, promptText);

    ROCLINT_CHECK(promptRev.text() == promptText);

    GraniteAdapter adapter;
    GraniteOptionDiscoveryRequest req{
        .sessionId = promptRev.sessionId().value(),
        .revisionId = promptRev.revisionId().value(),
        .promptString = promptRev.text(),
        .s1Category = "target",
        .hostContextCategory = "CODE_EDITOR"
    };

    auto discoveryResp = adapter.discoverOptions(req);
    ROCLINT_CHECK(discoveryResp.success);
    ROCLINT_CHECK(discoveryResp.choiceGroup.has_value());

    auto group = *discoveryResp.choiceGroup;
    ROCLINT_CHECK(group.dimension == "target");
    ROCLINT_CHECK(group.alternatives.size() == 2);

   
    
    auto matchResult = findUniqueSpan(promptText, "FloatingCard.qml");
    ROCLINT_CHECK(matchResult.status == SpanMatchStatus::ExactMatch);
    ROCLINT_CHECK(matchResult.span.has_value());
    group.alternatives[0].evidence.push_back(*matchResult.span);

    NormalizedAnalysis analysis;
    analysis.id = generateDeterministicId("snapshot", 0, promptRev.revisionId().value());
    analysis.baseRevisionId = promptRev.revisionId().value();
    analysis.choiceGroups.push_back(group);

    
    auto valResult = validateNormalizedAnalysis(analysis, promptRev.text());
    ROCLINT_CHECK(valResult.isValid());
    ROCLINT_CHECK(!valResult.requiresManualReview());
    ROCLINT_CHECK(valResult.issues.empty());

    std::cout << "[I04 Walkthrough] End-to-end pipeline: Prompt -> Granite -> SourceSpan -> NormalizedAnalysis -> Validated OK!\n";
}

void testRejectionOfCorruptedProviderPayload() {
    std::string promptText = "Deploy 🚀 to production";

    GraniteAdapter adapter;
    // Mock a provider that violates Contract C03 by emitting 4 alternatives
    adapter.setMockHandler([](const GraniteOptionDiscoveryRequest&) {
        return R"({
            "status": "options_generated",
            "choice_group": {
                "dimension": "action",
                "question": "Which action?",
                "alternatives": [
                    {"id": "a1", "label": "Option 1"},
                    {"id": "a2", "label": "Option 2"},
                    {"id": "a3", "label": "Option 3"},
                    {"id": "a4", "label": "Option 4"}
                ]
            }
        })";
    });

    GraniteOptionDiscoveryRequest req{.promptString = promptText};
    auto resp = adapter.discoverOptions(req);
    ROCLINT_CHECK(resp.success);
    ROCLINT_CHECK(resp.choiceGroup.has_value());

    NormalizedAnalysis analysis;
    analysis.id = "snap_oversized";
    analysis.baseRevisionId = "rev_001";
    analysis.choiceGroups.push_back(*resp.choiceGroup);

    auto valResult = validateNormalizedAnalysis(analysis, promptText);
    ROCLINT_CHECK(!valResult.isValid());
    ROCLINT_CHECK(valResult.requiresManualReview());

    std::cout << "[I04 Walkthrough] Non-bounded model cardinality safely caught and routed to manual review!\n";
}

int main() {
    std::cout << "[Test I04 Analysis Checkpoint] Starting integration walkthrough...\n";
    testEndToEndAnalysisPipeline();
    testRejectionOfCorruptedProviderPayload();
    std::cout << "[Test I04 Analysis Checkpoint] Milestone ACHIEVED: Stardust can turn a prompt into validated typed pre-Jev analysis!\n";
    return 0;
}
