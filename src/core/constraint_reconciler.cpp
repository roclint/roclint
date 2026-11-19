#include "roclint/core/constraint_reconciler.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace roclint::core {

namespace {

std::string toLower(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == 0xE2 && i + 2 < text.size()) {
            unsigned char c2 = static_cast<unsigned char>(text[i + 1]);
            unsigned char c3 = static_cast<unsigned char>(text[i + 2]);
            if (c2 == 0x80 && (c3 == 0x99 || c3 == 0x98)) {
                result.push_back('\'');
                i += 2;
                continue;
            }
        }
        if (c == '`') {
            result.push_back('\'');
        } else {
            result.push_back(static_cast<char>(std::tolower(c)));
        }
    }
    return result;
}

std::vector<std::string> extractWords(std::string_view text) {
    const std::string norm = toLower(text);
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

std::vector<std::string> extractSubstantiveWords(std::string_view text) {
    const auto raw = extractWords(text);
    std::vector<std::string> substantive;
    for (const auto& w : raw) {
        if (w.size() < 2) continue;
        if (kRequirementStopWords.contains(w)) continue;
        if (kGenericRequirementWords.contains(w)) continue;
        substantive.push_back(w);
    }
    // Fallback if rule consists entirely of generic verbs / stop words
    if (substantive.empty()) {
        for (const auto& w : raw) {
            if (w.size() < 2) continue;
            if (kRequirementStopWords.contains(w)) continue;
            substantive.push_back(w);
        }
    }
    return substantive;
}

static const std::unordered_set<std::string_view> kMutationWords = {
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

static const std::unordered_set<std::string_view> kPreservationVerbs = {
    "preserve", "preserving", "preserved",
    "keep", "keeping", "kept",
    "leave", "leaving", "left",
    "maintain", "maintaining", "maintained",
    "retain", "retaining", "retained",
    "remain", "remaining", "remains",
    "stay", "staying", "stays"
};

static const std::unordered_set<std::string_view> kPreservationPredicates = {
    "unchanged", "untouched", "unmodified", "intact"
};

static const std::unordered_set<std::string_view> kNegationWords = {
    "not", "no", "never", "without", "avoid", "stop", "prevent",
    "don't", "dont",
    "won't", "wont",
    "can't", "cant", "cannot",
    "shouldn't", "shouldnt",
    "wouldn't", "wouldnt",
    "couldn't", "couldnt",
    "doesn't", "doesnt",
    "didn't", "didnt",
    "isn't", "isnt",
    "aren't", "arent",
    "wasn't", "wasnt",
    "weren't", "werent",
    "haven't", "havent",
    "hasn't", "hasnt",
    "hadn't", "hadnt",
    "mustn't", "mustnt"
};

static const std::unordered_set<std::string_view> kControlOrStopWords = {
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

std::vector<std::string> extractTargetTokens(std::string_view rule) {
    std::vector<std::string> rawWords = extractWords(rule);
    std::vector<std::string> targetTokens;
    for (const auto& w : rawWords) {
        if (w.size() < 2) continue;
        if (kControlOrStopWords.contains(w)) continue;
        if (kMutationWords.contains(w)) continue;
        if (kPreservationVerbs.contains(w)) continue;
        if (kPreservationPredicates.contains(w)) continue;
        if (kNegationWords.contains(w)) continue;
        targetTokens.push_back(w);
    }
    return targetTokens;
}

enum class TargetScopeResult {
    NotMentioned,
    Preserved,
    Mutated,
    Ambiguous,
    Contradictory
};

struct TextSpan {
    std::size_t start{0};
    std::size_t end{0};
};

std::vector<TextSpan> findTargetOccurrences(std::string_view text, const std::vector<std::string>& targetTokens) {
    std::vector<TextSpan> occurrences;
    if (targetTokens.empty() || text.empty()) return occurrences;

    auto isWordBoundaryAt = [](std::string_view s, std::size_t pos) {
        if (pos == 0 || pos >= s.size()) return true;
        char prev = s[pos - 1];
        char curr = s[pos];
        if (curr == '\'' && pos + 1 < s.size() &&
            std::isalpha(static_cast<unsigned char>(prev)) &&
            std::isalpha(static_cast<unsigned char>(s[pos + 1]))) {
            return false;
        }
        if (prev == '\'' && pos >= 2 &&
            std::isalpha(static_cast<unsigned char>(s[pos - 2])) &&
            std::isalpha(static_cast<unsigned char>(curr))) {
            return false;
        }
        return !std::isalnum(static_cast<unsigned char>(prev)) || !std::isalnum(static_cast<unsigned char>(curr));
    };

    static const std::unordered_set<std::string_view> kGenericDescriptors = {
        "implementation", "logic", "handler", "routine", "routines",
        "code", "subsystem", "module", "system", "component", "details",
        "architecture", "structure", "function", "functions", "method", "methods",
        "file", "files", "data", "item", "items"
    };

    std::vector<TextSpan> rawSpans;

    // 1. Scan all sub-phrases of targetTokens of length >= 2 down to 2
    for (std::size_t len = targetTokens.size(); len >= 2; --len) {
        for (std::size_t startIdx = 0; startIdx + len <= targetTokens.size(); ++startIdx) {
            std::string phrase;
            for (std::size_t k = 0; k < len; ++k) {
                if (k > 0) phrase += " ";
                phrase += targetTokens[startIdx + k];
            }
            if (phrase.size() >= 3) {
                std::size_t pos = 0;
                while ((pos = text.find(phrase, pos)) != std::string::npos) {
                    std::size_t endPos = pos + phrase.size();
                    if (isWordBoundaryAt(text, pos) && isWordBoundaryAt(text, endPos)) {
                        rawSpans.push_back(TextSpan{pos, endPos});
                    }
                    pos = endPos;
                }
            }
        }
    }

    // 2. Scan single target tokens (skipping generic descriptors if target has > 1 token)
    for (const auto& tok : targetTokens) {
        if (tok.size() < 3) continue;
        if (targetTokens.size() > 1 && kGenericDescriptors.contains(tok)) continue;

        std::size_t pos = 0;
        while ((pos = text.find(tok, pos)) != std::string::npos) {
            std::size_t endPos = pos + tok.size();
            if (isWordBoundaryAt(text, pos) && isWordBoundaryAt(text, endPos)) {
                rawSpans.push_back(TextSpan{pos, endPos});
            }
            pos = endPos;
        }
    }

    if (rawSpans.empty()) {
        return occurrences;
    }

    // 3. Merge overlapping spans to the longest match, retaining non-overlapping occurrences
    std::sort(rawSpans.begin(), rawSpans.end(), [](const TextSpan& a, const TextSpan& b) {
        std::size_t lenA = a.end - a.start;
        std::size_t lenB = b.end - b.start;
        if (lenA != lenB) return lenA > lenB;
        return a.start < b.start;
    });

    std::vector<TextSpan> selected;
    for (const auto& span : rawSpans) {
        bool overlaps = false;
        for (const auto& existing : selected) {
            if (std::max(span.start, existing.start) < std::min(span.end, existing.end)) {
                overlaps = true;
                break;
            }
        }
        if (!overlaps) {
            selected.push_back(span);
        }
    }

    // Sort selected occurrences by start position ascending
    std::sort(selected.begin(), selected.end(), [](const TextSpan& a, const TextSpan& b) {
        return a.start < b.start;
    });

    return selected;
}

TargetScopeResult evaluateTargetBoundScoping(
    std::string_view rewriteLower,
    std::string_view rule) {

    const auto targetTokens = extractTargetTokens(rule);
    if (targetTokens.empty()) {
        return TargetScopeResult::NotMentioned;
    }

    const auto occurrences = findTargetOccurrences(rewriteLower, targetTokens);
    if (occurrences.empty()) {
        return TargetScopeResult::NotMentioned;
    }

    static const std::vector<std::string_view> kLeftMarkers = {
        " then ", " and ", " but ", " while ", " however ", " although ",
        " whereas ", " except ", " without ", " as well as ", " instead of ",
        " rather than ", " after ", " before ", " with ", " leaving ",
        " keeping ", " yet ", " so that "
    };

    static const std::vector<std::string_view> kRightMarkers = {
        " then ", " and ", " but ", " while ", " however ", " although ",
        " whereas ", " except ", " without ", " as well as ", " instead of ",
        " rather than ", " before ", " after ", " with ", " leaving ",
        " keeping ", " yet ", " so that "
    };

    auto isPunctuationDelim = [](char c) {
        return c == '.' || c == '!' || c == '?' || c == ';' || c == '\n' || c == ',' || c == ':' || c == '-';
    };

    static const std::unordered_set<std::string_view> kAllowedPostTargetConnectors = {
        "is", "are", "was", "were", "be", "been", "being",
        "has", "have", "had", "having", "does", "did", "do",
        "get", "gets", "got", "getting",
        "remains", "remain", "remained", "stays", "stay", "stayed",
        "kept", "leaves", "leaving", "left", "held",
        "must", "should", "shall", "will", "would", "can", "could", "may", "might",
        "strictly", "completely", "totally", "fully", "largely", "entirely", "also",
        "not", "never", "avoid", "stop", "prevent", "to", "as", "and", "both", "either",
        // negative auxiliary contractions
        "isn't", "isnt", "aren't", "arent", "wasn't", "wasnt", "weren't", "werent",
        "won't", "wont", "can't", "cant", "cannot",
        "doesn't", "doesnt", "didn't", "didnt", "don't", "dont",
        "hasn't", "hasnt", "haven't", "havent", "hadn't", "hadnt",
        "shouldn't", "shouldnt", "wouldn't", "wouldnt", "couldn't", "couldnt",
        "mustn't", "mustnt",
        // generic target descriptors
        "implementation", "logic", "handler", "routine", "routines",
        "code", "subsystem", "module", "system", "component", "details",
        "architecture", "structure", "function", "functions", "method", "methods",
        "file", "files", "data", "item", "items"
    };
    static const std::unordered_set<std::string_view> kTargetModifiers = {
        "a", "an", "the", "all", "any", "both", "each", "every", "this", "that",
        "its", "their", "our", "your", "existing", "current", "old", "new",
        "legacy", "original", "entire", "whole", "public", "internal", "only", "just"
    };
    static const std::unordered_set<std::string_view> kTargetPrepositions = {
        "about", "at", "for", "from", "in", "of", "on", "regarding", "to"
    };
    static const std::unordered_set<std::string_view> kReferenceHeads = {
        "documentation", "docs", "guide", "guides", "example", "examples",
        "test", "tests", "specification", "specifications"
    };

    bool anyMutated = false;
    bool anyPreserved = false;
    [[maybe_unused]] bool anyAmbiguous = false;
    bool anyContradictory = false;

    for (const auto& span : occurrences) {
        // Find clauseStart scanning backwards from span.start
        std::size_t clauseStart = 0;
        for (std::size_t i = span.start; i > 0; --i) {
            char c = rewriteLower[i - 1];
            if (isPunctuationDelim(c)) {
                clauseStart = i;
                break;
            }
            bool matchedMarker = false;
            for (const auto& m : kLeftMarkers) {
                if (i >= m.size() && rewriteLower.substr(i - m.size(), m.size()) == m) {
                    if (m == " without ") {
                        clauseStart = i - m.size() + 1; // start at "without"
                    } else {
                        clauseStart = i;
                    }
                    matchedMarker = true;
                    break;
                }
            }
            if (matchedMarker) break;
        }

        // Find clauseEnd scanning forwards from span.end
        std::size_t clauseEnd = rewriteLower.size();
        for (std::size_t i = span.end; i < rewriteLower.size(); ++i) {
            char c = rewriteLower[i];
            if (isPunctuationDelim(c)) {
                clauseEnd = i;
                break;
            }
            bool matchedMarker = false;
            for (const auto& m : kRightMarkers) {
                if (i + m.size() <= rewriteLower.size() && rewriteLower.substr(i, m.size()) == m) {
                    clauseEnd = i;
                    matchedMarker = true;
                    break;
                }
            }
            if (matchedMarker) break;
        }

        std::string_view preContext = (span.start > clauseStart) ? rewriteLower.substr(clauseStart, span.start - clauseStart) : std::string_view{};
        std::string_view postContext = (clauseEnd > span.end) ? rewriteLower.substr(span.end, clauseEnd - span.end) : std::string_view{};

        auto preWords = extractWords(preContext);
        auto postWords = extractWords(postContext);

        // A preceding verb binds to the target only when the intervening
        // words modify the target rather than introduce another object.
        std::size_t modifierStart = preWords.size();
        while (modifierStart > 0) {
            const auto& word = preWords[modifierStart - 1];
            if (!kTargetModifiers.contains(word) && !kPreservationPredicates.contains(word) &&
                !word.ends_with("ly")) break;
            --modifierStart;
        }
        if ((modifierStart > 0 && kTargetPrepositions.contains(preWords[modifierStart - 1])) ||
            (!postWords.empty() && kReferenceHeads.contains(postWords.front()))) {
            anyAmbiguous = true;
            continue;
        }

        bool occMutated = false;
        bool occPreserved = false;
        bool prePreservationNegated = false;

        // Check preWords
        for (size_t i = modifierStart > 0 ? modifierStart - 1 : 0; i < preWords.size(); ++i) {
            const auto& w = preWords[i];
            if (kMutationWords.contains(w)) {
                int negationCount = 0;
                size_t lookback = (i >= 5) ? i - 5 : 0;
                for (size_t j = lookback; j < i; ++j) {
                    if (kNegationWords.contains(preWords[j])) {
                        negationCount++;
                    }
                }
                bool isNegated = (negationCount % 2 == 1);
                if (isNegated) {
                    if (!occMutated) {
                        occPreserved = true;
                    }
                } else {
                    occMutated = true;
                    occPreserved = false;
                }
            }
            if (kPreservationVerbs.contains(w) || kPreservationPredicates.contains(w)) {
                int negationCount = 0;
                size_t lookback = (i >= 5) ? i - 5 : 0;
                for (size_t j = lookback; j < i; ++j) {
                    if (kNegationWords.contains(preWords[j])) {
                        negationCount++;
                    }
                }
                bool isNegated = (negationCount % 2 == 1);
                if (isNegated) {
                    occMutated = true;
                    occPreserved = false;
                    prePreservationNegated = true;
                } else {
                    if (!occMutated) {
                        occPreserved = true;
                    }
                }
            }
        }

        // Check postWords
        for (size_t i = 0; i < postWords.size(); ++i) {
            const auto& w = postWords[i];

            // Prevent preservation or mutation leakage across intervening substantive nouns
            bool hasInterveningNoun = false;
            for (size_t j = 0; j < i; ++j) {
                if (!kAllowedPostTargetConnectors.contains(postWords[j])) {
                    hasInterveningNoun = true;
                    break;
                }
            }

            if (!hasInterveningNoun) {
                int negationCount = 0;
                size_t lookback = (i >= 5) ? i - 5 : 0;
                for (size_t j = lookback; j < i; ++j) {
                    if (kNegationWords.contains(postWords[j])) {
                        negationCount++;
                    }
                }
                if (prePreservationNegated) {
                    negationCount++;
                }
                bool isNegated = (negationCount % 2 == 1);

                if (kPreservationPredicates.contains(w) || kPreservationVerbs.contains(w)) {
                    if (isNegated) {
                        occMutated = true;
                        occPreserved = false;
                    } else if (!occMutated) {
                        occPreserved = true;
                    }
                }
                if (w == "strictly" && i + 1 < postWords.size() && postWords[i + 1] == "untouched") {
                    if (isNegated) {
                        occMutated = true;
                        occPreserved = false;
                    } else if (!occMutated) {
                        occPreserved = true;
                    }
                }
                if (kMutationWords.contains(w)) {
                    if (isNegated) {
                        if (!occMutated) {
                            occPreserved = true;
                        }
                    } else {
                        occMutated = true;
                        occPreserved = false;
                    }
                }
            }
        }

        if (occMutated) {
            anyMutated = true;
        } else if (occPreserved) {
            anyPreserved = true;
        } else {
            anyAmbiguous = true;
        }
    }

    if (anyContradictory || (anyMutated && anyPreserved)) {
        return TargetScopeResult::Contradictory;
    }
    if (anyMutated) {
        return TargetScopeResult::Mutated;
    }
    if (anyPreserved) {
        return TargetScopeResult::Preserved;
    }
    return TargetScopeResult::Ambiguous;
}

} // namespace

ConstraintReconciler::ConstraintReconciler() {
    // Built-in applicability predicates
    registerPredicate("depicts_ipv4_forwarding", [](std::string_view text) {
        const std::string lower = toLower(text);
        if (lower.find("ipv4") != std::string::npos ||
            lower.find("ip routing") != std::string::npos ||
            lower.find("forwarding table") != std::string::npos ||
            lower.find("next hop") != std::string::npos ||
            lower.find("next-hop") != std::string::npos ||
            lower.find("cidr") != std::string::npos) {
            return ApplicabilityStatus::Active;
        }
        if (lower.find("bgp") != std::string::npos || lower.find("layer 3") != std::string::npos) {
            // General mention without depicting forwarding steps
            return ApplicabilityStatus::Inactive;
        }
        return ApplicabilityStatus::Inactive;
    });

    registerPredicate("shows_packet_forwarding", [](std::string_view text) {
        const std::string lower = toLower(text);
        if (lower.find("packet") != std::string::npos &&
            (lower.find("hop") != std::string::npos || lower.find("forward") != std::string::npos || lower.find("route") != std::string::npos)) {
            return ApplicabilityStatus::Active;
        }
        return ApplicabilityStatus::Inactive;
    });

    registerPredicate("touches_token_validation", [](std::string_view text) {
        const std::string lower = toLower(text);
        if (lower.find("token") != std::string::npos && lower.find("validat") != std::string::npos) {
            return ApplicabilityStatus::Active;
        }
        return ApplicabilityStatus::Inactive;
    });
}

void ConstraintReconciler::registerPredicate(std::string name, PredicateFn predicate) {
    if (!name.empty() && predicate) {
        predicates_[std::move(name)] = std::move(predicate);
    }
}

ApplicabilityStatus ConstraintReconciler::evaluateApplicability(
    const ScopedConstraint& constraint,
    std::string_view contextText) const {

    if (!constraint.appliesWhen.has_value() || constraint.appliesWhen->empty()) {
        return ApplicabilityStatus::Active;
    }

    const auto it = predicates_.find(*constraint.appliesWhen);
    if (it != predicates_.end()) {
        return it->second(contextText);
    }

    return ApplicabilityStatus::Unknown;
}

bool ConstraintReconciler::areOrthogonal(
    const ScopedConstraint& a,
    const ScopedConstraint& b) noexcept {

    if (!a.aspect.empty() && !b.aspect.empty() && a.aspect != b.aspect) {
        return true;
    }
    return false;
}

ValidationResult ConstraintReconciler::reconcile(
    std::string_view /*originalPrompt*/,
    const std::vector<ScopedConstraint>& constraints,
    std::string_view candidateRewrite) const {

    ValidationResult result;
    const std::string rewriteLower = toLower(candidateRewrite);

    for (const auto& constraint : constraints) {
        const auto applicability = evaluateApplicability(constraint, candidateRewrite);

        // 1. Inactive constraints do not gate or fail validation
        if (applicability == ApplicabilityStatus::Inactive) {
            continue;
        }

        // 2. Unknown applicability on Hard constraints yields Unresolved
        if (applicability == ApplicabilityStatus::Unknown) {
            if (constraint.enforcement == Enforcement::Hard || constraint.enforcement == Enforcement::Conditional) {
                result.unresolved.push_back(ValidationFinding{
                    constraint.id,
                    "Applicability condition '" + constraint.appliesWhen.value_or("unknown") + "' could not be evaluated."
                });
            }
            continue;
        }

        // 3. Active Advisory constraints carry zero veto power
        if (constraint.enforcement == Enforcement::Advisory) {
            continue;
        }

        if (constraint.polarity == Polarity::Prohibit) {
            const auto scopeRes = evaluateTargetBoundScoping(rewriteLower, constraint.rule);
            if (scopeRes == TargetScopeResult::Mutated) {
                result.violations.push_back(ValidationFinding{
                    constraint.id,
                    "Candidate rewrite violates negative boundary prohibition: '" + constraint.rule + "'."
                });
            } else if (scopeRes == TargetScopeResult::Ambiguous) {
                result.unresolved.push_back(ValidationFinding{
                    constraint.id,
                    "Candidate rewrite mentions protected target with ambiguous scope in rule: '" + constraint.rule + "'. Semantic review required."
                });
            } else if (scopeRes == TargetScopeResult::Contradictory) {
                result.unresolved.push_back(ValidationFinding{
                    constraint.id,
                    "Candidate rewrite contains contradictory mutation and preservation directives for protected target in rule: '" + constraint.rule + "'."
                });
            }
        } else {
            // Polarity::Require
            if (constraint.origin == Origin::User && constraint.enforcement == Enforcement::Hard) {
                const auto substantive = extractSubstantiveWords(constraint.rule);
                if (!substantive.empty()) {
                    std::vector<std::string> missing;
                    for (const auto& word : substantive) {
                        if (rewriteLower.find(word) == std::string::npos) {
                            missing.push_back(word);
                        }
                    }
                    if (!missing.empty()) {
                        std::string missingDetails;
                        for (size_t mi = 0; mi < missing.size(); ++mi) {
                            if (mi > 0) missingDetails += ", ";
                            missingDetails += "'" + missing[mi] + "'";
                        }
                        result.unresolved.push_back(ValidationFinding{
                            constraint.id,
                            "Candidate rewrite may paraphrase or omit user requirement (missing substantive terms: " +
                            missingDetails + "): '" + constraint.rule + "'. Semantic review required."
                        });
                    }
                }
            }
        }
    }

    if (!result.violations.empty()) {
        result.outcome = ValidationOutcome::Fail;
    } else if (!result.unresolved.empty()) {
        result.outcome = ValidationOutcome::Unresolved;
    } else {
        result.outcome = ValidationOutcome::Pass;
    }

    return result;
}

} // namespace roclint::core
