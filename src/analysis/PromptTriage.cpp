#include "roclint/analysis/PromptTriage.hpp"
#include "roclint/core/crypto/Sha256.hpp"
#include "roclint/core/prompt_state/TextNormalization.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace roclint::core::analysis {
namespace {
Status invalid(const char* reason) { return makeError(ErrorCode::ValidationFailed, reason); }
bool token(std::string_view value) {
    return !value.empty() && value.size() <= 256 && std::all_of(value.begin(), value.end(), [](unsigned char c) { return c > 0x20 && c < 0x7f; });
}
bool digest(std::string_view value) {
    const auto normalized = crypto::normalizeSha256Digest(value);
    return normalized.ok() && normalized.value() == value;
}
bool scoresValid(const PromptTriage& triage) {
    return std::all_of(triage.categoryScores.begin(), triage.categoryScores.end(), [](double score) {
        return std::isfinite(score) && score >= 0 && score <= 1;
    });
}
std::size_t primary(const PromptTriage& triage) {
    return static_cast<std::size_t>(std::max_element(triage.categoryScores.begin(), triage.categoryScores.end()) - triage.categoryScores.begin());
}
void hashDouble(crypto::CanonicalHasher& hash, double value) { hash.number(std::bit_cast<std::uint64_t>(value == 0 ? 0.0 : value)); }
}

const char* toString(PromptCategory category) noexcept {
    switch (category) {
        case PromptCategory::Action: return "ACTION";
        case PromptCategory::Target: return "TARGET";
        case PromptCategory::Rule: return "RULE";
        case PromptCategory::RuleConflict: return "RULE_CONFLICT";
    }
    return "UNKNOWN";
}

std::string computePromptTriageHash(const PromptTriage& triage) {
    crypto::CanonicalHasher hash("roclint.prompt_triage.v1");
    hash.field(triage.sessionId).field(triage.revisionId).field(triage.revisionTextHash).number(triage.stateVersion)
        .field(triage.requestGroupId).field(triage.attemptId).field(triage.provider).field(triage.modelId)
        .field(triage.modelRevision).field(triage.weightsSha256).field(triage.configurationId).field(triage.templateId)
        .number(static_cast<std::uint64_t>(triage.execution));
    for (const auto score : triage.categoryScores) hashDouble(hash, score);
    hashDouble(hash, triage.ambiguityScore);
    hash.number(static_cast<std::uint64_t>(triage.primaryCategory));
    hashDouble(hash, kPromptTriageThreshold);
    hashDouble(hash, triage.latencyMs);
    return hash.finalizeHex();
}

Status validatePromptTriage(const PromptTriage& triage) {
    for (const auto& value : {triage.sessionId, triage.revisionId, triage.requestGroupId, triage.attemptId,
                             triage.modelId, triage.modelRevision, triage.configurationId})
        if (!token(value)) return invalid("Prompt triage is missing a bounded identity/binding");
    if (triage.provider != "julia_local" ||
        (triage.templateId != kPromptTriageTemplateId && triage.templateId != "julia.prompt_triage.categories.v1") ||
        triage.stateVersion == 0 || triage.stateVersion > 9007199254740991ULL ||
        !digest(triage.revisionTextHash) || !digest(triage.weightsSha256)) return invalid("Prompt triage provenance is invalid");
    if (triage.execution != TriageExecution::Model && triage.execution != TriageExecution::Mock)
        return invalid("An unexecuted classifier cannot establish a clear route");
#if !defined(ROCLINT_ENABLE_TEST_MOCKS)
    if (triage.execution == TriageExecution::Mock) return invalid("Mock triage cannot enter production state");
#endif
    if (!scoresValid(triage) || !std::isfinite(triage.latencyMs) || triage.latencyMs < 0)
        return invalid("Prompt triage has invalid model scores or timing");
    const auto index = primary(triage);
    if (triage.ambiguityScore != triage.categoryScores[index] || triage.primaryCategory != static_cast<PromptCategory>(index))
        return invalid("Prompt triage derived fields differ from its model head scores");
    const auto hash = computePromptTriageHash(triage);
    if (triage.triageHash != hash || triage.id != "triage_" + hash) return invalid("Prompt triage hash does not match its contents");
    return Status::success();
}

Result<PromptTriage> sealPromptTriage(PromptTriage triage, std::string_view frozenPrompt) {
    const bool blank = std::all_of(frozenPrompt.begin(), frozenPrompt.end(), [](char c) { return c == ' ' || c == '\n' || c == '\t'; });
    if (blank || frozenPrompt.size() > 64 * 1024 || !hasCanonicalLineEndings(frozenPrompt) ||
        triage.revisionTextHash != crypto::Sha256::hexDigest(frozenPrompt)) return invalid("Prompt triage input differs from the bounded frozen revision").error();
    if (!scoresValid(triage)) return invalid("Prompt triage contains nonfinite or out-of-range model scores").error();
    const auto index = primary(triage);
    triage.primaryCategory = static_cast<PromptCategory>(index);
    triage.ambiguityScore = triage.categoryScores[index];
    triage.triageHash = computePromptTriageHash(triage);
    triage.id = "triage_" + triage.triageHash;
    if (const auto status = validatePromptTriage(triage); !status.ok()) return status.error();
    return triage;
}

Result<PromptTriageRoute> routePromptTriage(const PromptTriage& triage) {
    if (const auto status = validatePromptTriage(triage); !status.ok()) return status.error();
    return triage.ambiguityScore < kPromptTriageThreshold ? PromptTriageRoute::Original : PromptTriageRoute::Clarify;
}

} // namespace roclint::core::analysis
