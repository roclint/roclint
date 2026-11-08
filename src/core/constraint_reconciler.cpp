#include "roclint/core/constraint_reconciler.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace roclint::core {

namespace {

std::string toLower(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (char c : text) {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return result;
}

std::vector<std::string> extractSubstantiveWords(std::string_view text) {
    static const std::unordered_set<std::string_view> kStopWords = {
        "the", "and", "for", "with", "that", "this", "must", "should",
        "shall", "from", "have", "has", "are", "were", "was", "be", "to",
        "of", "in", "on", "it", "an", "as", "at", "by", "or", "into",
        "make", "build", "show", "use", "not", "dont", "never"
    };
    std::vector<std::string> words;
    std::string current;
    for (char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            current.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        } else {
            if (current.size() >= 3 && !kStopWords.contains(current)) {
                words.push_back(std::move(current));
            }
            current.clear();
        }
    }
    if (current.size() >= 3 && !kStopWords.contains(current)) {
        words.push_back(std::move(current));
    }
    return words;
}

bool containsAnyKeyword(std::string_view text, const std::vector<std::string>& keywords) {
    const std::string lower = toLower(text);
    for (const auto& kw : keywords) {
        if (lower.find(toLower(kw)) != std::string::npos) {
            return true;
        }
    }
    return false;
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

        // 4. Active Hard or Conditional constraints
        if (constraint.polarity == Polarity::Prohibit) {
            // Check if rewrite violates negative prohibition
            static const std::unordered_set<std::string_view> kMutationWords = {
                "modify", "rewrite", "update", "change", "refactor", "alter", "replace", "touch", "delete", "remove"
            };
            std::vector<std::string> targetNouns;
            for (const auto& word : extractSubstantiveWords(constraint.rule)) {
                if (!kMutationWords.contains(word)) {
                    targetNouns.push_back(word);
                }
            }

            if (!targetNouns.empty()) {
                std::size_t targetMatches = 0;
                for (const auto& noun : targetNouns) {
                    if (rewriteLower.find(noun) != std::string::npos) {
                        ++targetMatches;
                    }
                }

                if (targetMatches > 0) {
                    bool hasMutation = false;
                    for (const auto& verb : kMutationWords) {
                        if (rewriteLower.find(verb) != std::string::npos) {
                            hasMutation = true;
                            break;
                        }
                    }

                    static const char* kPreservePhrases[] = {
                        "without", "untouched", "don't", "do not", "never", "preserve", "leaving", "keep", "unmodified"
                    };
                    bool explicitlyPreserved = false;
                    for (const char* p : kPreservePhrases) {
                        if (rewriteLower.find(p) != std::string::npos) {
                            explicitlyPreserved = true;
                            break;
                        }
                    }

                    if (hasMutation && !explicitlyPreserved) {
                        result.violations.push_back(ValidationFinding{
                            constraint.id,
                            "Candidate rewrite violates negative boundary prohibition: '" + constraint.rule + "'."
                        });
                    }
                }
            }
        } else {
            // Polarity::Require
            if (constraint.origin == Origin::User && constraint.enforcement == Enforcement::Hard) {
                const auto substantive = extractSubstantiveWords(constraint.rule);
                if (!substantive.empty()) {
                    bool anyFound = false;
                    for (const auto& word : substantive) {
                        if (rewriteLower.find(word) != std::string::npos) {
                            anyFound = true;
                            break;
                        }
                    }
                    if (!anyFound) {
                        result.violations.push_back(ValidationFinding{
                            constraint.id,
                            "Candidate rewrite omitted mandatory user requirement: '" + constraint.rule + "'."
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
