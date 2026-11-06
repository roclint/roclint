#include "roclint/rewrite/RewriteProposal.hpp"
#include "roclint/rewrite/RewriteValidator.hpp"
#include "support/check.hpp"
#include "support/fixtures.hpp"
#include "support/wording_fixtures.hpp"

#include <iostream>

using namespace roclint::core;
using namespace roclint::core::rewrite;

namespace {

ValidationResult validate(const fx::Chain& c, const RewriteProposal& p, const std::set<std::string>& acks = {}) {
    return RewriteValidator::validateProposal(p, c.session, c.revision, c.intent, acks);
}

void testParsedProposalIsSealedAndBound() {
    fx::Chain c;
    const auto& p = c.proposal;
    ROCLINT_CHECK(p.status == ProposalStatus::ProposalReady);
    ROCLINT_CHECK(p.id == "prop_" + p.proposalHash);
    ROCLINT_CHECK(ProposalHasher::computeProposalHash(p) == p.proposalHash);
    ROCLINT_CHECK(p.sessionId == c.session);
    ROCLINT_CHECK(p.baseRevisionId == c.revision);
    ROCLINT_CHECK(p.intentId == c.intent.id);
    ROCLINT_CHECK(p.snapshotId == c.snapshot.id);
    ROCLINT_CHECK(p.modelDigest == fx::testModel().digest);
    ROCLINT_CHECK(p.quantization == "Q8_0");
    ROCLINT_CHECK(p.transport == std::string(roclint::bridge::kLoopbackOllamaTransport));
    ROCLINT_CHECK(p.diffText == RewriteValidator::computeAccessibleDiff(p.originalText, p.rewrittenText));
}

void testHashCoversBoundFieldsButNotTimestamp() {
    fx::Chain c;
    auto p = c.proposal;
    p.createdAtMs = 999999;
    ROCLINT_CHECK(ProposalHasher::computeProposalHash(p) == c.proposal.proposalHash);
    for (auto mutate : {+[](RewriteProposal& x) { x.rewrittenText += "!"; },
                        +[](RewriteProposal& x) { x.modelDigest = "sha256:other"; },
                        +[](RewriteProposal& x) { x.proposalEditGeneration += 1; },
                        +[](RewriteProposal& x) { x.rewriteProfileId = "LOCAL_COMPACT@1"; },
                        +[](RewriteProposal& x) { x.constraints.clear(); }}) {
        auto q = c.proposal;
        mutate(q);
        ROCLINT_CHECK(ProposalHasher::computeProposalHash(q) != c.proposal.proposalHash);
    }
}

void testAstSnapshotHashSwitchesToV2Domain() {
    fx::Chain c;
    auto legacy = c.proposal;
    ROCLINT_CHECK(legacy.astSnapshotHash.empty());
    const auto legacyHash = legacy.proposalHash;
    ROCLINT_CHECK(ProposalHasher::computeProposalHash(legacy) == legacyHash);

    auto astBound = legacy;
    astBound.astSnapshotHash = std::string(64, 'a');
    ProposalHasher::seal(astBound);
    ROCLINT_CHECK(astBound.proposalHash != legacyHash);
    ROCLINT_CHECK(ProposalHasher::computeProposalHash(astBound) == astBound.proposalHash);

    auto changed = astBound;
    changed.astSnapshotHash = std::string(64, 'b');
    ROCLINT_CHECK(ProposalHasher::computeProposalHash(changed) != astBound.proposalHash);

    auto legacyAgain = astBound;
    legacyAgain.astSnapshotHash.clear();
    ProposalHasher::seal(legacyAgain);
    ROCLINT_CHECK(legacyAgain.proposalHash == legacyHash);
}

void testParserCarriesAstSnapshotHash() {
    fx::Chain c;
    roclint::bridge::GraniteRewriteRequest req;
    req.sessionId = c.session;
    req.revisionId = c.revision;
    req.originalText = fx::kChainPrompt;
    req.analysis = c.snapshot;
    req.confirmedIntent = c.intent;
    req.astSnapshotHash = std::string(64, 'c');
    const auto parsed = roclint::bridge::GraniteRewriteAdapter::parseRewriteResponse(
        R"({"status":"rewritten","rewritten_prompt":"  Keep structure.\n"})", req,
        ProfileRegistry::defaultProfile(), fx::testModel(), 1);
    ROCLINT_CHECK(parsed.ok());
    ROCLINT_CHECK(parsed.value().astSnapshotHash == req.astSnapshotHash);
    ROCLINT_CHECK(parsed.value().status == ProposalStatus::ProposalReady);
}

void testValidProposalPassesWithoutPendingReview() {
    fx::Chain c;
    const auto r = validate(c, c.proposal);
    ROCLINT_CHECK(r.structurallyValid);
    ROCLINT_CHECK(r.isValid);
    ROCLINT_CHECK(r.pendingAcknowledgments.empty());
    ROCLINT_CHECK(c.proposal.getConstraint("intent_item_grp_0")->status == ConstraintStatus::MechanicallyVerified);
}

void testSemanticReviewRequiresAcknowledgment() {
    fx::Chain c("sess_a", 1, "Make the parser quicker.");
    ROCLINT_CHECK(c.proposal.getConstraint("intent_item_grp_0")->status == ConstraintStatus::SemanticReviewRequired);
    const auto pending = validate(c, c.proposal);
    ROCLINT_CHECK(pending.structurallyValid);
    ROCLINT_CHECK(!pending.isValid);
    ROCLINT_CHECK(pending.pendingAcknowledgments.size() == 1);
    ROCLINT_CHECK(pending.pendingAcknowledgments[0] == "intent_item_grp_0");
    ROCLINT_CHECK(validate(c, c.proposal, {"intent_item_grp_0"}).isValid);
}

void testForgedOrStaleProposalsAreRejected() {
    fx::Chain c;

    auto forgedClaim = c.proposal;
    forgedClaim.constraints.back().status = ConstraintStatus::MechanicallyVerified;
    forgedClaim.rewrittenText = "Something unrelated.";
    forgedClaim.diffText = RewriteValidator::computeAccessibleDiff(forgedClaim.originalText, forgedClaim.rewrittenText);
    ProposalHasher::seal(forgedClaim);
    ROCLINT_CHECK(!validate(c, forgedClaim).structurallyValid);

    auto forgedDiff = c.proposal;
    forgedDiff.diffText = "  nothing changed\n";
    ProposalHasher::seal(forgedDiff);
    ROCLINT_CHECK(!validate(c, forgedDiff).structurallyValid);

    auto unsealed = c.proposal;
    unsealed.rewrittenText += " Also delete the tests.";
    ROCLINT_CHECK(!validate(c, unsealed).structurallyValid);

    auto crlf = c.proposal;
    crlf.rewrittenText = "a\r\nb";
    ProposalHasher::seal(crlf);
    ROCLINT_CHECK(!validate(c, crlf).structurallyValid);

    auto unknownProfile = c.proposal;
    unknownProfile.rewriteProfileId = "MYSTERY@9";
    ProposalHasher::seal(unknownProfile);
    ROCLINT_CHECK(!validate(c, unknownProfile).structurallyValid);

    const auto otherRev = RewriteValidator::validateProposal(c.proposal, c.session, "rev_sess_a_2", c.intent, {});
    ROCLINT_CHECK(!otherRev.isValid && otherRev.finalStatus == ProposalStatus::StaleProposal);

    const auto otherSession = RewriteValidator::validateProposal(c.proposal, "sess_b", c.revision, c.intent, {});
    ROCLINT_CHECK(!otherSession.isValid && otherSession.finalStatus == ProposalStatus::Rejected);

    auto intentSwap = c.intent;
    intentSwap.items[0].chosenAlternativeId = "alt_0_1";
    intentSwap.intentHash = intent::IntentProvenanceBuilder::computeIntentHash(intentSwap);
    intentSwap.id = "intent_" + intentSwap.intentHash;
    const auto staleIntent = RewriteValidator::validateProposal(c.proposal, c.session, c.revision, intentSwap, {});
    ROCLINT_CHECK(!staleIntent.isValid && staleIntent.finalStatus == ProposalStatus::StaleProposal);
}

void testNeedsClarificationIsNotApprovable() {
    fx::Chain c;
    roclint::bridge::GraniteRewriteRequest req;
    req.sessionId = c.session;
    req.revisionId = c.revision;
    req.originalText = fx::kChainPrompt;
    req.analysis = c.snapshot;
    req.confirmedIntent = c.intent;
    auto p = roclint::bridge::GraniteRewriteAdapter::parseRewriteResponse(
        R"({"status": "needs_clarification", "question": "Which parser?"})", req,
        ProfileRegistry::defaultProfile(), fx::testModel(), 1);
    ROCLINT_CHECK(p.ok());
    ROCLINT_CHECK(p.value().status == ProposalStatus::NeedsClarification);
    const auto r = validate(c, p.value());
    ROCLINT_CHECK(!r.isValid);
    ROCLINT_CHECK(r.finalStatus == ProposalStatus::NeedsClarification);
    ROCLINT_CHECK(r.rejectionReason == "Which parser?");
}

void testRewriteParserResilience() {
    fx::Chain c;
    roclint::bridge::GraniteRewriteRequest req;
    req.sessionId = c.session; req.revisionId = c.revision;
    req.originalText = fx::kChainPrompt; req.analysis = c.snapshot; req.confirmedIntent = c.intent;
    const auto parse = [&](const std::string& output) {
        return roclint::bridge::GraniteRewriteAdapter::parseRewriteResponse(output, req,
            ProfileRegistry::defaultProfile(), fx::testModel(), 1);
    };
    const std::string valid = R"({"status":"rewritten","rewritten_prompt":"  Keep structure.\n"})";
    for (const auto& output : {valid, "```json\n" + valid + "\n```", " \t```\r\n" + valid + "\r\n```\n"}) {
        const auto result = parse(output);
        ROCLINT_CHECK(result.ok());
        ROCLINT_CHECK(result.value().rewrittenText == "  Keep structure.\n");
    }
    const auto question = parse(R"({"status":"needs_clarification","question":" \r\nWhich parser?\t "})");
    ROCLINT_CHECK(question.ok() && question.value().clarificationQuestion == "Which parser?");
    for (const auto& output : {std::string("{}"), std::string(R"({"status":null})"),
            std::string(R"({"status":42})"), std::string(R"({"status":""})"), std::string(R"({"status":"success"})"),
            std::string(R"({"status":"rewritten"})"), std::string(R"({"status":"rewritten","rewritten_prompt":null})"),
            std::string(R"({"status":"rewritten","rewritten_prompt":""})"),
            std::string(R"({"status":"needs_clarification","question":" \n\t\r\f\u000b"})"),
            std::string(R"({"status":"needs_clarification","question":null})"),
            std::string(R"({"status":"needs_clarification"})"), std::string(R"({"status":"rewritten","status":"rewritten"})"),
            "Here: " + valid, valid + " Done.", "```json\n" + valid, valid.substr(0, valid.size() - 1),
            "```json\n" + valid + "\n```\n```\n{}\n```", std::string(65537, ' ')})
        ROCLINT_CHECK(!parse(output).ok());
}

void testKnownFidelityRegressions() {
    intent::ConfirmedIntent intent;
    intent::ConfirmedItem item;
    item.kind = intent::IntentResolutionKind::SelectedAlternative;
    item.chosenValue = "Allow minor semantic changes for speed";
    intent.items.push_back(item);
    ROCLINT_CHECK(!RewriteValidator::validateFidelity("Refactor the parser.", intent,
        "Refactor the parser, allowing broad semantic reinterpretation of input.").ok());
    ROCLINT_CHECK(RewriteValidator::validateFidelity("Refactor the parser.", intent,
        "Refactor the parser, allowing minor semantic changes for speed.").ok());
    ROCLINT_CHECK(RewriteValidator::validateFidelity("Refactor the parser.", intent,
        "Allow minor semantic changes. Do not allow broad semantic changes.").ok());
    ROCLINT_CHECK(!RewriteValidator::validateFidelity("Allow minor semantic changes. Do not allow broad semantic changes.", intent,
        "Allow broad semantic changes.").ok());
    ROCLINT_CHECK(!RewriteValidator::validateFidelity("Improve this code without changing its API.", intent,
        "Improve this code and keep it working.").ok());
    ROCLINT_CHECK(RewriteValidator::validateFidelity("Improve this code without changing its API.", intent,
        "Improve the implementation without changing its API.").ok());
    ROCLINT_CHECK(!RewriteValidator::validateFidelity("Make the parser faster.", intent,
        "Make the parser run in less than 100ms.").ok());
    item.chosenValue = "Less than 100 ms"; intent.items.push_back(item);
    ROCLINT_CHECK(RewriteValidator::validateFidelity("Make the parser faster.", intent,
        "Make the parser run in less than 100ms.").ok());
    ROCLINT_CHECK(RewriteValidator::validateFidelity("Make a 1968 Beatles-inspired homepage.", {},
        "Make a homepage inspired by a 1968 Beatles album.").ok());
}

void testLeanWordingContractPreservesRequestedContent() {
    int index=0;
    for (const auto& item : fx::leanWordingCases()) {
        const auto request=fx::wordingRequest(item,index++);
        const auto profile=ProfileRegistry::getProfile(item.profile); ROCLINT_CHECK(profile.has_value());
        const auto instructions=roclint::bridge::buildRewritePrompt(request,*profile);
        ROCLINT_CHECK(instructions.find("granite.rewrite.format.v6") != std::string::npos);
        ROCLINT_CHECK(instructions.find("shortest wording") != std::string::npos);
        ROCLINT_CHECK(instructions.find("Retain explicitly requested usability, accessibility and quality") != std::string::npos);
        ROCLINT_CHECK(instructions.find(roclint::bridge::xmlEscape(item.original)) != std::string::npos);
        const auto result=roclint::bridge::GraniteRewriteAdapter::parseRewriteResponse(fx::rewriteReply(item.expected), request,*profile,fx::testModel(),2000);
        ROCLINT_CHECK(result.ok() && result.value().rewrittenText==item.expected && result.value().originalText==item.original);
        ROCLINT_CHECK(ProposalHasher::computeProposalHash(result.value())==result.value().proposalHash);
    }
}

void testAccessibleDiff() {
    ROCLINT_CHECK(RewriteValidator::computeAccessibleDiff("a\nb\nc", "a\nB\nc\nd") ==
                  "  a\n~ was: b\n~ now: B\n  c\n+ d\n");
    ROCLINT_CHECK(RewriteValidator::computeAccessibleDiff("a\nb", "a") == "  a\n- b\n");
    ROCLINT_CHECK(RewriteValidator::computeAccessibleDiff("", "x") == "+ x\n");
}

} // namespace

int main() {
    std::cout << "[Test RewriteProposal] Starting...\n";
    testParsedProposalIsSealedAndBound();
    testHashCoversBoundFieldsButNotTimestamp();
    testAstSnapshotHashSwitchesToV2Domain();
    testParserCarriesAstSnapshotHash();
    testValidProposalPassesWithoutPendingReview();
    testSemanticReviewRequiresAcknowledgment();
    testForgedOrStaleProposalsAreRejected();
    testNeedsClarificationIsNotApprovable();
    testKnownFidelityRegressions();
    testLeanWordingContractPreservesRequestedContent();
    testAccessibleDiff();
    testRewriteParserResilience();
    std::cout << "[Test RewriteProposal] All tests PASSED.\n";
    return 0;
}
