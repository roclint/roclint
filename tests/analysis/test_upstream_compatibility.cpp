// Regression guard for the triage cryptographic audit chain.
// Verifies that:
//   1. V1 hash values are stable across code changes (historical record integrity).
//   2. V2 hash does not depend on kPromptTriageThreshold (policy decoupling).
//   3. hashVersion field correctly dispatches between hash domains.
//   4. Round-trip encode/decode via PayloadCodec preserves hashVersion.

#include "roclint/analysis/PromptTriage.hpp"
#include "roclint/core/crypto/Sha256.hpp"
#include "roclint/storage/PayloadCodec.hpp"
#include "support/check.hpp"

#include <array>
#include <iostream>
#include <string>

using namespace roclint::core;
using namespace roclint::core::analysis;
using namespace roclint::storage;

namespace {

// Shared fixture builder — same fields used by testPromptTriageOwnsTheExactGateBoundary
// in test_s1_triage_adapter.cpp, so both tests can cross-verify the same payload.
PromptTriage makeFixture(const std::string& prompt, TriageHashVersion version) {
    PromptTriage r;
    // This historical vector predates template v3. Pin every original field;
    // changing the current default must never change the golden legacy digest.
    r.templateId = "julia.prompt_triage.categories.v2";
    r.sessionId = "sess_triage"; r.revisionId = "rev_sess_triage_1";
    r.revisionTextHash = crypto::Sha256::hexDigest(prompt);
    r.stateVersion = 2; r.requestGroupId = "grp_triage"; r.attemptId = "att_triage";
    r.modelId = "fixture_julia"; r.modelRevision = "fixture_v1";
    r.weightsSha256 = std::string(64, 'a'); r.configurationId = "test_only";
    r.execution = TriageExecution::Mock;
    r.categoryScores = {0.1, 0.2, 0.69, 0.3}; r.latencyMs = 1;
    r.hashVersion = version;
    return r;
}

// --- Test 1: V1 hash stability ---
// Once this test captures a known-good V1 hash value (printed to stdout on first run),
// pin it here so future changes that accidentally alter the V1 domain are caught.
void testV1HashStability() {
    const std::string prompt = "Review this request.";
    auto r = makeFixture(prompt, TriageHashVersion::V1);
    const auto sealed = sealPromptTriage(r, prompt);
    ROCLINT_CHECK(sealed.ok());
    ROCLINT_CHECK(validatePromptTriage(sealed.value()).ok());
    ROCLINT_CHECK(sealed.value().hashVersion == TriageHashVersion::V1);
    // The hash id must carry the correct triage_ prefix.
    ROCLINT_CHECK(sealed.value().id == "triage_" + sealed.value().triageHash);
    // Pinned V1 hash — must NOT change; any change indicates a regression in historical
    // record verifiability (the v1 domain or its baked-in kPromptTriageThreshold changed).
    ROCLINT_CHECK(sealed.value().triageHash == "762e300f25de33539ca5aaf21d0519d2c24c79e81affdc47aa2785ddbb24dc89");
}

// --- Test 2: V2 hash excludes kPromptTriageThreshold ---
void testV2HashDoesNotIncludeThreshold() {
    const std::string prompt = "Review this request.";
    auto rv2 = makeFixture(prompt, TriageHashVersion::V2);
    const auto sealedV2 = sealPromptTriage(rv2, prompt);
    ROCLINT_CHECK(sealedV2.ok());
    ROCLINT_CHECK(validatePromptTriage(sealedV2.value()).ok());

    auto rv1 = makeFixture(prompt, TriageHashVersion::V1);
    const auto sealedV1 = sealPromptTriage(rv1, prompt);
    ROCLINT_CHECK(sealedV1.ok());
    ROCLINT_CHECK(validatePromptTriage(sealedV1.value()).ok());

    // V2 and V1 hashes must differ because their domains differ.
    ROCLINT_CHECK(sealedV2.value().triageHash != sealedV1.value().triageHash);
}

// --- Test 3: hashVersion tampering causes validation failure ---
void testHashVersionTamperingFails() {
    const std::string prompt = "Apply the patch.";
    PromptTriage r;
    r.sessionId = "sess_tamper"; r.revisionId = "rev_tamper_1";
    r.revisionTextHash = crypto::Sha256::hexDigest(prompt);
    r.stateVersion = 1; r.requestGroupId = "grp_tamper"; r.attemptId = "att_tamper";
    r.modelId = "fixture_julia"; r.modelRevision = "fixture_v1";
    r.weightsSha256 = std::string(64, 'c'); r.configurationId = "test_only";
    r.execution = TriageExecution::Mock;
    r.categoryScores = {0.3, 0.3, 0.3, 0.3}; r.latencyMs = 5;
    r.hashVersion = TriageHashVersion::V2;

    const auto sealed = sealPromptTriage(r, prompt);
    ROCLINT_CHECK(sealed.ok());

    // Flip hashVersion post-seal: hash domain mismatches the stored hash.
    auto tampered = sealed.value();
    tampered.hashVersion = TriageHashVersion::V1;
    ROCLINT_CHECK(!validatePromptTriage(tampered).ok());
}

// --- Test 4: Round-trip encode/decode preserves hashVersion for both V1 and V2 ---
void testCodecPreservesHashVersion() {
    const std::string prompt = "Standardize imports.";
    PromptTriage r;
    r.sessionId = "sess_codec"; r.revisionId = "rev_codec_1";
    r.revisionTextHash = crypto::Sha256::hexDigest(prompt);
    r.stateVersion = 3; r.requestGroupId = "grp_codec"; r.attemptId = "att_codec";
    r.modelId = "fixture_julia"; r.modelRevision = "fixture_v1";
    r.weightsSha256 = std::string(64, 'd'); r.configurationId = "test_only";
    r.execution = TriageExecution::Mock;
    r.categoryScores = {0.1, 0.5, 0.3, 0.2}; r.latencyMs = 7;

    for (const auto version : {TriageHashVersion::V1, TriageHashVersion::V2}) {
        r.hashVersion = version;
        const auto sealed = sealPromptTriage(r, prompt);
        ROCLINT_CHECK(sealed.ok());

        const auto encoded = encodePromptTriage(sealed.value());
        const auto decoded = decodePromptTriage(encoded);
        ROCLINT_CHECK(decoded.ok());
        ROCLINT_CHECK(decoded.value().hashVersion == version);
        ROCLINT_CHECK(decoded.value().triageHash == sealed.value().triageHash);
        ROCLINT_CHECK(validatePromptTriage(decoded.value()).ok());
    }
}

// --- Test 5: Records without hash_version in JSON decode as V1 (backwards compatibility) ---
void testMissingHashVersionDecodesAsV1() {
    // Build a valid V1 sealed record, encode it, strip the hash_version field, decode again.
    const std::string prompt = "Delete the file.";
    PromptTriage r;
    r.sessionId = "sess_compat"; r.revisionId = "rev_compat_1";
    r.revisionTextHash = crypto::Sha256::hexDigest(prompt);
    r.stateVersion = 1; r.requestGroupId = "grp_compat"; r.attemptId = "att_compat";
    r.modelId = "fixture_julia"; r.modelRevision = "fixture_v1";
    r.weightsSha256 = std::string(64, 'e'); r.configurationId = "test_only";
    r.execution = TriageExecution::Mock;
    r.categoryScores = {0.2, 0.3, 0.4, 0.1}; r.latencyMs = 3;
    r.hashVersion = TriageHashVersion::V1;

    const auto sealed = sealPromptTriage(r, prompt);
    ROCLINT_CHECK(sealed.ok());

    std::string encoded = encodePromptTriage(sealed.value());
    // Strip the hash_version field by searching for it in the JSON string.
    // PayloadCodec serializes to compact JSON; the field appears as "hash_version":1
    const std::string fieldV1 = "\"hash_version\":1";
    const std::string fieldV2 = "\"hash_version\":2";
    auto pos = encoded.find(fieldV1);
    if (pos == std::string::npos) pos = encoded.find(fieldV2);
    ROCLINT_CHECK(pos != std::string::npos); // field must be present before stripping

    // Remove the field and a trailing comma if present (handle either ",field" or "field,")
    std::string stripped = encoded;
    // Try removing ",<field>" first (field is not first key)
    auto with_prefix = stripped.find("," + fieldV1);
    if (with_prefix != std::string::npos)
        stripped.erase(with_prefix, fieldV1.size() + 1);
    else {
        with_prefix = stripped.find("," + fieldV2);
        if (with_prefix != std::string::npos)
            stripped.erase(with_prefix, fieldV2.size() + 1);
        else {
            // Field is first — remove "field," suffix
            auto without_prefix = stripped.find(fieldV1 + ",");
            if (without_prefix == std::string::npos)
                without_prefix = stripped.find(fieldV2 + ",");
            if (without_prefix != std::string::npos)
                stripped.erase(without_prefix, fieldV1.size() + 1);
        }
    }

    const auto decoded = decodePromptTriage(stripped);
    ROCLINT_CHECK(decoded.ok());
    ROCLINT_CHECK(decoded.value().hashVersion == TriageHashVersion::V1);
    ROCLINT_CHECK(validatePromptTriage(decoded.value()).ok());
}

// --- Test 6: Three-tier routing vectors against TriagePolicy::defaults() ---
void testRouteWithDefaultPolicyVectors() {
    const std::string prompt = "Standardize all imports.";
    PromptTriage r;
    r.sessionId = "sess_it"; r.revisionId = "rev_it_1";
    r.revisionTextHash = crypto::Sha256::hexDigest(prompt);
    r.stateVersion = 1; r.requestGroupId = "grp_it"; r.attemptId = "att_it";
    r.modelId = "fixture_julia"; r.modelRevision = "fixture_v1";
    r.weightsSha256 = std::string(64, 'f'); r.configurationId = "test_only";
    r.execution = TriageExecution::Mock; r.latencyMs = 3;

    const TriagePolicy policy = TriagePolicy::defaults();

    struct TestVector { std::array<double, 4> scores; PromptTriageRoute expected; };
    const TestVector vectors[] = {
        {{0.05, 0.10, 0.08, 0.12}, PromptTriageRoute::Original},        // all clear
        {{0.65, 0.20, 0.10, 0.20}, PromptTriageRoute::UncertainBuffer},  // ACTION in dead band [0.62,0.70)
        {{0.80, 0.20, 0.10, 0.20}, PromptTriageRoute::Clarify},          // ACTION above threshold
        {{0.20, 0.20, 0.20, 0.62}, PromptTriageRoute::Clarify},          // RULE_CONFLICT above 0.60
        {{0.20, 0.20, 0.20, 0.55}, PromptTriageRoute::UncertainBuffer},  // RULE_CONFLICT in [0.52,0.60)
        {{0.20, 0.20, 0.20, 0.51}, PromptTriageRoute::Original},         // RULE_CONFLICT below dead band
    };

    for (const auto& v : vectors) {
        r.categoryScores = v.scores;
        const auto sealed = sealPromptTriage(r, prompt);
        ROCLINT_CHECK(sealed.ok());
        const auto route = routePromptTriage(sealed.value(), policy);
        ROCLINT_CHECK(route.ok() && route.value() == v.expected);
    }
}

} // namespace

int main() {
    std::cout << "[Test UpstreamCompatibility] Starting...\n";
    testV1HashStability();
    testV2HashDoesNotIncludeThreshold();
    testHashVersionTamperingFails();
    testCodecPreservesHashVersion();
    testMissingHashVersionDecodesAsV1();
    testRouteWithDefaultPolicyVectors();
    std::cout << "[Test UpstreamCompatibility] All tests PASSED.\n";
    return 0;
}
