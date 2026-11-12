#include "roclint/core/scoped_constraint.hpp"
#include "roclint/core/validation_result.hpp"
#include "roclint/analysis/SourceSpan.hpp"
#include "support/check.hpp"

#include <iostream>
#include <string>
#include <vector>

using namespace roclint::core;

namespace {

void testEnumRoundTrips() {
    // Origin
    ROCLINT_CHECK(parseOrigin("User").value() == Origin::User);
    ROCLINT_CHECK(parseOrigin("Domain").value() == Origin::Domain);
    ROCLINT_CHECK(parseOrigin("Environment").value() == Origin::Environment);
    ROCLINT_CHECK(parseOrigin("Model").value() == Origin::Model);
    ROCLINT_CHECK(!parseOrigin("Invalid").ok());
    ROCLINT_CHECK(std::string(toString(Origin::User)) == "User");
    ROCLINT_CHECK(std::string(toString(Origin::Domain)) == "Domain");
    ROCLINT_CHECK(std::string(toString(Origin::Environment)) == "Environment");
    ROCLINT_CHECK(std::string(toString(Origin::Model)) == "Model");

    // Derivation
    ROCLINT_CHECK(parseDerivation("Direct").value() == Derivation::Direct);
    ROCLINT_CHECK(parseDerivation("Paraphrased").value() == Derivation::Paraphrased);
    ROCLINT_CHECK(parseDerivation("Entailed").value() == Derivation::Entailed);
    ROCLINT_CHECK(parseDerivation("Assumed").value() == Derivation::Assumed);
    ROCLINT_CHECK(!parseDerivation("Unknown").ok());
    ROCLINT_CHECK(std::string(toString(Derivation::Direct)) == "Direct");
    ROCLINT_CHECK(std::string(toString(Derivation::Paraphrased)) == "Paraphrased");
    ROCLINT_CHECK(std::string(toString(Derivation::Entailed)) == "Entailed");
    ROCLINT_CHECK(std::string(toString(Derivation::Assumed)) == "Assumed");

    // Enforcement
    ROCLINT_CHECK(parseEnforcement("Hard").value() == Enforcement::Hard);
    ROCLINT_CHECK(parseEnforcement("Conditional").value() == Enforcement::Conditional);
    ROCLINT_CHECK(parseEnforcement("Advisory").value() == Enforcement::Advisory);
    ROCLINT_CHECK(!parseEnforcement("Optional").ok());
    ROCLINT_CHECK(std::string(toString(Enforcement::Hard)) == "Hard");
    ROCLINT_CHECK(std::string(toString(Enforcement::Conditional)) == "Conditional");
    ROCLINT_CHECK(std::string(toString(Enforcement::Advisory)) == "Advisory");

    // Polarity
    ROCLINT_CHECK(parsePolarity("Require").value() == Polarity::Require);
    ROCLINT_CHECK(parsePolarity("Prohibit").value() == Polarity::Prohibit);
    ROCLINT_CHECK(!parsePolarity("Ban").ok());
    ROCLINT_CHECK(std::string(toString(Polarity::Require)) == "Require");
    ROCLINT_CHECK(std::string(toString(Polarity::Prohibit)) == "Prohibit");

    // EvidenceStatus
    ROCLINT_CHECK(parseEvidenceStatus("Unverified").value() == EvidenceStatus::Unverified);
    ROCLINT_CHECK(parseEvidenceStatus("Supported").value() == EvidenceStatus::Supported);
    ROCLINT_CHECK(parseEvidenceStatus("Rejected").value() == EvidenceStatus::Rejected);
    ROCLINT_CHECK(!parseEvidenceStatus("Verified").ok());
    ROCLINT_CHECK(std::string(toString(EvidenceStatus::Unverified)) == "Unverified");
    ROCLINT_CHECK(std::string(toString(EvidenceStatus::Supported)) == "Supported");
    ROCLINT_CHECK(std::string(toString(EvidenceStatus::Rejected)) == "Rejected");

    // ApplicabilityStatus
    ROCLINT_CHECK(parseApplicabilityStatus("Active").value() == ApplicabilityStatus::Active);
    ROCLINT_CHECK(parseApplicabilityStatus("Inactive").value() == ApplicabilityStatus::Inactive);
    ROCLINT_CHECK(parseApplicabilityStatus("Unknown").value() == ApplicabilityStatus::Unknown);
    ROCLINT_CHECK(!parseApplicabilityStatus("Pending").ok());
    ROCLINT_CHECK(std::string(toString(ApplicabilityStatus::Active)) == "Active");
    ROCLINT_CHECK(std::string(toString(ApplicabilityStatus::Inactive)) == "Inactive");
    ROCLINT_CHECK(std::string(toString(ApplicabilityStatus::Unknown)) == "Unknown");

    // ValidationOutcome
    ROCLINT_CHECK(parseValidationOutcome("Pass").value() == ValidationOutcome::Pass);
    ROCLINT_CHECK(parseValidationOutcome("Fail").value() == ValidationOutcome::Fail);
    ROCLINT_CHECK(parseValidationOutcome("Unresolved").value() == ValidationOutcome::Unresolved);
    ROCLINT_CHECK(!parseValidationOutcome("Warning").ok());
    ROCLINT_CHECK(std::string(toString(ValidationOutcome::Pass)) == "Pass");
    ROCLINT_CHECK(std::string(toString(ValidationOutcome::Fail)) == "Fail");
    ROCLINT_CHECK(std::string(toString(ValidationOutcome::Unresolved)) == "Unresolved");
}

void testScopedConstraintSerialization() {
    ScopedConstraint sc;
    sc.id = "c_001";
    sc.rule = "Forwarding decisions must adhere to destination prefix lookup";
    sc.origin = Origin::Domain;
    sc.derivation = Derivation::Entailed;
    sc.enforcement = Enforcement::Conditional;
    sc.polarity = Polarity::Require;
    sc.evidenceStatus = EvidenceStatus::Supported;
    sc.aspect = "routing";
    sc.evidence = {
        SourceSpan{"prompt_0", 10, 25},
        SourceSpan{"doc_rfc", 100, 250}
    };
    sc.appliesWhen = "depicts_ipv4_forwarding";
    sc.authorityRef = "RFC 1812 §4.2.2.1";

    const auto jsonVal = toJson(sc);
    const auto parsedRes = scopedConstraintFromJson(jsonVal);
    ROCLINT_CHECK(parsedRes.ok());
    ROCLINT_CHECK(parsedRes.value() == sc);

    // Test with optional fields null
    ScopedConstraint scMinimal;
    scMinimal.id = "c_min";
    scMinimal.rule = "Clean prose";
    const auto minJson = toJson(scMinimal);
    const auto minParsed = scopedConstraintFromJson(minJson);
    ROCLINT_CHECK(minParsed.ok());
    ROCLINT_CHECK(minParsed.value() == scMinimal);
    ROCLINT_CHECK(!minParsed.value().appliesWhen.has_value());
    ROCLINT_CHECK(!minParsed.value().authorityRef.has_value());
}

void testValidationResultSerialization() {
    ValidationResult vr;
    vr.outcome = ValidationOutcome::Fail;
    vr.violations.push_back(ValidationFinding{"c_001", "Packet routed to invalid next-hop"});
    vr.unresolved.push_back(ValidationFinding{"c_002", "Unknown if transport layer is visualized"});

    const auto jsonVal = toJson(vr);
    const auto parsedRes = validationResultFromJson(jsonVal);
    ROCLINT_CHECK(parsedRes.ok());
    ROCLINT_CHECK(parsedRes.value() == vr);
}

void testPromotionSafeguard() {
    // 1. Model assumptions cannot gain Hard authority
    ScopedConstraint modelAssumption;
    modelAssumption.origin = Origin::Model;
    modelAssumption.derivation = Derivation::Assumed;
    modelAssumption.enforcement = Enforcement::Hard;
    modelAssumption.evidenceStatus = EvidenceStatus::Supported;
    ROCLINT_CHECK(!canPromoteToEnforceable(modelAssumption));

    // 2. Unverified evidence cannot gain Hard authority
    ScopedConstraint unverifiedHard;
    unverifiedHard.origin = Origin::User;
    unverifiedHard.derivation = Derivation::Direct;
    unverifiedHard.enforcement = Enforcement::Hard;
    unverifiedHard.evidenceStatus = EvidenceStatus::Unverified;
    unverifiedHard.evidence = {SourceSpan{"p0", 0, 5}};
    ROCLINT_CHECK(!canPromoteToEnforceable(unverifiedHard));

    // 3. User origin claim cannot be accepted if derivation is Assumed
    ScopedConstraint userAssumed;
    userAssumed.origin = Origin::User;
    userAssumed.derivation = Derivation::Assumed;
    userAssumed.enforcement = Enforcement::Conditional;
    userAssumed.evidenceStatus = EvidenceStatus::Supported;
    userAssumed.evidence = {SourceSpan{"p0", 0, 5}};
    ROCLINT_CHECK(!canPromoteToEnforceable(userAssumed));

    // 4. User origin claim without evidence is rejected
    ScopedConstraint userNoEvidence;
    userNoEvidence.origin = Origin::User;
    userNoEvidence.derivation = Derivation::Direct;
    userNoEvidence.enforcement = Enforcement::Conditional;
    userNoEvidence.evidenceStatus = EvidenceStatus::Supported;
    ROCLINT_CHECK(!canPromoteToEnforceable(userNoEvidence));

    // 5. Rejected evidence can NEVER be enforced
    ScopedConstraint rejectedConstraint;
    rejectedConstraint.origin = Origin::Domain;
    rejectedConstraint.derivation = Derivation::Entailed;
    rejectedConstraint.enforcement = Enforcement::Conditional;
    rejectedConstraint.evidenceStatus = EvidenceStatus::Rejected;
    ROCLINT_CHECK(!canPromoteToEnforceable(rejectedConstraint));

    // 6. Valid user hard constraint passes promotion gate
    ScopedConstraint legitimateUserHard;
    legitimateUserHard.origin = Origin::User;
    legitimateUserHard.derivation = Derivation::Direct;
    legitimateUserHard.enforcement = Enforcement::Hard;
    legitimateUserHard.evidenceStatus = EvidenceStatus::Supported;
    legitimateUserHard.evidence = {SourceSpan{"p0", 10, 30}};
    ROCLINT_CHECK(canPromoteToEnforceable(legitimateUserHard));

    // 7. Model advisory assumption passes as advisory
    ScopedConstraint validAdvisory;
    validAdvisory.origin = Origin::Model;
    validAdvisory.derivation = Derivation::Assumed;
    validAdvisory.enforcement = Enforcement::Advisory;
    validAdvisory.evidenceStatus = EvidenceStatus::Unverified;
    ROCLINT_CHECK(canPromoteToEnforceable(validAdvisory));
}

void testSourceSpanBoundariesAndUtf8() {
    // UTF-8 test string: "Layer 3 • Сеть • 🚀"
    // "Layer 3 " = 8 bytes
    // "• " = 3 + 1 = 4 bytes
    // "Сеть" = 4 chars * 2 bytes = 8 bytes
    // " • " = 1 + 3 + 1 = 5 bytes
    // "🚀" = 4 bytes
    const std::string text = "Layer 3 • Сеть • 🚀";
    
    // Valid span covering "Сеть"
    // Begin: 12, End: 20 (8 bytes)
    analysis::SourceSpan analysisSpan{12, 20};
    ROCLINT_CHECK(analysis::isValidSpan(text, analysisSpan));
    ROCLINT_CHECK(analysis::extractSpan(text, analysisSpan).value() == "Сеть");

    // Invalid span slicing inside the 4-byte rocket emoji
    // Rocket starts at byte 25
    const std::size_t rocketOffset = text.find("🚀");
    ROCLINT_CHECK(rocketOffset != std::string::npos);
    analysis::SourceSpan slicedSpan{rocketOffset, rocketOffset + 2};
    ROCLINT_CHECK(!analysis::isValidSpan(text, slicedSpan));

    // Serialization checks on SourceSpan
    SourceSpan span{"doc", 12, 20};
    const auto spanJson = toJson(span);
    const auto parsedSpan = sourceSpanFromJson(spanJson);
    ROCLINT_CHECK(parsedSpan.ok());
    ROCLINT_CHECK(parsedSpan.value() == span);

    // Inverted range fails deserialization
    auto badJson = json::Value::object();
    badJson.set("source_id", json::Value::string("doc"));
    badJson.set("begin_byte", json::Value::number(50));
    badJson.set("end_byte", json::Value::number(10)); // end < begin
    ROCLINT_CHECK(!sourceSpanFromJson(badJson).ok());
}

void testAdversarialProvenanceFixtures() {
    // ------------------------------------------------------------------------
    // P-01: False constraint invention
    // Prompt: "Make an accurate Layer 3 animation."
    // Model produces candidate: "Display real CIDR routing tables on screen."
    // ------------------------------------------------------------------------
    ScopedConstraint p01Candidate;
    p01Candidate.id = "p01_candidate";
    p01Candidate.rule = "Display real CIDR routing tables on screen";
    p01Candidate.origin = Origin::Model;
    p01Candidate.derivation = Derivation::Assumed;
    p01Candidate.enforcement = Enforcement::Advisory; // Safe default
    p01Candidate.evidenceStatus = EvidenceStatus::Unverified;

    // Attemped laundering: model tries to claim it's User Hard
    p01Candidate.enforcement = Enforcement::Hard;
    p01Candidate.origin = Origin::User;
    ROCLINT_CHECK(!canPromoteToEnforceable(p01Candidate)); // REJECTED!

    // Reset to safe defaults
    p01Candidate.enforcement = Enforcement::Advisory;
    p01Candidate.origin = Origin::Model;
    ROCLINT_CHECK(canPromoteToEnforceable(p01Candidate)); // Stays advisory only

    // ------------------------------------------------------------------------
    // P-02: Orthogonal coexistence
    // Prompt: "Make a surreal dashboard, but with strict financial precision."
    // Visual style and data integrity govern orthogonal aspects.
    // ------------------------------------------------------------------------
    ScopedConstraint styleConstraint;
    styleConstraint.id = "p02_style";
    styleConstraint.rule = "Surreal aesthetic framing";
    styleConstraint.aspect = "presentation";
    styleConstraint.origin = Origin::User;
    styleConstraint.derivation = Derivation::Direct;
    styleConstraint.enforcement = Enforcement::Hard;
    styleConstraint.evidenceStatus = EvidenceStatus::Supported;
    styleConstraint.evidence = {SourceSpan{"p0", 7, 14}};

    ScopedConstraint dataConstraint;
    dataConstraint.id = "p02_data";
    dataConstraint.rule = "Strict financial precision to two decimal places";
    dataConstraint.aspect = "data_integrity";
    dataConstraint.origin = Origin::User;
    dataConstraint.derivation = Derivation::Direct;
    dataConstraint.enforcement = Enforcement::Hard;
    dataConstraint.evidenceStatus = EvidenceStatus::Supported;
    dataConstraint.evidence = {SourceSpan{"p0", 35, 60}};

    ROCLINT_CHECK(styleConstraint.aspect != dataConstraint.aspect);
    ROCLINT_CHECK(canPromoteToEnforceable(styleConstraint));
    ROCLINT_CHECK(canPromoteToEnforceable(dataConstraint));

    // ------------------------------------------------------------------------
    // P-03: Applicability gating
    // An RFC 1812 router forwarding rule has appliesWhen = "depicts_ipv4_forwarding".
    // ------------------------------------------------------------------------
    ScopedConstraint rfcRule;
    rfcRule.id = "p03_rfc1812";
    rfcRule.rule = "Destination IP lookup must follow longest prefix match";
    rfcRule.origin = Origin::Domain;
    rfcRule.derivation = Derivation::Entailed;
    rfcRule.enforcement = Enforcement::Conditional;
    rfcRule.evidenceStatus = EvidenceStatus::Supported;
    rfcRule.authorityRef = "RFC 1812 §4.2.2.1";
    rfcRule.appliesWhen = "depicts_ipv4_forwarding";

    ROCLINT_CHECK(rfcRule.enforcement == Enforcement::Conditional);
    ROCLINT_CHECK(rfcRule.appliesWhen.has_value());
    ROCLINT_CHECK(canPromoteToEnforceable(rfcRule));

    // ------------------------------------------------------------------------
    // P-04: Negative boundary preservation
    // Prompt: "Refactor auth, but don't touch token validation."
    // Polarity::Prohibit must be explicitly captured and enforced as Hard.
    // ------------------------------------------------------------------------
    ScopedConstraint negativeBoundary;
    negativeBoundary.id = "p04_prohibit_token";
    negativeBoundary.rule = "Do not modify token validation implementation";
    negativeBoundary.origin = Origin::User;
    negativeBoundary.derivation = Derivation::Direct;
    negativeBoundary.enforcement = Enforcement::Hard;
    negativeBoundary.polarity = Polarity::Prohibit;
    negativeBoundary.evidenceStatus = EvidenceStatus::Supported;
    negativeBoundary.evidence = {SourceSpan{"p0", 25, 48}};

    ROCLINT_CHECK(negativeBoundary.polarity == Polarity::Prohibit);
    ROCLINT_CHECK(canPromoteToEnforceable(negativeBoundary));

    // ------------------------------------------------------------------------
    // P-05: Honest unknowns
    // Prompt: "Build a fast consensus engine."
    // Raft vs Paxos is unspecified: result must be Unresolved, NOT Fail.
    // ------------------------------------------------------------------------
    ValidationResult honestUnknownResult;
    honestUnknownResult.outcome = ValidationOutcome::Unresolved;
    honestUnknownResult.unresolved.push_back(ValidationFinding{
        "consensus_protocol",
        "Protocol selection (Raft vs Paxos) is unconstrained by user specification."
    });

    ROCLINT_CHECK(honestUnknownResult.outcome == ValidationOutcome::Unresolved);
    ROCLINT_CHECK(honestUnknownResult.violations.empty());
    ROCLINT_CHECK(!honestUnknownResult.unresolved.empty());
}

} // namespace

int main() {
    std::cout << "[RUNNING] test_scoped_constraint...\n";
    testEnumRoundTrips();
    testScopedConstraintSerialization();
    testValidationResultSerialization();
    testPromotionSafeguard();
    testSourceSpanBoundariesAndUtf8();
    testAdversarialProvenanceFixtures();
    std::cout << "[PASS] All scoped constraint and validation tests passed successfully.\n";
    return 0;
}
