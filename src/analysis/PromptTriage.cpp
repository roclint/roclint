#include "roclint/analysis/PromptTriage.hpp"
#include "roclint/core/crypto/Sha256.hpp"
#include "roclint/core/prompt_state/TextNormalization.hpp"
#include "roclint/core/schema/SchemaVisitor.hpp"

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
// Only a complete, unconditional closing directive can override all four heads.
DecisionOwnership decisionOwnership(std::string_view prompt) {
    std::string normalized;
    normalized.reserve(prompt.size());
    for (std::size_t i = 0; i < prompt.size(); ++i) {
        char c = prompt[i];
        if (prompt.substr(i).starts_with("’")) {
            c = '\'';
            i += std::string_view("’").size() - 1;
        }
        if (c == ' ' || c == '\t' || c == '\n') {
            if (!normalized.empty() && normalized.back() != ' ') normalized.push_back(' ');
        } else {
            normalized.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
        }
    }
    std::string_view text(normalized);
    if (text.ends_with(' ')) text.remove_suffix(1);
    if (text.ends_with('.') || text.ends_with('!')) text.remove_suffix(1);
    const auto boundary = text.find_last_of(".!?");
    auto directive = text.substr(boundary == text.npos ? 0 : boundary + 1);
    if (directive.starts_with(' ')) directive.remove_prefix(1);

    constexpr std::string_view reserved[]{"i'll decide", "i will decide", "my call", "it's my call",
        "hold for my decision"};
    constexpr std::string_view delegated[]{"you decide", "your call", "it's your call", "handle it yourself"};
    const auto has = [](const auto& cues, auto predicate) {
        return std::any_of(std::begin(cues), std::end(cues), predicate);
    };
    const auto matches = [directive](auto cue) { return directive == cue; };
    // Permit a short named object ("you choose the layout") or subject
    // ("the layout is my call"). Leave qualifications and mixed clauses to S1.
    const auto simpleSubject = [](std::string_view phrase) {
        if (!phrase.starts_with("the ")) return false;
        phrase.remove_prefix(4);
        unsigned words = 0;
        while (!phrase.empty()) {
            const auto end = phrase.find(' ');
            const auto word = phrase.substr(0, end);
            if (word.empty() || ++words > 3 ||
                !std::all_of(word.begin(), word.end(), [](char c) { return c >= 'a' && c <= 'z'; }) ||
                word == "if" || word == "unless" || word == "and" || word == "or" || word == "when")
                return false;
            if (end == phrase.npos) break;
            phrase.remove_prefix(end + 1);
        }
        return words > 0;
    };
    const auto verb = [&](std::string_view prefix) {
        return directive.starts_with(prefix) && simpleSubject(directive.substr(prefix.size()));
    };
    const auto call = [&](std::string_view suffix) {
        return directive.ends_with(suffix) && simpleSubject(directive.substr(0, directive.size() - suffix.size()));
    };
    constexpr std::string_view reserveVerbs[]{"i'll decide ", "i will decide ", "i'll choose ",
        "i will choose ", "i'll select ", "i will select "};
    constexpr std::string_view delegateVerbs[]{"you decide ", "you choose ", "you select ", "you may choose "};
    const bool reserves = has(reserved, matches) || call(" is my call") || has(reserveVerbs, verb);
    const bool delegates = has(delegated, matches) || call(" is your call") || has(delegateVerbs, verb);
    const auto earlier = text.substr(0, boundary == text.npos ? 0 : boundary);
    const auto mentioned = [earlier](auto cue) { return earlier.find(cue) != earlier.npos; };
    if (reserves && !has(delegated, mentioned) && !has(delegateVerbs, mentioned)) return DecisionOwnership::Reserved;
    if (delegates && !has(reserved, mentioned) && !has(reserveVerbs, mentioned)) return DecisionOwnership::Delegated;
    return DecisionOwnership::Unspecified;
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

const char* toString(PromptTriageRoute route) noexcept {
    switch (route) {
        case PromptTriageRoute::Original:       return "ORIGINAL";
        case PromptTriageRoute::Clarify:        return "CLARIFY";
        case PromptTriageRoute::UncertainBuffer: return "UNCERTAIN_BUFFER";
    }
    return "UNKNOWN";
}

std::string computePromptTriageHash(const PromptTriage& triage) {
    const char* domain = (triage.hashVersion == TriageHashVersion::V1)
        ? "roclint.prompt_triage.v1"
        : triage.hashVersion == TriageHashVersion::V3 ? "roclint.prompt_triage.v3" : "roclint.prompt_triage.v2";
    crypto::CanonicalHasher hash(domain);
    if (triage.hashVersion == TriageHashVersion::V1) {
        // V1 legacy sequence: threshold was baked in right before latencyMs
        hash.field(triage.sessionId).field(triage.revisionId).field(triage.revisionTextHash).number(triage.stateVersion)
            .field(triage.requestGroupId).field(triage.attemptId).field(triage.provider).field(triage.modelId)
            .field(triage.modelRevision).field(triage.weightsSha256).field(triage.configurationId).field(triage.templateId)
            .number(static_cast<std::uint64_t>(triage.execution));
        for (const auto score : triage.categoryScores) hashDouble(hash, score);
        hashDouble(hash, triage.ambiguityScore);
        hash.number(static_cast<std::uint64_t>(triage.primaryCategory));
        hashDouble(hash, kPromptTriageThreshold);
        hashDouble(hash, triage.latencyMs);
    } else {
        // V2 single-source-of-truth visitor: iterates fields automatically from struct definition
        schema::HashVisitor visitor(hash);
        triage.visitEvidence(visitor);
    }
    return hash.finalizeHex();
}

Status validatePromptTriage(const PromptTriage& triage) {
    if (triage.hashVersion == TriageHashVersion::V3) {
        if (triage.ownership != DecisionOwnership::Reserved && triage.ownership != DecisionOwnership::Delegated)
            return invalid("Invalid sealed ownership cue");
    } else if ((triage.hashVersion != TriageHashVersion::V1 && triage.hashVersion != TriageHashVersion::V2) ||
               triage.ownership != DecisionOwnership::Unspecified) {
        return invalid("Invalid triage evidence version or ownership cue");
    }
    for (const auto& value : {triage.sessionId, triage.revisionId, triage.requestGroupId, triage.attemptId,
                             triage.modelId, triage.modelRevision, triage.configurationId})
        if (!token(value)) return invalid("Prompt triage is missing a bounded identity/binding");
    if (triage.provider != "julia_local" ||
        (triage.templateId != kPromptTriageTemplateId && triage.templateId != "julia.prompt_triage.categories.v2" &&
         triage.templateId != "julia.prompt_triage.categories.v1") ||
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
    if (triage.hashVersion == TriageHashVersion::V2 || triage.hashVersion == TriageHashVersion::V3) {
        triage.ownership = decisionOwnership(frozenPrompt);
        triage.hashVersion = triage.ownership == DecisionOwnership::Unspecified ? TriageHashVersion::V2 : TriageHashVersion::V3;
    }
    triage.primaryCategory = static_cast<PromptCategory>(index);
    triage.ambiguityScore = triage.categoryScores[index];
    triage.triageHash = computePromptTriageHash(triage);
    triage.id = "triage_" + triage.triageHash;
    if (const auto status = validatePromptTriage(triage); !status.ok()) return status.error();
    return triage;
}

Result<PromptTriageRoute> routePromptTriage(const PromptTriage& triage) {
    return routePromptTriage(triage, TriagePolicy::defaults());
}

Result<PromptTriageRoute> routePromptTriage(const PromptTriage& triage, const TriagePolicy& policy) {
    if (const auto status = validatePromptTriage(triage); !status.ok()) return status.error();
    if (triage.ownership == DecisionOwnership::Reserved) return PromptTriageRoute::Clarify;
    if (triage.ownership == DecisionOwnership::Delegated) return PromptTriageRoute::Original;
    bool inDeadBand = false;
    for (std::size_t i = 0; i < triage.categoryScores.size(); ++i) {
        const double score = triage.categoryScores[i];
        const double thr   = policy.thresholds[i];
        if (score >= thr)
            return PromptTriageRoute::Clarify;
        if (score >= thr - policy.uncertaintyDelta)
            inDeadBand = true;
    }
    return inDeadBand ? PromptTriageRoute::UncertainBuffer : PromptTriageRoute::Original;
}

} // namespace roclint::core::analysis
