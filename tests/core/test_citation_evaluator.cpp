#include "roclint/core/citation_evaluator.hpp"
#include "support/check.hpp"

#include <iostream>
#include <string>
#include <vector>

using namespace roclint::core;

namespace {

void testCitationVerdictEnumsAndJson() {
    ROCLINT_CHECK(parseCitationVerdict("Verified").value() == CitationVerdict::Verified);
    ROCLINT_CHECK(parseCitationVerdict("Overreached").value() == CitationVerdict::Overreached);
    ROCLINT_CHECK(parseCitationVerdict("InvalidSpan").value() == CitationVerdict::InvalidSpan);
    ROCLINT_CHECK(parseCitationVerdict("Fabricated").value() == CitationVerdict::Fabricated);
    ROCLINT_CHECK(!parseCitationVerdict("Unknown").ok());

    CitationEvaluation eval;
    eval.constraintId = "c_eval_01";
    eval.verdict = CitationVerdict::Overreached;
    eval.assignedStatus = EvidenceStatus::Rejected;
    eval.explanation = "Detected source-span laundering.";
    eval.verifiedConstraint.id = "c_eval_01";
    eval.verifiedConstraint.rule = "Invented rule";
    eval.verifiedConstraint.origin = Origin::Model;
    eval.verifiedConstraint.enforcement = Enforcement::Advisory;

    const auto jsonVal = toJson(eval);
    const auto parsedRes = citationEvaluationFromJson(jsonVal);
    ROCLINT_CHECK(parsedRes.ok());
    ROCLINT_CHECK(parsedRes.value() == eval);
}

void testSourceSpanLaunderingDetection() {
    CitationEvaluator evaluator;
    const std::string prompt = "Make a creative Layer 3 networking visualization, but technically accurate.";
    
    // Find byte offset of "technically accurate"
    const auto offset = prompt.find("technically accurate");
    ROCLINT_CHECK(offset != std::string::npos);

    // Attemped laundering: Model tries to claim the user demanded CIDR routing tables
    // citing the span "technically accurate".
    ScopedConstraint launderedCandidate;
    launderedCandidate.id = "c_laundering";
    launderedCandidate.rule = "Display real CIDR routing tables on screen";
    launderedCandidate.origin = Origin::User;
    launderedCandidate.derivation = Derivation::Direct;
    launderedCandidate.enforcement = Enforcement::Hard;
    launderedCandidate.evidenceStatus = EvidenceStatus::Unverified;
    launderedCandidate.evidence = {SourceSpan{"p0", offset, offset + std::string("technically accurate").size()}};

    auto eval = evaluator.evaluate(prompt, "p0", launderedCandidate);
    
    // Laundering must be caught as Overreached
    ROCLINT_CHECK(eval.verdict == CitationVerdict::Overreached);
    ROCLINT_CHECK(eval.assignedStatus == EvidenceStatus::Rejected);
    
    // The adjusted constraint must be safely demoted to Model Advisory
    ROCLINT_CHECK(eval.verifiedConstraint.origin == Origin::Model);
    ROCLINT_CHECK(eval.verifiedConstraint.derivation == Derivation::Assumed);
    ROCLINT_CHECK(eval.verifiedConstraint.enforcement == Enforcement::Advisory);
    ROCLINT_CHECK(eval.verifiedConstraint.evidenceStatus == EvidenceStatus::Unverified);

    // Safeguard check: demoted constraint CANNOT be promoted to Hard!
    eval.verifiedConstraint.enforcement = Enforcement::Hard;
    ROCLINT_CHECK(!canPromoteToEnforceable(eval.verifiedConstraint));
}

void testLegitimateUserConstraintVerification() {
    CitationEvaluator evaluator;
    const std::string prompt = "Refactor auth, but don't touch token validation.";
    
    const auto offset = prompt.find("don't touch token validation");
    ROCLINT_CHECK(offset != std::string::npos);
    const std::size_t spanLen = std::string("don't touch token validation").size();

    ScopedConstraint legitCandidate;
    legitCandidate.id = "c_legit_auth";
    legitCandidate.rule = "Do not modify token validation implementation";
    legitCandidate.origin = Origin::User;
    legitCandidate.derivation = Derivation::Direct;
    legitCandidate.enforcement = Enforcement::Hard;
    legitCandidate.polarity = Polarity::Prohibit;
    legitCandidate.evidenceStatus = EvidenceStatus::Unverified;
    legitCandidate.evidence = {SourceSpan{"p0", offset, offset + spanLen}};

    const auto eval = evaluator.evaluate(prompt, "p0", legitCandidate);

    ROCLINT_CHECK(eval.verdict == CitationVerdict::Verified);
    ROCLINT_CHECK(eval.assignedStatus == EvidenceStatus::Supported);
    ROCLINT_CHECK(eval.verifiedConstraint.evidenceStatus == EvidenceStatus::Supported);
    ROCLINT_CHECK(eval.verifiedConstraint.enforcement == Enforcement::Hard);
    ROCLINT_CHECK(canPromoteToEnforceable(eval.verifiedConstraint));
}

void testNegationMismatchDetection() {
    CitationEvaluator evaluator;
    const std::string prompt = "Refactor auth and touch token validation.";
    
    const auto offset = prompt.find("touch token validation");
    ROCLINT_CHECK(offset != std::string::npos);
    const std::size_t spanLen = std::string("touch token validation").size();

    // Model claims Polarity::Prohibit, but the prompt affirmed it!
    ScopedConstraint mismatchCandidate;
    mismatchCandidate.id = "c_mismatch";
    mismatchCandidate.rule = "Do not touch token validation";
    mismatchCandidate.origin = Origin::User;
    mismatchCandidate.derivation = Derivation::Direct;
    mismatchCandidate.enforcement = Enforcement::Hard;
    mismatchCandidate.polarity = Polarity::Prohibit;
    mismatchCandidate.evidence = {SourceSpan{"p0", offset, offset + spanLen}};

    const auto eval = evaluator.evaluate(prompt, "p0", mismatchCandidate);
    ROCLINT_CHECK(eval.verdict == CitationVerdict::Overreached);
    ROCLINT_CHECK(eval.assignedStatus == EvidenceStatus::Rejected);
    ROCLINT_CHECK(eval.verifiedConstraint.origin == Origin::Model);
}

void testInvalidAndUtf8SlicedSpans() {
    CitationEvaluator evaluator;
    const std::string prompt = "Layer 3 • 🚀 packet tracer";

    // 1. Span out of bounds
    ScopedConstraint oobCandidate;
    oobCandidate.id = "c_oob";
    oobCandidate.rule = "Valid rule";
    oobCandidate.origin = Origin::User;
    oobCandidate.evidence = {SourceSpan{"p0", 100, 200}};

    const auto evalOob = evaluator.evaluate(prompt, "p0", oobCandidate);
    ROCLINT_CHECK(evalOob.verdict == CitationVerdict::InvalidSpan);
    ROCLINT_CHECK(evalOob.assignedStatus == EvidenceStatus::Rejected);

    // 2. Span slicing inside 4-byte rocket emoji
    const auto rocketPos = prompt.find("🚀");
    ROCLINT_CHECK(rocketPos != std::string::npos);
    ScopedConstraint slicedCandidate;
    slicedCandidate.id = "c_sliced";
    slicedCandidate.rule = "Valid rule";
    slicedCandidate.origin = Origin::User;
    slicedCandidate.evidence = {SourceSpan{"p0", rocketPos, rocketPos + 2}};

    const auto evalSliced = evaluator.evaluate(prompt, "p0", slicedCandidate);
    ROCLINT_CHECK(evalSliced.verdict == CitationVerdict::InvalidSpan);
    ROCLINT_CHECK(evalSliced.assignedStatus == EvidenceStatus::Rejected);

    // 3. Empty evidence list
    ScopedConstraint emptyEvidence;
    emptyEvidence.id = "c_empty";
    emptyEvidence.origin = Origin::User;
    const auto evalEmpty = evaluator.evaluate(prompt, "p0", emptyEvidence);
    ROCLINT_CHECK(evalEmpty.verdict == CitationVerdict::InvalidSpan);
}

void testDomainAuthorityVerification() {
    CitationEvaluator evaluator;
    evaluator.registerAuthorityPrefix("RFC ");

    // 1. Recognized RFC domain invariant
    ScopedConstraint rfcCandidate;
    rfcCandidate.id = "c_rfc1812";
    rfcCandidate.rule = "Destination forwarding follows longest prefix match";
    rfcCandidate.origin = Origin::Domain;
    rfcCandidate.derivation = Derivation::Entailed;
    rfcCandidate.enforcement = Enforcement::Conditional;
    rfcCandidate.authorityRef = "RFC 1812 §4.2.2.1";
    rfcCandidate.appliesWhen = "depicts_ipv4_forwarding";

    const auto evalRfc = evaluator.evaluate("", "p0", rfcCandidate);
    ROCLINT_CHECK(evalRfc.verdict == CitationVerdict::Verified);
    ROCLINT_CHECK(evalRfc.assignedStatus == EvidenceStatus::Supported);
    ROCLINT_CHECK(evalRfc.verifiedConstraint.evidenceStatus == EvidenceStatus::Supported);

    // 2. Missing authority reference
    ScopedConstraint noAuthCandidate = rfcCandidate;
    noAuthCandidate.authorityRef = std::nullopt;
    const auto evalNoAuth = evaluator.evaluate("", "p0", noAuthCandidate);
    ROCLINT_CHECK(evalNoAuth.verdict == CitationVerdict::Overreached);
    ROCLINT_CHECK(evalNoAuth.verifiedConstraint.origin == Origin::Model);
    ROCLINT_CHECK(evalNoAuth.verifiedConstraint.enforcement == Enforcement::Advisory);

    // 3. Fabricated authority reference
    ScopedConstraint fakeAuthCandidate = rfcCandidate;
    fakeAuthCandidate.authorityRef = "InventedProtocolStandard:v9";
    const auto evalFake = evaluator.evaluate("", "p0", fakeAuthCandidate);
    ROCLINT_CHECK(evalFake.verdict == CitationVerdict::Fabricated);
    ROCLINT_CHECK(evalFake.assignedStatus == EvidenceStatus::Rejected);
}

void testModelHardEnforcementDemotion() {
    CitationEvaluator evaluator;
    
    ScopedConstraint modelCandidate;
    modelCandidate.id = "c_model_hard";
    modelCandidate.rule = "Use blue neon glowing lines";
    modelCandidate.origin = Origin::Model;
    modelCandidate.derivation = Derivation::Assumed;
    modelCandidate.enforcement = Enforcement::Hard; // Model tries to make its suggestion Hard

    const auto eval = evaluator.evaluate("some prompt", "p0", modelCandidate);
    ROCLINT_CHECK(eval.verdict == CitationVerdict::Overreached);
    ROCLINT_CHECK(eval.verifiedConstraint.enforcement == Enforcement::Advisory);
    ROCLINT_CHECK(eval.verifiedConstraint.evidenceStatus == EvidenceStatus::Unverified);
}

void testBatchEvaluation() {
    CitationEvaluator evaluator;
    const std::string prompt = "Build a fast Layer 3 simulator.";

    std::vector<ScopedConstraint> batch;
    
    ScopedConstraint c1;
    c1.id = "c1";
    c1.rule = "Layer 3 simulator";
    c1.origin = Origin::User;
    c1.derivation = Derivation::Direct;
    c1.evidence = {SourceSpan{"p0", prompt.find("Layer 3"), prompt.find("Layer 3") + 7}};
    batch.push_back(c1);

    ScopedConstraint c2;
    c2.id = "c2";
    c2.rule = "Must use Vue 3 with Pinia";
    c2.origin = Origin::User; // Laundering attempt!
    c2.derivation = Derivation::Direct;
    c2.evidence = {SourceSpan{"p0", prompt.find("fast"), prompt.find("fast") + 4}};
    batch.push_back(c2);

    const auto results = evaluator.evaluateBatch(prompt, "p0", batch);
    ROCLINT_CHECK(results.size() == 2);
    ROCLINT_CHECK(results[0].verdict == CitationVerdict::Verified);
    ROCLINT_CHECK(results[1].verdict == CitationVerdict::Overreached);
}

} // namespace

int main() {
    std::cout << "[RUNNING] test_citation_evaluator...\n";
    testCitationVerdictEnumsAndJson();
    testSourceSpanLaunderingDetection();
    testLegitimateUserConstraintVerification();
    testNegationMismatchDetection();
    testInvalidAndUtf8SlicedSpans();
    testDomainAuthorityVerification();
    testModelHardEnforcementDemotion();
    testBatchEvaluation();
    std::cout << "[PASS] All citation evaluator tests passed successfully.\n";
    return 0;
}
