#include "roclint/core/citation_evaluator.hpp"
#include "roclint/analysis/SourceSpan.hpp"

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

bool isStopWord(std::string_view word) noexcept {
    static const std::unordered_set<std::string_view> kStopWords = {
        "the", "and", "for", "with", "that", "this", "must", "should",
        "shall", "from", "have", "has", "are", "were", "was", "be", "to",
        "of", "in", "on", "it", "an", "as", "at", "by", "or", "into",
        "make", "build", "show", "use"
    };
    return kStopWords.contains(word);
}

std::vector<std::string> extractSubstantiveWords(std::string_view text) {
    std::vector<std::string> words;
    std::string current;
    for (char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            current.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        } else {
            if (current.size() >= 3 && !isStopWord(current)) {
                words.push_back(std::move(current));
            }
            current.clear();
        }
    }
    if (current.size() >= 3 && !isStopWord(current)) {
        words.push_back(std::move(current));
    }
    return words;
}

bool containsNegationMarker(std::string_view text) {
    const std::string lower = toLower(text);
    static const char* kNegations[] = {
        "don't", "do not", "never", "not", "no ", "avoid", "prevent",
        "prohibit", "without", "neither", "none"
    };
    for (const char* neg : kNegations) {
        if (lower.find(neg) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

const char* toString(CitationVerdict verdict) noexcept {
    switch (verdict) {
        case CitationVerdict::Verified: return "Verified";
        case CitationVerdict::Overreached: return "Overreached";
        case CitationVerdict::InvalidSpan: return "InvalidSpan";
        case CitationVerdict::Fabricated: return "Fabricated";
    }
    return "InvalidSpan";
}

Result<CitationVerdict> parseCitationVerdict(std::string_view text) noexcept {
    if (text == "Verified") return CitationVerdict::Verified;
    if (text == "Overreached") return CitationVerdict::Overreached;
    if (text == "InvalidSpan") return CitationVerdict::InvalidSpan;
    if (text == "Fabricated") return CitationVerdict::Fabricated;
    return makeError(ErrorCode::InvalidInput, "Unknown CitationVerdict: " + std::string(text));
}

json::Value toJson(const CitationEvaluation& eval) {
    auto obj = json::Value::object();
    obj.set("constraint_id", json::Value::string(eval.constraintId));
    obj.set("verdict", json::Value::string(toString(eval.verdict)));
    obj.set("assigned_status", json::Value::string(toString(eval.assignedStatus)));
    obj.set("explanation", json::Value::string(eval.explanation));
    obj.set("verified_constraint", toJson(eval.verifiedConstraint));
    return obj;
}

Result<CitationEvaluation> citationEvaluationFromJson(const json::Value& val) {
    if (!val.isObject()) {
        return makeError(ErrorCode::InvalidInput, "CitationEvaluation JSON must be an object");
    }
    const auto* idVal = val.find("constraint_id");
    const auto* verdictVal = val.find("verdict");
    const auto* statusVal = val.find("assigned_status");
    const auto* expVal = val.find("explanation");
    const auto* constraintVal = val.find("verified_constraint");

    if (!idVal || !idVal->isString() ||
        !verdictVal || !verdictVal->isString() ||
        !statusVal || !statusVal->isString() ||
        !expVal || !expVal->isString() ||
        !constraintVal || !constraintVal->isObject()) {
        return makeError(ErrorCode::InvalidInput, "Missing or invalid CitationEvaluation fields");
    }

    auto verdict = parseCitationVerdict(verdictVal->asString());
    if (!verdict.ok()) return verdict.error();

    auto status = parseEvidenceStatus(statusVal->asString());
    if (!status.ok()) return status.error();

    auto constraint = scopedConstraintFromJson(*constraintVal);
    if (!constraint.ok()) return constraint.error();

    CitationEvaluation eval;
    eval.constraintId = idVal->asString();
    eval.verdict = verdict.value();
    eval.assignedStatus = status.value();
    eval.explanation = expVal->asString();
    eval.verifiedConstraint = std::move(constraint.value());
    return eval;
}

void CitationEvaluator::registerAuthorityPrefix(std::string prefix) {
    if (!prefix.empty()) {
        authorityPrefixes_.push_back(std::move(prefix));
    }
}

bool CitationEvaluator::isRecognizedAuthority(std::string_view authorityRef) const noexcept {
    if (authorityRef.empty()) {
        return false;
    }
    for (const auto& prefix : authorityPrefixes_) {
        if (authorityRef.starts_with(prefix)) {
            return true;
        }
    }
    return false;
}

CitationEvaluation CitationEvaluator::evaluate(
    std::string_view promptText,
    std::string_view promptId,
    const ScopedConstraint& candidate) const {

    CitationEvaluation result;
    result.constraintId = candidate.id;
    result.verifiedConstraint = candidate;
    result.verifiedConstraint.evidenceStatus = EvidenceStatus::Unverified;

    // ------------------------------------------------------------------------
    // Path 1: User Origin Verification
    // ------------------------------------------------------------------------
    if (candidate.origin == Origin::User) {
        if (candidate.evidence.empty()) {
            result.verdict = CitationVerdict::InvalidSpan;
            result.assignedStatus = EvidenceStatus::Rejected;
            result.explanation = "User origin claimed without citation evidence.";
            result.verifiedConstraint.evidenceStatus = EvidenceStatus::Rejected;
            return result;
        }

        std::string citedAccumulator;
        for (const auto& span : candidate.evidence) {
            // If the span references the active prompt
            if (span.sourceId == promptId || span.sourceId.empty()) {
                if (span.beginByte > span.endByte || span.endByte > promptText.size()) {
                    result.verdict = CitationVerdict::InvalidSpan;
                    result.assignedStatus = EvidenceStatus::Rejected;
                    result.explanation = "SourceSpan byte range out of prompt bounds.";
                    result.verifiedConstraint.evidenceStatus = EvidenceStatus::Rejected;
                    return result;
                }
                if (!analysis::isUtf8Boundary(promptText, span.beginByte) ||
                    !analysis::isUtf8Boundary(promptText, span.endByte)) {
                    result.verdict = CitationVerdict::InvalidSpan;
                    result.assignedStatus = EvidenceStatus::Rejected;
                    result.explanation = "SourceSpan slices inside a multi-byte UTF-8 sequence.";
                    result.verifiedConstraint.evidenceStatus = EvidenceStatus::Rejected;
                    return result;
                }
                citedAccumulator += " ";
                citedAccumulator += promptText.substr(span.beginByte, span.endByte - span.beginByte);
            }
        }

        if (citedAccumulator.empty()) {
            result.verdict = CitationVerdict::Fabricated;
            result.assignedStatus = EvidenceStatus::Rejected;
            result.explanation = "No evidence spans reference the active user prompt.";
            result.verifiedConstraint.evidenceStatus = EvidenceStatus::Rejected;
            return result;
        }

        // Check for Derivation::Assumed claiming User origin
        if (candidate.derivation == Derivation::Assumed) {
            result.verdict = CitationVerdict::Overreached;
            result.assignedStatus = EvidenceStatus::Rejected;
            result.explanation = "Assumed derivations cannot claim User origin. Demoted to Model Advisory.";
            result.verifiedConstraint.origin = Origin::Model;
            result.verifiedConstraint.derivation = Derivation::Assumed;
            result.verifiedConstraint.enforcement = Enforcement::Advisory;
            result.verifiedConstraint.evidenceStatus = EvidenceStatus::Unverified;
            return result;
        }

        // Substantive Lexical Grounding Check
        const auto ruleWords = extractSubstantiveWords(candidate.rule);
        const auto citedWords = extractSubstantiveWords(citedAccumulator);

        std::size_t matches = 0;
        for (const auto& rw : ruleWords) {
            for (const auto& cw : citedWords) {
                if (rw == cw || rw.find(cw) != std::string::npos || cw.find(rw) != std::string::npos) {
                    ++matches;
                    break;
                }
            }
        }

        // If rule has words but zero match the cited span: source-span laundering!
        if (!ruleWords.empty() && matches == 0) {
            result.verdict = CitationVerdict::Overreached;
            result.assignedStatus = EvidenceStatus::Rejected;
            result.explanation = "Cited source span does not substantiate rule terms (source-span laundering detected). Demoted to Model Advisory.";
            result.verifiedConstraint.origin = Origin::Model;
            result.verifiedConstraint.derivation = Derivation::Assumed;
            result.verifiedConstraint.enforcement = Enforcement::Advisory;
            result.verifiedConstraint.evidenceStatus = EvidenceStatus::Unverified;
            return result;
        }

        // Check Polarity consistency
        if (candidate.polarity == Polarity::Prohibit && !containsNegationMarker(citedAccumulator)) {
            result.verdict = CitationVerdict::Overreached;
            result.assignedStatus = EvidenceStatus::Rejected;
            result.explanation = "Prohibition polarity claimed without negation marker in cited text. Demoted to Model Advisory.";
            result.verifiedConstraint.origin = Origin::Model;
            result.verifiedConstraint.derivation = Derivation::Assumed;
            result.verifiedConstraint.enforcement = Enforcement::Advisory;
            result.verifiedConstraint.evidenceStatus = EvidenceStatus::Unverified;
            return result;
        }

        // Legitimate user citation verified
        result.verdict = CitationVerdict::Verified;
        result.assignedStatus = EvidenceStatus::Supported;
        result.explanation = "Cited user source span legitimately substantiates the constraint.";
        result.verifiedConstraint.evidenceStatus = EvidenceStatus::Supported;
        return result;
    }

    // ------------------------------------------------------------------------
    // Path 2: Domain Origin Verification
    // ------------------------------------------------------------------------
    if (candidate.origin == Origin::Domain) {
        if (!candidate.authorityRef.has_value() || candidate.authorityRef->empty()) {
            result.verdict = CitationVerdict::Overreached;
            result.assignedStatus = EvidenceStatus::Rejected;
            result.explanation = "Domain invariant lacks an authority reference. Demoted to Model Advisory.";
            result.verifiedConstraint.origin = Origin::Model;
            result.verifiedConstraint.derivation = Derivation::Assumed;
            result.verifiedConstraint.enforcement = Enforcement::Advisory;
            result.verifiedConstraint.evidenceStatus = EvidenceStatus::Unverified;
            return result;
        }

        if (!isRecognizedAuthority(*candidate.authorityRef)) {
            result.verdict = CitationVerdict::Fabricated;
            result.assignedStatus = EvidenceStatus::Rejected;
            result.explanation = "Unrecognized authority reference format: " + *candidate.authorityRef;
            result.verifiedConstraint.evidenceStatus = EvidenceStatus::Rejected;
            return result;
        }

        result.verdict = CitationVerdict::Verified;
        result.assignedStatus = EvidenceStatus::Supported;
        result.explanation = "Recognized domain authority reference verified: " + *candidate.authorityRef;
        result.verifiedConstraint.evidenceStatus = EvidenceStatus::Supported;
        return result;
    }

    // ------------------------------------------------------------------------
    // Path 3: Environment Origin Verification
    // ------------------------------------------------------------------------
    if (candidate.origin == Origin::Environment) {
        if (!candidate.authorityRef.has_value() || candidate.authorityRef->empty()) {
            result.verdict = CitationVerdict::Overreached;
            result.assignedStatus = EvidenceStatus::Rejected;
            result.explanation = "Environment constraint lacks a tool/system authority reference. Demoted to Model Advisory.";
            result.verifiedConstraint.origin = Origin::Model;
            result.verifiedConstraint.derivation = Derivation::Assumed;
            result.verifiedConstraint.enforcement = Enforcement::Advisory;
            result.verifiedConstraint.evidenceStatus = EvidenceStatus::Unverified;
            return result;
        }

        result.verdict = CitationVerdict::Verified;
        result.assignedStatus = EvidenceStatus::Supported;
        result.explanation = "Environment tool reference verified: " + *candidate.authorityRef;
        result.verifiedConstraint.evidenceStatus = EvidenceStatus::Supported;
        return result;
    }

    // ------------------------------------------------------------------------
    // Path 4: Model Candidate Verification
    // ------------------------------------------------------------------------
    if (candidate.origin == Origin::Model) {
        if (candidate.enforcement == Enforcement::Hard) {
            result.verdict = CitationVerdict::Overreached;
            result.assignedStatus = EvidenceStatus::Rejected;
            result.explanation = "Model candidates cannot self-assign Hard enforcement. Demoted to Advisory.";
            result.verifiedConstraint.enforcement = Enforcement::Advisory;
            result.verifiedConstraint.evidenceStatus = EvidenceStatus::Unverified;
            return result;
        }

        result.verdict = CitationVerdict::Verified;
        result.assignedStatus = EvidenceStatus::Unverified;
        result.explanation = "Model advisory proposal preserved with zero veto power.";
        result.verifiedConstraint.evidenceStatus = EvidenceStatus::Unverified;
        return result;
    }

    result.verdict = CitationVerdict::InvalidSpan;
    result.assignedStatus = EvidenceStatus::Rejected;
    result.explanation = "Unknown origin configuration.";
    return result;
}

std::vector<CitationEvaluation> CitationEvaluator::evaluateBatch(
    std::string_view promptText,
    std::string_view promptId,
    const std::vector<ScopedConstraint>& candidates) const {

    std::vector<CitationEvaluation> evaluations;
    evaluations.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        evaluations.push_back(evaluate(promptText, promptId, candidate));
    }
    return evaluations;
}

} // namespace roclint::core
