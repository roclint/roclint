#pragma once

// Deterministic policy for already-settled requirements.
//
// The product requirement is narrow and deliberate: an S1 RULE_CONFLICT signal
// alone is not evidence of a real contradiction. When the user's text already
// states exact values or units and the deterministic extractor found no explicit
// conflict, the automatic route must keep the original instead of asking the user
// to redefine a value they already supplied. This is not a quality heuristic and
// never invents an answer; the explicit interview/review path remains available.

#include "roclint/analysis/PromptTriage.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace roclint::core::intent {

inline constexpr const char* kSettledRequirementPolicyVersion = "settled_requirement.v1";

// Numeric values with a unit or percent. The token is kept verbatim so a rewrite
// that silently drops or changes it can be refused.
[[nodiscard]] inline std::vector<std::string> exactRequirementTokens(std::string_view text) {
    static const std::regex quantity(
        R"(([0-9]+(?:\.[0-9]+)?\s*(?:ms\b|milliseconds?\b|seconds?\b|secs?\b|kb\b|kib\b|mb\b|mib\b|gb\b|gib\b|%)))",
        std::regex::icase);
    const std::string source(text);
    std::vector<std::string> tokens;
    for (std::sregex_iterator it(source.begin(), source.end(), quantity), end; it != end; ++it) {
        std::string token = it->str();
        while (!token.empty() && token.front() == ' ') token.erase(token.begin());
        while (!token.empty() && token.back() == ' ') token.pop_back();
        if (std::find(tokens.begin(), tokens.end(), token) == tokens.end()) {
            tokens.push_back(std::move(token));
        }
    }
    return tokens;
}

[[nodiscard]] inline bool hasSettledExactValues(std::string_view text) {
    return !exactRequirementTokens(text).empty();
}

// True only when all three conditions hold:
//  - the automatic route classified this prompt as Clarify;
//  - the only head at or above its threshold is RULE_CONFLICT (Action, Target and
//    Rule are below theirs), so the routing signal is a suspicion, not evidence;
//  - the prompt contains exact values/units and the deterministic extractor found
//    no explicit contradiction.
// Explicit review and any deterministic conflict bypass this policy.
[[nodiscard]] inline bool shouldKeepOriginalForRuleConflictOnly(
    const analysis::PromptTriage& triage,
    std::string_view promptText,
    bool deterministicExplicitConflict,
    const analysis::TriagePolicy& policy = analysis::TriagePolicy::defaults()) {
    if (deterministicExplicitConflict) return false;
    if (!hasSettledExactValues(promptText)) return false;
    const auto& scores = triage.categoryScores;
    if (scores[0] >= policy.thresholds[0]) return false;
    if (scores[1] >= policy.thresholds[1]) return false;
    if (scores[2] >= policy.thresholds[2]) return false;
    return scores[3] >= policy.thresholds[3];
}

// Exact tokens from the original that are absent from the candidate. Comparison is
// whitespace-insensitive within one token (e.g. "500 ms" == "500ms").
[[nodiscard]] inline std::vector<std::string> droppedExactRequirements(std::string_view original,
                                                                      std::string_view candidate) {
    const auto compact = [](std::string value) {
        value.erase(std::remove_if(value.begin(), value.end(),
                                   [](unsigned char c) { return std::isspace(c) != 0; }),
                    value.end());
        for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return value;
    };
    const std::string compactCandidate = compact(std::string(candidate));
    std::vector<std::string> dropped;
    for (const auto& token : exactRequirementTokens(original)) {
        if (compactCandidate.find(compact(token)) == std::string::npos) {
            dropped.push_back(token);
        }
    }
    return dropped;
}

// A rewrite must remain a prompt rather than an instruction about the prompt.
// Conservative: only an imperative meta-verb at the start, in wording that is not
// already the user's own opening, counts.
[[nodiscard]] inline bool looksLikeMetaInstruction(std::string_view original, std::string_view candidate) {
    const auto lower = [](std::string_view text) {
        std::string result;
        result.reserve(text.size());
        for (const char c : text) {
            result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        return result;
    };
    static constexpr std::string_view kVerbs[] = {
        "determine ", "identify ", "clarify ", "ask ", "specify ", "decide ",
        "choose ", "figure out ", "gather ", "collect ", "find out "
    };
    static constexpr std::string_view kMetaMarkers[] = {
        "the specific", "the user", "the requirements", "the desired", "the context",
        "what they", "their preferences", "the intent"
    };
    const std::string output = lower(candidate);
    const std::string source = lower(original);
    bool startsWithMetaVerb = false;
    for (const auto verb : kVerbs) {
        if (output.starts_with(verb)) {
            startsWithMetaVerb = true;
            break;
        }
    }
    if (!startsWithMetaVerb) return false;
    for (const auto verb : kVerbs) {
        if (source.starts_with(verb)) return false;
    }
    for (const auto marker : kMetaMarkers) {
        if (output.find(marker) != std::string::npos) return true;
    }
    return false;
}

} // namespace roclint::core::intent
