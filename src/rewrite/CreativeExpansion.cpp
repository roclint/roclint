#include "roclint/rewrite/CreativeExpansion.hpp"
#include "roclint/rewrite/RewriteValidator.hpp"
#include "roclint/core/constraint_reconciler.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <unordered_set>

namespace roclint::core::rewrite {

namespace {

[[nodiscard]] std::string toLowerAscii(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == 0xE2 && i + 2 < s.size()) {
            unsigned char c2 = static_cast<unsigned char>(s[i + 1]);
            unsigned char c3 = static_cast<unsigned char>(s[i + 2]);
            if (c2 == 0x80 && (c3 == 0x99 || c3 == 0x98)) {
                out.push_back('\'');
                i += 2;
                continue;
            }
        }
        if (c == '`') {
            out.push_back('\'');
        } else {
            out.push_back(static_cast<char>(std::tolower(c)));
        }
    }
    return out;
}

[[nodiscard]] bool containsCaseInsensitive(std::string_view haystack, std::string_view needle) {
    auto it = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(), needle.end(),
        [](char ch1, char ch2) {
            return std::tolower(static_cast<unsigned char>(ch1)) ==
                   std::tolower(static_cast<unsigned char>(ch2));
        });
    return it != haystack.end();
}

std::string cleanClause(std::string s) {
    while (!s.empty() && (std::isspace(static_cast<unsigned char>(s.front())) || s.front() == ',' || s.front() == ';')) {
        s.erase(s.begin());
    }
    while (!s.empty() && (std::isspace(static_cast<unsigned char>(s.back())) || s.back() == ',' || s.back() == ';' || s.back() == '.')) {
        s.pop_back();
    }
    return s;
}

std::vector<std::string> extractWords(std::string_view text) {
    const std::string norm = toLowerAscii(text);
    std::vector<std::string> words;
    std::string current;
    for (size_t i = 0; i < norm.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(norm[i]);
        if (std::isalnum(c)) {
            current.push_back(static_cast<char>(c));
        } else if (c == '\'' && !current.empty() &&
                   std::isalpha(static_cast<unsigned char>(current.back())) &&
                   i + 1 < norm.size() &&
                   std::isalpha(static_cast<unsigned char>(norm[i + 1]))) {
            current.push_back('\'');
        } else {
            if (!current.empty()) {
                words.push_back(std::move(current));
                current.clear();
            }
        }
    }
    if (!current.empty()) {
        words.push_back(std::move(current));
    }
    return words;
}

static const std::unordered_set<std::string_view> kGenericRequirementWords = {
    "write", "writes", "writing", "wrote", "written",
    "guide", "guides", "guiding", "guided",
    "create", "creates", "creating", "created",
    "make", "makes", "making", "made",
    "provide", "provides", "providing", "provided",
    "generate", "generates", "generating", "generated",
    "implement", "implements", "implementing", "implemented",
    "build", "builds", "building", "built",
    "explain", "explains", "explaining", "explained",
    "improve", "improves", "improving", "improved",
    "add", "adds", "adding", "added",
    "show", "shows", "showing", "showed", "shown",
    "use", "uses", "using", "used",
    "ensure", "ensures", "ensuring", "ensured",
    "perform", "performs", "performing", "performed",
    "update", "updates", "updating", "updated",
    "describe", "describes", "describing", "described",
    "note", "notes", "noting", "noted",
    "doc", "docs", "document", "documents", "documenting", "documented", "documentation",
    "help", "helps", "helping", "helped"
};

static const std::unordered_set<std::string_view> kRequirementStopWords = {
    "the", "a", "an", "and", "or", "for", "with", "that", "this", "from",
    "have", "has", "had", "are", "were", "was", "be", "been", "being",
    "to", "of", "in", "on", "it", "its", "as", "at", "by", "into",
    "any", "all", "do", "does", "did", "doing",
    "must", "should", "shall", "will", "would", "can", "could", "may", "might",
    "not", "no", "never", "without", "dont", "don't",
    "how", "what", "which", "who", "whom", "where", "when", "why",
    "such", "than", "then", "too", "very", "so", "just", "about", "both",
    "each", "either", "neither", "other", "some", "our", "your", "their", "my", "his", "her"
};

