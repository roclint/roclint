#include "roclint/rewrite/RewriteProfile.hpp"
#include "roclint/bridge/GraniteRewriteAdapter.hpp"
#include "support/check.hpp"

#include <iostream>
#include <string>

using namespace roclint::core;
using namespace roclint::core::analysis;
using namespace roclint::core::intent;
using namespace roclint::core::rewrite;
using namespace roclint::bridge;

void testProfileRegistryDiscovery() {
    std::cout << "--- 1. Profile Registry Discovery ---\n";
    ROCLINT_CHECK(ProfileRegistry::defaultProfile().id == "GENERIC@1");

    auto pGeneric = ProfileRegistry::getProfile("GENERIC@1");
    ROCLINT_CHECK(pGeneric.has_value());
    ROCLINT_CHECK(pGeneric->family == ProfileFamily::Generic);

    auto pAnthropic = ProfileRegistry::getProfile("ANTHROPIC_STRUCTURED@1");
    ROCLINT_CHECK(pAnthropic.has_value());
    ROCLINT_CHECK(pAnthropic->family == ProfileFamily::AnthropicStructured);
    ROCLINT_CHECK(pAnthropic->formatXmlTags);

    auto pOpenAi = ProfileRegistry::getProfile("OPENAI_STRUCTURED@1");
    ROCLINT_CHECK(pOpenAi.has_value());
    ROCLINT_CHECK(pOpenAi->family == ProfileFamily::OpenAiStructured);
    ROCLINT_CHECK(pOpenAi->formatMarkdownSections);

    auto pLocal = ProfileRegistry::getProfile("LOCAL_COMPACT@1");
    ROCLINT_CHECK(pLocal.has_value());
    ROCLINT_CHECK(pLocal->family == ProfileFamily::LocalCompact);
    ROCLINT_CHECK(pLocal->compactEncoding);

    auto pUnknown = ProfileRegistry::getProfile("UNKNOWN_PROFILE@99");
    ROCLINT_CHECK(!pUnknown.has_value());
    ROCLINT_CHECK(!ProfileRegistry::isKnownProfile("INVALID_XYZ"));
    std::cout << "  [PASS] All 4 versioned profiles discovered and typed.\n";
}

void testUnknownProfileTypedRejection() {
    std::cout << "--- 2. Unknown Profile Typed Rejection (Guard 7) ---\n";
    GraniteRewriteAdapter adapter;

    NormalizedAnalysis snap;
    snap.id = "snap_b08p";
    auto intent = IntentProvenanceBuilder::buildConfirmedIntent("rev_1", snap, {});

    GraniteRewriteRequest req{
        .sessionId = "sess_1",
        .revisionId = "rev_1",
        .promptString = "Refactor connection logic",
        .confirmedIntent = intent,
        .rewriteProfileId = "NON_EXISTENT_PROFILE@42"
    };

    auto resp = adapter.synthesizeRewrite(req);
    ROCLINT_CHECK(!resp.success);
    ROCLINT_CHECK(resp.errorMessage.find("UNKNOWN_REWRITE_PROFILE") != std::string::npos);
    std::cout << "  [PASS] Unknown profile returns typed configuration failure without fallback.\n";
}

void testStructuralPreservationAcrossProfiles() {
    std::cout << "--- 3. Structural Formatting vs Semantic Invariance ---\n";
    GraniteRewriteAdapter adapter;

    NormalizedAnalysis snap;
    snap.id = "snap_b08p_struct";
    ChoiceGroup g;
    g.id = "target_component";
    g.alternatives.push_back(ChoiceAlternative{.id = "alt_card", .interpretation = "FloatingCard.qml"});
    snap.choiceGroups.push_back(g);

    auto intent = IntentProvenanceBuilder::buildConfirmedIntent("rev_1", snap, {
        ConfirmedItem{.itemId = "target_component", .kind = IntentResolutionKind::SelectedAlternative, .chosenValue = "alt_card"}
    });

    // 1. Generic Profile
    GraniteRewriteRequest reqGeneric{
        .sessionId = "sess_1",
        .revisionId = "rev_1",
        .promptString = "Add timeout handling",
        .confirmedIntent = intent,
        .rewriteProfileId = "GENERIC@1"
    };
    auto respGeneric = adapter.synthesizeRewrite(reqGeneric);
    ROCLINT_CHECK(respGeneric.success && respGeneric.proposal.has_value());
    const auto& propGeneric = *respGeneric.proposal;
    ROCLINT_CHECK(propGeneric.compiledPrompt.find("/* ROCLINT [PROFILE: GENERIC@1] */") != std::string::npos);
    ROCLINT_CHECK(propGeneric.compiledPrompt.find("alt_card") != std::string::npos);

    // 2. Anthropic Structured Profile
    GraniteRewriteRequest reqAnthropic{
        .sessionId = "sess_1",
        .revisionId = "rev_1",
        .promptString = "Add timeout handling",
        .confirmedIntent = intent,
        .rewriteProfileId = "ANTHROPIC_STRUCTURED@1"
    };
    auto respAnthropic = adapter.synthesizeRewrite(reqAnthropic);
    ROCLINT_CHECK(respAnthropic.success && respAnthropic.proposal.has_value());
    const auto& propAnthropic = *respAnthropic.proposal;
    ROCLINT_CHECK(propAnthropic.compiledPrompt.find("<task>") != std::string::npos);
    ROCLINT_CHECK(propAnthropic.compiledPrompt.find("<context>") != std::string::npos);
    ROCLINT_CHECK(propAnthropic.compiledPrompt.find("alt_card") != std::string::npos);

    // Both preserve exact confirmed intent items and required constraints
    ROCLINT_CHECK(propGeneric.constraints.size() == propAnthropic.constraints.size());
    ROCLINT_CHECK(propGeneric.constraints[0].id == propAnthropic.constraints[0].id);
    std::cout << "  [PASS] Identical ConfirmedIntent produces tailored structures preserving semantics.\n";
}

int main() {
    std::cout << "[TEST] Running B08P Target-Oriented Rewrite Profiles Tests...\n\n";

    testProfileRegistryDiscovery();
    testUnknownProfileTypedRejection();
    testStructuralPreservationAcrossProfiles();

    std::cout << "\n=========================================================\n";
    std::cout << "  ALL B08P REWRITE PROFILES TESTS PASSED!\n";
    std::cout << "=========================================================\n";
    return 0;
}
