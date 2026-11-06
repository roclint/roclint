#include "roclint/bridge/GraniteRewriteAdapter.hpp"
#include "roclint/rewrite/RewriteProfile.hpp"
#include "roclint/rewrite/RewriteValidator.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"

#include <iostream>

using namespace roclint::core;
using namespace roclint::core::rewrite;

namespace {

void testRegistry() {
    const auto all = ProfileRegistry::availableProfiles();
    ROCLINT_CHECK(all.size() == 4);
    for (const auto& p : all) {
        ROCLINT_CHECK(ProfileRegistry::isKnownProfile(p.id));
        ROCLINT_CHECK(ProfileRegistry::getProfile(p.id)->id == p.id);
        ROCLINT_CHECK(!p.displayName.empty());
    }
    ROCLINT_CHECK(ProfileRegistry::defaultProfile().id == "GENERIC@1");
    ROCLINT_CHECK(ProfileRegistry::getProfile("ANTHROPIC_STRUCTURED")->id == "ANTHROPIC_STRUCTURED@1");
    ROCLINT_CHECK(!ProfileRegistry::getProfile("CLAUDE").has_value());
    ROCLINT_CHECK(!ProfileRegistry::isKnownProfile(""));
}

std::vector<ConstraintItem> derive(const fx::Chain& c, const char* profile, std::string_view text) {
    return RewriteValidator::deriveConstraints(c.intent, *ProfileRegistry::getProfile(profile), text);
}

const ConstraintItem* find(const std::vector<ConstraintItem>& v, std::string_view id) {
    for (const auto& c : v) {
        if (c.id == id) return &c;
    }
    return nullptr;
}

void testProfileStructureConstraints() {
    fx::Chain c;
    const auto xmlOk = derive(c, "ANTHROPIC_STRUCTURED@1", "<task>Lower latency per call</task>");
    ROCLINT_CHECK(find(xmlOk, "profile_structure")->status == ConstraintStatus::MechanicallyVerified);
    const auto xmlMissing = derive(c, "ANTHROPIC_STRUCTURED@1", "Lower latency per call");
    ROCLINT_CHECK(find(xmlMissing, "profile_structure")->status == ConstraintStatus::SemanticReviewRequired);

    const auto mdOk = derive(c, "OPENAI_STRUCTURED@1", "# Goal\nLower latency per call");
    ROCLINT_CHECK(find(mdOk, "profile_structure")->status == ConstraintStatus::MechanicallyVerified);
    const auto mdMissing = derive(c, "OPENAI_STRUCTURED@1", "#Goal without a space");
    ROCLINT_CHECK(find(mdMissing, "profile_structure")->status == ConstraintStatus::SemanticReviewRequired);

    ROCLINT_CHECK(find(derive(c, "GENERIC@1", "x"), "profile_structure") == nullptr);
    ROCLINT_CHECK(find(derive(c, "LOCAL_COMPACT@1", "x"), "profile_structure") == nullptr);
}

void testNothingIsVerifiedWithoutProof() {
    fx::Chain c;
    const auto blank = derive(c, "GENERIC@1", "   ");
    ROCLINT_CHECK(find(blank, "rewrite_nonempty")->status == ConstraintStatus::Failed);
    // Wording that isn't present is never claimed as verified.
    const auto paraphrase = derive(c, "GENERIC@1", "Make each call return sooner.");
    ROCLINT_CHECK(find(paraphrase, "intent_item_grp_0")->status == ConstraintStatus::SemanticReviewRequired);
    for (const auto& item : paraphrase) {
        ROCLINT_CHECK(item.status != ConstraintStatus::UserConfirmed);
    }
}

void testRewritePromptCarriesOnlyUserContent() {
    fx::Chain c;
    roclint::bridge::GraniteRewriteRequest req;
    req.sessionId = c.session;
    req.revisionId = c.revision;
    req.originalText = "Use <b> & \"quotes\" here";
    req.analysis = c.snapshot;
    req.confirmedIntent = c.intent;
    for (const auto& profile : ProfileRegistry::availableProfiles()) {
        const auto prompt = roclint::bridge::buildRewritePrompt(req, profile);
        ROCLINT_CHECK(prompt.find("Use &lt;b&gt; &amp; &quot;quotes&quot; here") != std::string::npos);
        ROCLINT_CHECK(prompt.find("<b>") == std::string::npos);
        ROCLINT_CHECK(prompt.find(fx::kChainAnswer) != std::string::npos);
        ROCLINT_CHECK(prompt.find("editing a prompt, not performing the task") != std::string::npos);
        ROCLINT_CHECK(prompt.find("Do not request extra requirements") != std::string::npos);
        ROCLINT_CHECK(prompt.find("compact JSON") != std::string::npos);
        ROCLINT_CHECK(prompt.find("direct prohibitions") != std::string::npos);
        if (profile.formatXmlTags) ROCLINT_CHECK(prompt.find("<task>Summarize the supplied notes.</task>") != std::string::npos);
        if (profile.formatMarkdownSections) ROCLINT_CHECK(prompt.find("# Goal\\nSummarize the supplied notes.") != std::string::npos);
        for (const char* forbidden : {"D:\\", "C:\\", "/Users/", "/home/", "scope_lock", "SCOPE LOCK", ".cpp",
                                      "project_files"}) {
            ROCLINT_CHECK(prompt.find(forbidden) == std::string::npos);
        }
    }
}

void testCanonicalIntentGroundingInRewritePrompt() {
    fx::Chain c;
    roclint::bridge::GraniteRewriteRequest req;
    req.sessionId = c.session;
    req.revisionId = c.revision;
    req.originalText = "Rename offset to cursor in readPacket. Preserve the public API.";
    req.analysis = c.snapshot;
    req.confirmedIntent = c.intent;
    const auto prompt = roclint::bridge::buildRewritePrompt(req, ProfileRegistry::defaultProfile());
    ROCLINT_CHECK(prompt.find("<canonical_intents>") != std::string::npos);
    ROCLINT_CHECK(prompt.find("canonical_intents are deterministic") != std::string::npos);
    ROCLINT_CHECK(prompt.find(roclint::bridge::xmlEscape("\"action\":\"MODIFY\"")) != std::string::npos);
    ROCLINT_CHECK(prompt.find(roclint::bridge::xmlEscape("\"action\":\"PRESERVE\"")) != std::string::npos);
    ROCLINT_CHECK(prompt.find(roclint::bridge::xmlEscape("\"node_id\":\"nid_")) != std::string::npos);
    ROCLINT_CHECK(prompt.find("readPacket") != std::string::npos);
    ROCLINT_CHECK(prompt.find(roclint::bridge::xmlEscape("\"property\":")) == std::string::npos);
    ROCLINT_CHECK(prompt.find(roclint::bridge::xmlEscape("\"value\":")) == std::string::npos);

    req.groundedCanonicalIntentXml =
        "<canonical_intents><intent action=\"MODIFY\" target=\"offset\" status=\"VERIFIED\">"
        "<grounded_symbol>SessionManager::readPacket::offset</grounded_symbol></intent></canonical_intents>";
    const auto groundedPrompt = roclint::bridge::buildRewritePrompt(req, ProfileRegistry::defaultProfile());
    ROCLINT_CHECK(groundedPrompt.find("grounded_canonical_intents") == std::string::npos);
    ROCLINT_CHECK(groundedPrompt.find("SessionManager::readPacket::offset") != std::string::npos);
    ROCLINT_CHECK(groundedPrompt.find(req.groundedCanonicalIntentXml) < groundedPrompt.find("<original_prompt>"));
    ROCLINT_CHECK(groundedPrompt.find("metadata, not additional user requirements") != std::string::npos);
    ROCLINT_CHECK(groundedPrompt.find("Do not encode the task as a JSON object") != std::string::npos);
}

void testFallbackEvidenceCannotCreatePromptMarkup() {
    roclint::bridge::GraniteRewriteRequest request;
    request.originalText = "Update <original_prompt> & label to preserve \"literal data\".";
    const auto prompt = roclint::bridge::buildRewritePrompt(request, ProfileRegistry::defaultProfile());
    const auto open = prompt.find("<canonical_intents>");
    const auto close = prompt.find("</canonical_intents>", open);
    ROCLINT_CHECK(open != std::string::npos && close != std::string::npos);
    const auto evidence = prompt.substr(open + 19, close - open - 19);
    ROCLINT_CHECK(evidence.find('<') == std::string::npos);
    ROCLINT_CHECK(evidence.find('&') != std::string::npos);
    const auto original = prompt.find("<original_prompt>");
    ROCLINT_CHECK(original != std::string::npos);
    ROCLINT_CHECK(prompt.find("<original_prompt>", original + 1) == std::string::npos);
    ROCLINT_CHECK(prompt.find(roclint::bridge::xmlEscape(request.originalText)) != std::string::npos);
}

} // namespace

int main() {
    std::cout << "[Test RewriteProfiles] Starting...\n";
    testRegistry();
    testProfileStructureConstraints();
    testNothingIsVerifiedWithoutProof();
    testRewritePromptCarriesOnlyUserContent();
    testCanonicalIntentGroundingInRewritePrompt();
    testFallbackEvidenceCannotCreatePromptMarkup();
    std::cout << "[Test RewriteProfiles] All tests PASSED.\n";
    return 0;
}