std::vector<std::string> extractRequirementSubstantiveWords(std::string_view text) {
    const auto raw = extractWords(text);
    std::vector<std::string> substantive;
    for (const auto& w : raw) {
        if (w.size() < 2) continue;
        if (kRequirementStopWords.contains(w)) continue;
        if (kGenericRequirementWords.contains(w)) continue;
        substantive.push_back(w);
    }
    if (substantive.empty()) {
        for (const auto& w : raw) {
            if (w.size() < 2) continue;
            if (kRequirementStopWords.contains(w)) continue;
            substantive.push_back(w);
        }
    }
    return substantive;
}

static const std::unordered_set<std::string_view> kProhibitionActionWords = {
    "modify", "modifying", "modified",
    "rewrite", "rewriting", "rewritten",
    "update", "updating", "updated",
    "change", "changing", "changed",
    "refactor", "refactoring", "refactored",
    "alter", "altering", "altered",
    "replace", "replacing", "replaced",
    "touch", "touching", "touched",
    "delete", "deleting", "deleted",
    "remove", "removing", "removed",
    "mutate", "mutating", "mutated",
    "break", "breaking", "broken",
    "edit", "editing", "edited",
    "disclose", "disclosing", "disclosed",
    "leak", "leaking", "leaked",
    "reveal", "revealing", "revealed",
    "expose", "exposing", "exposed"
};

static const std::unordered_set<std::string_view> kProhibitionControlWords = {
    "the", "a", "an", "and", "or", "for", "with", "that", "this", "from",
    "have", "has", "had", "are", "were", "was", "be", "to", "of", "in", "on",
    "it", "as", "at", "by", "into", "any", "all", "do", "does", "did",
    "must", "should", "shall", "cannot", "cant", "can't", "make", "build",
    "show", "use", "not", "dont", "don't", "never", "avoid", "no", "without",
    "won't", "wont", "shouldn't", "shouldnt", "wouldn't", "wouldnt",
    "couldn't", "couldnt", "doesn't", "doesnt", "didn't", "didnt",
    "isn't", "isnt", "aren't", "arent", "wasn't", "wasnt", "weren't", "werent",
    "haven't", "havent", "hasn't", "hasnt", "hadn't", "hadnt", "mustn't", "mustnt"
};

static const std::unordered_set<std::string_view> kProhibitionPreservationWords = {
    "preserve", "preserving", "preserved",
    "keep", "keeping", "kept",
    "leave", "leaving", "left",
    "maintain", "maintaining", "maintained",
    "retain", "retaining", "retained",
    "remain", "remaining", "remains",
    "stay", "staying", "stays",
    "unchanged", "untouched", "unmodified", "intact"
};

std::vector<std::string> extractProhibitionTargetTokens(std::string_view rule) {
    auto words = extractWords(rule);
    std::vector<std::string> targetTokens;
    for (const auto& w : words) {
        if (w.size() < 2) continue;
        if (kProhibitionControlWords.contains(w)) continue;
        if (kProhibitionActionWords.contains(w)) continue;
        if (kProhibitionPreservationWords.contains(w)) continue;
        targetTokens.push_back(w);
    }
    return targetTokens;
}

struct SentenceDecomposition {
    std::vector<std::string> positiveClauses;
    std::vector<std::string> prohibitionClauses;
};

