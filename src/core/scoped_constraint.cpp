#include "roclint/core/scoped_constraint.hpp"

namespace roclint::core {

const char* toString(Origin origin) noexcept {
    switch (origin) {
        case Origin::User: return "User";
        case Origin::Domain: return "Domain";
        case Origin::Environment: return "Environment";
        case Origin::Model: return "Model";
    }
    return "Model";
}

const char* toString(Derivation derivation) noexcept {
    switch (derivation) {
        case Derivation::Direct: return "Direct";
        case Derivation::Paraphrased: return "Paraphrased";
        case Derivation::Entailed: return "Entailed";
        case Derivation::Assumed: return "Assumed";
    }
    return "Assumed";
}

const char* toString(Enforcement enforcement) noexcept {
    switch (enforcement) {
        case Enforcement::Hard: return "Hard";
        case Enforcement::Conditional: return "Conditional";
        case Enforcement::Advisory: return "Advisory";
    }
    return "Advisory";
}

const char* toString(Polarity polarity) noexcept {
    switch (polarity) {
        case Polarity::Require: return "Require";
        case Polarity::Prohibit: return "Prohibit";
    }
    return "Require";
}

const char* toString(EvidenceStatus status) noexcept {
    switch (status) {
        case EvidenceStatus::Unverified: return "Unverified";
        case EvidenceStatus::Supported: return "Supported";
        case EvidenceStatus::Rejected: return "Rejected";
    }
    return "Unverified";
}

Result<Origin> parseOrigin(std::string_view text) noexcept {
    if (text == "User") return Origin::User;
    if (text == "Domain") return Origin::Domain;
    if (text == "Environment") return Origin::Environment;
    if (text == "Model") return Origin::Model;
    return makeError(ErrorCode::InvalidInput, "Unknown Origin: " + std::string(text));
}

Result<Derivation> parseDerivation(std::string_view text) noexcept {
    if (text == "Direct") return Derivation::Direct;
    if (text == "Paraphrased") return Derivation::Paraphrased;
    if (text == "Entailed") return Derivation::Entailed;
    if (text == "Assumed") return Derivation::Assumed;
    return makeError(ErrorCode::InvalidInput, "Unknown Derivation: " + std::string(text));
}

Result<Enforcement> parseEnforcement(std::string_view text) noexcept {
    if (text == "Hard") return Enforcement::Hard;
    if (text == "Conditional") return Enforcement::Conditional;
    if (text == "Advisory") return Enforcement::Advisory;
    return makeError(ErrorCode::InvalidInput, "Unknown Enforcement: " + std::string(text));
}

Result<Polarity> parsePolarity(std::string_view text) noexcept {
    if (text == "Require") return Polarity::Require;
    if (text == "Prohibit") return Polarity::Prohibit;
    return makeError(ErrorCode::InvalidInput, "Unknown Polarity: " + std::string(text));
}

Result<EvidenceStatus> parseEvidenceStatus(std::string_view text) noexcept {
    if (text == "Unverified") return EvidenceStatus::Unverified;
    if (text == "Supported") return EvidenceStatus::Supported;
    if (text == "Rejected") return EvidenceStatus::Rejected;
    return makeError(ErrorCode::InvalidInput, "Unknown EvidenceStatus: " + std::string(text));
}

json::Value toJson(const SourceSpan& span) {
    auto obj = json::Value::object();
    obj.set("source_id", json::Value::string(span.sourceId));
    obj.set("begin_byte", json::Value::number(static_cast<double>(span.beginByte)));
    obj.set("end_byte", json::Value::number(static_cast<double>(span.endByte)));
    return obj;
}

Result<SourceSpan> sourceSpanFromJson(const json::Value& val) {
    if (!val.isObject()) {
        return makeError(ErrorCode::InvalidInput, "SourceSpan JSON must be an object");
    }
    const auto* sourceIdVal = val.find("source_id");
    const auto* beginVal = val.find("begin_byte");
    const auto* endVal = val.find("end_byte");
    if (!sourceIdVal || !sourceIdVal->isString() ||
        !beginVal || !beginVal->isNumber() ||
        !endVal || !endVal->isNumber()) {
        return makeError(ErrorCode::InvalidInput, "Missing or invalid SourceSpan fields");
    }
    const double begin = beginVal->asNumber();
    const double end = endVal->asNumber();
    if (begin < 0.0 || end < 0.0 || end < begin) {
        return makeError(ErrorCode::InvalidInput, "Invalid SourceSpan byte range");
    }
    SourceSpan span;
    span.sourceId = sourceIdVal->asString();
    span.beginByte = static_cast<std::size_t>(begin);
    span.endByte = static_cast<std::size_t>(end);
    return span;
}

json::Value toJson(const ScopedConstraint& constraint) {
    auto obj = json::Value::object();
    obj.set("id", json::Value::string(constraint.id));
    obj.set("rule", json::Value::string(constraint.rule));
    obj.set("origin", json::Value::string(toString(constraint.origin)));
    obj.set("derivation", json::Value::string(toString(constraint.derivation)));
    obj.set("enforcement", json::Value::string(toString(constraint.enforcement)));
    obj.set("polarity", json::Value::string(toString(constraint.polarity)));
    obj.set("evidence_status", json::Value::string(toString(constraint.evidenceStatus)));
    obj.set("aspect", json::Value::string(constraint.aspect));

    auto evidenceArr = json::Value::array();
    for (const auto& span : constraint.evidence) {
        evidenceArr.push(toJson(span));
    }
    obj.set("evidence", std::move(evidenceArr));

    if (constraint.appliesWhen.has_value()) {
        obj.set("applies_when", json::Value::string(*constraint.appliesWhen));
    } else {
        obj.set("applies_when", json::Value::null());
    }

    if (constraint.authorityRef.has_value()) {
        obj.set("authority_ref", json::Value::string(*constraint.authorityRef));
    } else {
        obj.set("authority_ref", json::Value::null());
    }

    return obj;
}

Result<ScopedConstraint> scopedConstraintFromJson(const json::Value& val) {
    if (!val.isObject()) {
        return makeError(ErrorCode::InvalidInput, "ScopedConstraint JSON must be an object");
    }
    const auto* idVal = val.find("id");
    const auto* ruleVal = val.find("rule");
    const auto* originVal = val.find("origin");
    const auto* derivVal = val.find("derivation");
    const auto* enfVal = val.find("enforcement");
    const auto* polVal = val.find("polarity");
    const auto* evStatVal = val.find("evidence_status");
    const auto* aspectVal = val.find("aspect");
    const auto* evidenceVal = val.find("evidence");

    if (!idVal || !idVal->isString() ||
        !ruleVal || !ruleVal->isString() ||
        !originVal || !originVal->isString() ||
        !derivVal || !derivVal->isString() ||
        !enfVal || !enfVal->isString() ||
        !polVal || !polVal->isString() ||
        !evStatVal || !evStatVal->isString() ||
        !aspectVal || !aspectVal->isString() ||
        !evidenceVal || !evidenceVal->isArray()) {
        return makeError(ErrorCode::InvalidInput, "Missing or invalid required ScopedConstraint fields");
    }

    auto origin = parseOrigin(originVal->asString());
    if (!origin.ok()) return origin.error();

    auto deriv = parseDerivation(derivVal->asString());
    if (!deriv.ok()) return deriv.error();

    auto enf = parseEnforcement(enfVal->asString());
    if (!enf.ok()) return enf.error();

    auto pol = parsePolarity(polVal->asString());
    if (!pol.ok()) return pol.error();

    auto evStat = parseEvidenceStatus(evStatVal->asString());
    if (!evStat.ok()) return evStat.error();

    ScopedConstraint sc;
    sc.id = idVal->asString();
    sc.rule = ruleVal->asString();
    sc.origin = origin.value();
    sc.derivation = deriv.value();
    sc.enforcement = enf.value();
    sc.polarity = pol.value();
    sc.evidenceStatus = evStat.value();
    sc.aspect = aspectVal->asString();

    for (const auto& spanVal : evidenceVal->asArray()) {
        auto spanRes = sourceSpanFromJson(spanVal);
        if (!spanRes.ok()) return spanRes.error();
        sc.evidence.push_back(std::move(spanRes.value()));
    }

    const auto* appliesWhenVal = val.find("applies_when");
    if (appliesWhenVal && appliesWhenVal->isString()) {
        sc.appliesWhen = appliesWhenVal->asString();
    }

    const auto* authRefVal = val.find("authority_ref");
    if (authRefVal && authRefVal->isString()) {
        sc.authorityRef = authRefVal->asString();
    }

    return sc;
}

bool canPromoteToEnforceable(const ScopedConstraint& candidate) noexcept {
    // 1. Rejected evidence can never be enforced in any capacity.
    if (candidate.evidenceStatus == EvidenceStatus::Rejected) {
        return false;
    }

    // 2. Hard enforcement requires independently verified/supported evidence.
    if (candidate.enforcement == Enforcement::Hard) {
        if (candidate.evidenceStatus != EvidenceStatus::Supported) {
            return false;
        }
    }

    // 3. User origin claims cannot be accepted on faith:
    //    They require supported evidence, non-empty source citations, and cannot be Assumed.
    if (candidate.origin == Origin::User) {
        if (candidate.evidenceStatus != EvidenceStatus::Supported ||
            candidate.evidence.empty() ||
            candidate.derivation == Derivation::Assumed) {
            return false;
        }
    }

    // 4. Model-assumed requirements can NEVER acquire Hard enforcement.
    if (candidate.origin == Origin::Model && candidate.derivation == Derivation::Assumed) {
        if (candidate.enforcement == Enforcement::Hard) {
            return false;
        }
    }

    return true;
}

} // namespace roclint::core