SentenceDecomposition decomposeMixedSentences(
    std::string_view text,
    const std::vector<std::string>& knownProhibitions) {

    SentenceDecomposition result;

    std::vector<std::string> rawSentences;
    std::string current;
    for (char c : text) {
        if (c == '.' || c == '\n' || c == '!' || c == '?') {
            auto trimmed = current;
            while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) trimmed.erase(trimmed.begin());
            while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) trimmed.pop_back();
            if (!trimmed.empty()) {
                rawSentences.push_back(std::move(trimmed));
            }
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        auto trimmed = current;
        while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) trimmed.erase(trimmed.begin());
        while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) trimmed.pop_back();
        if (!trimmed.empty()) {
            rawSentences.push_back(std::move(trimmed));
        }
    }

    static const std::vector<std::string_view> kContrastiveMarkers = {
        ", but not ", ", but ", ", while leaving ", ", while ", "; ", ", except ", ", without "
    };

    auto isProhibitionText = [&](std::string_view s) -> bool {
        std::string lower = toLowerAscii(s);
        for (const auto& p : knownProhibitions) {
            if (containsCaseInsensitive(lower, toLowerAscii(p))) {
                return true;
            }
            auto targetTokens = extractProhibitionTargetTokens(p);
            if (!targetTokens.empty()) {
                bool hasAllTokens = true;
                for (const auto& tok : targetTokens) {
                    if (!containsCaseInsensitive(lower, tok)) {
                        hasAllTokens = false;
                        break;
                    }
                }
                if (hasAllTokens && (lower.find("not") != std::string::npos || lower.find("without") != std::string::npos || lower.find("never") != std::string::npos || lower.find("avoid") != std::string::npos || lower.find("untouched") != std::string::npos || lower.find("unchanged") != std::string::npos)) {
                    return true;
                }
            }
        }
        if (lower.starts_with("do not ") || lower.starts_with("don't ") ||
            lower.starts_with("dont ") || lower.starts_with("never ") ||
            lower.starts_with("avoid ") || lower.starts_with("without ") ||
            (lower.starts_with("leave ") && lower.find("untouched") != std::string::npos) ||
            (lower.starts_with("leaving ") && lower.find("untouched") != std::string::npos) ||
            ((lower.starts_with("keep ") || lower.starts_with("keeping ")) && (lower.find("unchanged") != std::string::npos || lower.find("intact") != std::string::npos))) {
            return true;
        }
        return false;
    };

    std::function<void(std::string_view)> decomposeClauseRecursive;
    decomposeClauseRecursive = [&](std::string_view clause) {
        auto trimmed = cleanClause(std::string(clause));
        if (trimmed.empty()) return;

        std::size_t bestMarkerPos = std::string::npos;
        std::size_t bestMarkerLen = 0;
        std::string_view bestMarker;
        for (const auto& marker : kContrastiveMarkers) {
            std::size_t pos = trimmed.find(marker);
            if (pos != std::string::npos) {
                if (bestMarkerPos == std::string::npos || pos < bestMarkerPos) {
                    bestMarkerPos = pos;
                    bestMarkerLen = marker.size();
                    bestMarker = marker;
                }
            }
        }

        if (bestMarkerPos == std::string::npos) {
            if (isProhibitionText(trimmed)) {
                result.prohibitionClauses.push_back(trimmed);
            } else {
                result.positiveClauses.push_back(trimmed);
            }
            return;
        }

        std::string part1 = cleanClause(trimmed.substr(0, bestMarkerPos));
        std::string remainder = cleanClause(trimmed.substr(bestMarkerPos + bestMarkerLen));

        if (!part1.empty()) {
            if (isProhibitionText(part1)) {
                result.prohibitionClauses.push_back(part1);
            } else {
                result.positiveClauses.push_back(part1);
            }
        }

        if (!remainder.empty()) {
            std::string part2;
            if (bestMarker == ", without ") {
                part2 = "without " + remainder;
            } else if (bestMarker == ", while leaving ") {
                part2 = "leave " + remainder;
            } else if (bestMarker == ", but not ") {
                part2 = "do not " + remainder;
            } else {
                part2 = remainder;
            }
            decomposeClauseRecursive(part2);
        }
    };

    for (const auto& raw : rawSentences) {
        decomposeClauseRecursive(raw);
    }

    return result;
}

} // namespace

std::vector<CreativeAlternative> CreativeExpansionEngine::expand(
    std::string_view originalPrompt,
    std::string_view baseRewrittenText,
    const std::vector<ScopedConstraint>& constraints) {

    std::vector<CreativeAlternative> alternatives;
    const std::string originalStr(originalPrompt);
    const std::string baseStr(baseRewrittenText);

    // Extract any explicit negative prohibitions that must be preserved
    std::vector<std::string> prohibitions;
    for (const auto& c : constraints) {
        if (c.polarity == Polarity::Prohibit) {
            prohibitions.push_back(c.rule);
        }
    }

    const std::string lowerOrig = toLowerAscii(originalPrompt);
    const std::string lowerBase = toLowerAscii(baseRewrittenText);

    // Helper to join multiple prohibitions cleanly
    auto formatProhibitions = [](const std::vector<std::string>& probs) -> std::string {
        if (probs.empty()) return {};
        std::string res;
        for (size_t i = 0; i < probs.size(); ++i) {
            if (i > 0) res += "; ";
            res += probs[i];
        }
        return res;
    };

    // Helper to split base string into positive requirement clauses and prohibitions
    auto decomp = decomposeMixedSentences(baseStr, prohibitions);
    std::vector<std::string> positiveSentences = decomp.positiveClauses;

    for (const auto& prob : decomp.prohibitionClauses) {
        bool alreadyPresent = false;
        auto probTokens = extractProhibitionTargetTokens(prob);
        for (const auto& existing : prohibitions) {
            if (containsCaseInsensitive(existing, prob) || containsCaseInsensitive(prob, existing)) {
                alreadyPresent = true;
                break;
            }
            auto existingTokens = extractProhibitionTargetTokens(existing);
            if (!probTokens.empty() && probTokens == existingTokens) {
                alreadyPresent = true;
                break;
            }
        }
        if (!alreadyPresent) {
            prohibitions.push_back(prob);
        }
    }

    const bool isVisualOrAnimation =
        containsCaseInsensitive(lowerOrig, "animat") ||
        containsCaseInsensitive(lowerOrig, "visualiz") ||
        containsCaseInsensitive(lowerOrig, "particle") ||
        containsCaseInsensitive(lowerOrig, "graphic") ||
        containsCaseInsensitive(lowerOrig, "layer 3") ||
        containsCaseInsensitive(lowerOrig, "dashboard") ||
        containsCaseInsensitive(lowerOrig, "render") ||
        containsCaseInsensitive(lowerBase, "particle") ||
        containsCaseInsensitive(lowerBase, "glowing");

    const bool isRefactoring =
        containsCaseInsensitive(lowerOrig, "refactor") ||
        containsCaseInsensitive(lowerOrig, "optimize") ||
        containsCaseInsensitive(lowerOrig, "faster") ||
        containsCaseInsensitive(lowerOrig, "latency") ||
        containsCaseInsensitive(lowerOrig, "speed") ||
        containsCaseInsensitive(lowerOrig, "auth") ||
        containsCaseInsensitive(lowerOrig, "readab");

    if (isVisualOrAnimation) {
        // Variation 1: Direct / Minimalist (Base rewrite, grounded and audited)
        std::string directText = baseStr;
        CreativeAlternative altDirect{
            .id = "alt_direct",
            .title = "Direct / Minimal",
            .styleDescriptor = "Leaves visual styling open for downstream implementation",
            .rewrittenText = directText,
            .diffText = RewriteValidator::computeAccessibleDiff(originalStr, directText),
            .auditOutcome = ValidationOutcome::Pass // Base candidate was verified during synthesize
        };
        alternatives.push_back(std::move(altDirect));

        // Check if glowing or particles are prohibited
        bool glowingProhibited = false;
        for (const auto& p : prohibitions) {
            std::string lp = toLowerAscii(p);
            if (lp.find("glow") != std::string::npos || lp.find("particle") != std::string::npos) {
                glowingProhibited = true;
                break;
            }
        }

        // Variation 2: Dynamic Flow (Alternative styling; unreviewed until selected)
        std::string flowText;
        if (!glowingProhibited && (containsCaseInsensitive(lowerOrig, "particle") || containsCaseInsensitive(lowerOrig, "flow"))) {
            flowText = baseStr + " Emphasize dynamic motion and continuous flow lines.";
        } else {
            flowText = baseStr + " Emphasize clear visual trajectory and dynamic motion along connected nodes.";
        }
        CreativeAlternative altFlow{
            .id = "alt_flow",
            .title = "Dynamic Flow",
            .styleDescriptor = "Expressive flow dynamics and trajectory highlighting",
            .rewrittenText = flowText,
            .diffText = RewriteValidator::computeAccessibleDiff(originalStr, flowText),
            .auditOutcome = ValidationOutcome::Unresolved // F05: unreviewed candidate does not carry unearned Pass
        };
        alternatives.push_back(std::move(altFlow));

        // Variation 3: Structured Topology & Stack (Architectural clarity; unreviewed until selected)
        std::string stackText = baseStr + " Maintain strict structural hierarchy, high contrast, and verifiable precision.";
        CreativeAlternative altStack{
            .id = "alt_stack",
            .title = "Stack & Topology",
            .styleDescriptor = "Structured topological hierarchy and encapsulation layers",
            .rewrittenText = stackText,
            .diffText = RewriteValidator::computeAccessibleDiff(originalStr, stackText),
            .auditOutcome = ValidationOutcome::Unresolved // F05: unreviewed candidate does not carry unearned Pass
        };
        alternatives.push_back(std::move(altStack));
    } else if (isRefactoring) {
        // Variation 1: Focused Prose (Base rewrite, grounded and audited)
        std::string proseText = baseStr;
        CreativeAlternative altProse{
            .id = "alt_prose",
            .title = "Focused Prose",
            .styleDescriptor = "Concise and idiomatic specification in clear prose",
            .rewrittenText = proseText,
            .diffText = RewriteValidator::computeAccessibleDiff(originalStr, proseText),
            .auditOutcome = ValidationOutcome::Pass // Base candidate was verified during synthesize
        };
        alternatives.push_back(std::move(altProse));

        // Variation 2: Step-by-Step Breakdown (F05: preserves target, positive invariants, and ALL prohibitions)
        std::string stepText;
        int stepNum = 1;
        if (!positiveSentences.empty()) {
            for (const auto& s : positiveSentences) {
                stepText += std::to_string(stepNum++) + ". " + s;
                if (stepText.back() != '.') stepText += ".";
                stepText += "\n";
            }
        } else {
            auto origDecomp = decomposeMixedSentences(originalPrompt, prohibitions);
            if (!origDecomp.positiveClauses.empty()) {
                for (const auto& s : origDecomp.positiveClauses) {
                    stepText += std::to_string(stepNum++) + ". " + s;
                    if (stepText.back() != '.') stepText += ".";
                    stepText += "\n";
                }
            } else {
                stepText = "1. " + std::string(originalPrompt);
                if (stepText.back() != '.') stepText += ".";
                stepText += "\n";
                stepNum = 2;
            }
        }

        if (!prohibitions.empty()) {
            stepText += std::to_string(stepNum) + ". Leave forbidden areas strictly untouched: " + formatProhibitions(prohibitions) + ".";
        } else {
            stepText += std::to_string(stepNum) + ". Preserve all existing invariants, error conditions, and validation routines unchanged.";
        }

        CreativeAlternative altSteps{
            .id = "alt_steps",
            .title = "Step-by-Step",
            .styleDescriptor = "Structured numbered steps separating changes from invariants",
            .rewrittenText = stepText,
            .diffText = RewriteValidator::computeAccessibleDiff(originalStr, stepText),
            .auditOutcome = ValidationOutcome::Unresolved // F05: unreviewed candidate does not carry unearned Pass
        };
        alternatives.push_back(std::move(altSteps));

        // Variation 3: Conservative & Minimal (F05: preserves target and ALL prohibitions)
        std::string consText;
        if (!positiveSentences.empty()) {
            consText = "Perform a minimal refactoring: ";
            for (size_t i = 0; i < positiveSentences.size(); ++i) {
                if (i > 0) consText += " ";
                consText += positiveSentences[i];
                if (consText.back() != '.') consText += ".";
            }
        } else {
            auto origDecomp = decomposeMixedSentences(originalPrompt, prohibitions);
            if (!origDecomp.positiveClauses.empty()) {
                consText = "Perform a minimal refactoring: ";
                for (size_t i = 0; i < origDecomp.positiveClauses.size(); ++i) {
                    if (i > 0) consText += " ";
                    consText += origDecomp.positiveClauses[i];
                    if (consText.back() != '.') consText += ".";
                }
            } else {
                consText = "Perform a minimal refactoring: " + std::string(originalPrompt);
                if (consText.back() != '.') consText += ".";
            }
        }

        if (!prohibitions.empty()) {
            consText += " Leave " + formatProhibitions(prohibitions) + " strictly untouched and unchanged.";
        } else {
            consText += " Preserve all existing behavior unchanged.";
        }

        CreativeAlternative altCons{
            .id = "alt_conservative",
            .title = "Conservative & Minimal",
            .styleDescriptor = "Surgical patch minimizing diff footprint and blast radius",
            .rewrittenText = consText,
            .diffText = RewriteValidator::computeAccessibleDiff(originalStr, consText),
            .auditOutcome = ValidationOutcome::Unresolved // F05: unreviewed candidate does not carry unearned Pass
        };
        alternatives.push_back(std::move(altCons));
    } else {
        // Default: Base proposal as primary, plus concise alternative
        CreativeAlternative altBase{
            .id = "alt_base",
            .title = "Standard",
            .styleDescriptor = "Direct rewrite synthesized from prompt analysis",
            .rewrittenText = baseStr,
            .diffText = RewriteValidator::computeAccessibleDiff(originalStr, baseStr),
            .auditOutcome = ValidationOutcome::Pass
        };
        alternatives.push_back(std::move(altBase));

        auto origDecomp = decomposeMixedSentences(originalPrompt, prohibitions);
        static const std::vector<std::string_view> kElaborationPrefixes = {
            " explaining the change", " explaining ", " for ordinary users", " for upgrading users", " to improve "
        };

        // Collect substantive requirement terms from originalPrompt and Require constraints
        std::unordered_set<std::string> requiredSubstantiveTerms;
        for (const auto& w : extractRequirementSubstantiveWords(originalPrompt)) {
            requiredSubstantiveTerms.insert(w);
        }
        for (const auto& c : constraints) {
            if (c.polarity == Polarity::Require) {
                for (const auto& w : extractRequirementSubstantiveWords(c.rule)) {
                    requiredSubstantiveTerms.insert(w);
                }
            }
        }

        const auto& positiveSource = (!positiveSentences.empty()) ? positiveSentences : origDecomp.positiveClauses;
        std::string concisePositive;

        for (size_t i = 0; i < positiveSource.size(); ++i) {
            std::string s = positiveSource[i];
            if (i < origDecomp.positiveClauses.size() && origDecomp.positiveClauses[i].size() < s.size()) {
                s = origDecomp.positiveClauses[i];
            }
            for (const auto& elab : kElaborationPrefixes) {
                auto pos = s.find(elab);
                if (pos != std::string::npos && pos >= 10) {
                    // Guarded elaboration truncation:
                    // Check whether the tail string after the prefix contains substantive requirement words
                    // from originalPrompt or Require constraints.
                    std::string_view tail = std::string_view(s).substr(pos);
                    auto tailWords = extractRequirementSubstantiveWords(tail);
                    bool tailHasSubstantiveReq = false;
                    for (const auto& tw : tailWords) {
                        if (requiredSubstantiveTerms.contains(tw)) {
                            tailHasSubstantiveReq = true;
                            break;
                        }
                    }
                    if (tailHasSubstantiveReq) {
                        // Guarded: tail contains substantive requirements, do not truncate at this prefix
                        continue;
                    }

                    s = cleanClause(s.substr(0, pos));
                    break;
                }
            }
            if (!s.empty()) {
                if (!concisePositive.empty()) concisePositive += " ";
                concisePositive += s;
                if (concisePositive.back() != '.') {
                    concisePositive += ".";
                }
            }
        }

        std::string conciseText = concisePositive;
        for (const auto& prob : prohibitions) {
            if (!containsCaseInsensitive(conciseText, prob)) {
                if (!conciseText.empty() && conciseText.back() != '.') conciseText += ".";
                if (!conciseText.empty()) conciseText += " ";
                conciseText += prob;
                if (!conciseText.empty() && conciseText.back() != '.') conciseText += ".";
            }
        }

        bool concisePreservesProhibitions = true;
        for (const auto& prob : prohibitions) {
            auto targetTokens = extractProhibitionTargetTokens(prob);
            for (const auto& tok : targetTokens) {
                if (!containsCaseInsensitive(conciseText, tok)) {
                    concisePreservesProhibitions = false;
                    break;
                }
            }
            if (!concisePreservesProhibitions) break;
        }

        bool concisePreservesPositiveReqs = true;
        for (const auto& c : constraints) {
            if (c.polarity == Polarity::Require && c.enforcement == Enforcement::Hard && c.origin == Origin::User) {
                auto subWords = extractRequirementSubstantiveWords(c.rule);
                for (const auto& w : subWords) {
                    if (!containsCaseInsensitive(conciseText, w)) {
                        concisePreservesPositiveReqs = false;
                        break;
                    }
                }
                if (!concisePreservesPositiveReqs) {
                    break;
                }
            }
        }

        if (concisePreservesPositiveReqs) {
            for (const auto& posClause : origDecomp.positiveClauses) {
                auto words = extractRequirementSubstantiveWords(posClause);
                for (const auto& w : words) {
                    if (!containsCaseInsensitive(conciseText, w)) {
                        concisePreservesPositiveReqs = false;
                        break;
                    }
                }
                if (!concisePreservesPositiveReqs) {
                    break;
                }
            }
        }

        if (!conciseText.empty() && conciseText != baseStr && conciseText.size() < baseStr.size() &&
            concisePreservesProhibitions && concisePreservesPositiveReqs) {
            CreativeAlternative altConcise{
                .id = "alt_concise",
                .title = "Concise",
                .styleDescriptor = "Shortened direct directive",
                .rewrittenText = conciseText,
                .diffText = RewriteValidator::computeAccessibleDiff(originalStr, conciseText),
                .auditOutcome = ValidationOutcome::Unresolved
            };
            alternatives.push_back(std::move(altConcise));
        }
    }

    // Validate candidates against reconciler and meaning preservation
    ConstraintReconciler reconciler;
    std::vector<CreativeAlternative> validAlternatives;
    for (auto& alt : alternatives) {
        bool preservesProhibitions = true;
        for (const auto& prob : prohibitions) {
            auto targetTokens = extractProhibitionTargetTokens(prob);
            for (const auto& tok : targetTokens) {
                if (!containsCaseInsensitive(alt.rewrittenText, tok)) {
                    preservesProhibitions = false;
                    break;
                }
            }
            if (!preservesProhibitions) break;
        }
        if (!preservesProhibitions) {
            continue;
        }

        auto valRes = reconciler.reconcile(originalPrompt, constraints, alt.rewrittenText);
        if (valRes.outcome == ValidationOutcome::Fail) {
            continue;
        }

        bool missingUserReq = false;
        for (const auto& unres : valRes.unresolved) {
            for (const auto& c : constraints) {
                if (c.id == unres.constraintId && c.origin == Origin::User && c.enforcement == Enforcement::Hard && c.polarity == Polarity::Require) {
                    missingUserReq = true;
                    break;
                }
            }
            if (missingUserReq) break;
        }
        if (missingUserReq) {
            continue;
        }

        validAlternatives.push_back(std::move(alt));
    }

    return validAlternatives;
}

} // namespace roclint::core::rewrite
