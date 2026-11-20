#include "roclint/storage/PayloadCodec.hpp"

#include "roclint/core/Json.hpp"
#include "roclint/core/schema/SchemaVisitor.hpp"
#include "roclint/core/validation_result.hpp"

#include <cmath>
#include <utility>
#include <vector>

#define ROCLINT_TRY_ASSIGN(lhs, expr)          \
    do {                                       \
        auto roclintTryResult_ = (expr);       \
        if (!roclintTryResult_.ok()) {         \
            return roclintTryResult_.error();  \
        }                                      \
        lhs = std::move(roclintTryResult_).value(); \
    } while (0)

namespace roclint::storage {

namespace json = core::json;
using core::ErrorCode;
using core::makeError;
using core::Result;
using json::Value;
namespace an = core::analysis;
namespace in = core::intent;
namespace rw = core::rewrite;

namespace {

constexpr json::ParseLimits kPayloadLimits{4 * 1024 * 1024, 16};

Value str(const std::string& s) { return Value::string(s); }
Value num(std::uint64_t n) { return Value::number(static_cast<double>(n)); }
Value flag(bool b) { return Value::boolean(b); }

Value strList(const std::vector<std::string>& values) {
    Value arr = Value::array();
    for (const auto& v : values) arr.push(str(v));
    return arr;
}

Value spanList(const std::vector<an::SourceSpan>& spans) {
    Value arr = Value::array();
    for (const auto& s : spans) {
        Value pair = Value::array();
        pair.push(num(s.startByte));
        pair.push(num(s.endByte));
        arr.push(std::move(pair));
    }
    return arr;
}

Result<std::vector<std::string>> readStrList(const Value& obj, std::string_view key) {
    auto arr = json::requireArray(obj, key);
    if (!arr.ok()) return arr.error();
    std::vector<std::string> out;
    for (const auto& v : arr.value()->asArray()) {
        if (!v.isString()) return makeError(ErrorCode::InvalidInput, "Non-string entry in '" + std::string(key) + "'");
        out.push_back(v.asString());
    }
    return out;
}

Result<std::vector<an::SourceSpan>> readSpans(const Value& obj) {
    auto arr = json::requireArray(obj, "evidence");
    if (!arr.ok()) return arr.error();
    std::vector<an::SourceSpan> out;
    for (const auto& v : arr.value()->asArray()) {
        if (!v.isArray() || v.asArray().size() != 2 || !v.asArray()[0].isNumber() || !v.asArray()[1].isNumber()) {
            return makeError(ErrorCode::InvalidInput, "Malformed evidence span");
        }
        const double s = v.asArray()[0].asNumber();
        const double e = v.asArray()[1].asNumber();
        if (s < 0 || e < 0 || std::floor(s) != s || std::floor(e) != e) {
            return makeError(ErrorCode::InvalidInput, "Malformed evidence span offsets");
        }
        out.push_back(an::SourceSpan{static_cast<std::size_t>(s), static_cast<std::size_t>(e)});
    }
    return out;
}

Result<std::int64_t> readInt64(const Value& obj, std::string_view key) {
    auto v = json::requireUInt(obj, key);
    if (!v.ok()) return v.error();
    return static_cast<std::int64_t>(v.value());
}

Result<in::IntentResolutionKind> kindFromString(const std::string& s) {
    using K = in::IntentResolutionKind;
    for (const K k : {K::SelectedAlternative, K::CustomText, K::UseOriginal, K::AcceptedAssumption,
                      K::DeclinedAssumption}) {
        if (s == in::toString(k)) return k;
    }
    return makeError(ErrorCode::InvalidInput, "Unknown resolution kind '" + s + "'");
}

Result<rw::ProposalStatus> proposalStatusFromString(const std::string& s) {
    using P = rw::ProposalStatus;
    for (const P p : {P::ProposalReady, P::NeedsClarification, P::StaleProposal, P::Rejected}) {
        if (s == rw::toString(p)) return p;
    }
    return makeError(ErrorCode::InvalidInput, "Unknown proposal status '" + s + "'");
}

Result<rw::ConstraintStatus> constraintStatusFromString(const std::string& s) {
    using C = rw::ConstraintStatus;
    for (const C c : {C::MechanicallyVerified, C::UserConfirmed, C::SemanticReviewRequired, C::Failed}) {
        if (s == rw::toString(c)) return c;
    }
    return makeError(ErrorCode::InvalidInput, "Unknown constraint status '" + s + "'");
}

} // namespace

std::string encodePromptTriage(const an::PromptTriage& t) {
    Value root = Value::object();
    root.set("schema", str("prompt_triage.v1"));
    root.set("id", str(t.id)); root.set("hash", str(t.triageHash));
    root.set("hash_version", num(static_cast<std::uint64_t>(t.hashVersion)));
    core::schema::JsonEncodeVisitor visitor(root);
    t.visitEvidence(visitor);
    return json::serialize(root);
}

Result<an::PromptTriage> decodePromptTriage(std::string_view payload) {
    auto parsed = json::parse(payload, kPayloadLimits);
    if (!parsed.ok()) return parsed.error();
    const auto& root = parsed.value();
    std::string schema;
    ROCLINT_TRY_ASSIGN(schema, json::requireString(root, "schema"));
    if (schema != "prompt_triage.v1") return makeError(ErrorCode::InvalidInput, "Unknown prompt classification schema");
    an::PromptTriage t;
    ROCLINT_TRY_ASSIGN(t.id, json::requireString(root, "id"));
    ROCLINT_TRY_ASSIGN(t.triageHash, json::requireString(root, "hash"));
    // hash_version is absent in records written before Phase 1; treat missing as V1 for backwards compatibility.
    if (const auto* hv = root.find("hash_version")) {
        if (!hv->isNumber()) return makeError(ErrorCode::InvalidInput, "Invalid hash_version field");
        const auto val = hv->asNumber();
        if (val != 1 && val != 2 && val != 3) return makeError(ErrorCode::InvalidInput, "Unknown hash_version value");
        t.hashVersion = static_cast<an::TriageHashVersion>(val);
    } else {
        t.hashVersion = an::TriageHashVersion::V1; // historical record pre-dating Phase 1
    }
    core::schema::JsonDecodeVisitor visitor(root);
    t.visitEvidence(visitor);
    if (!visitor.status().ok()) return visitor.status().error();
    if (const auto status = an::validatePromptTriage(t); !status.ok()) return status.error();
    return t;
}

std::string encodeAnalysisSnapshot(const an::AnalysisSnapshot& snapshot) {
    Value root = Value::object();
    root.set("schema", str(snapshot.payloadVersion == 2 ? "analysis_snapshot.v2" :
        snapshot.payloadVersion == 3 ? "analysis_snapshot.v3" :
        snapshot.payloadVersion == 4 ? "analysis_snapshot.v4" : "analysis_snapshot.v5"));
    if (snapshot.payloadVersion >= 4 && snapshot.promptTriage)
        root.set("prompt_triage_payload", str(encodePromptTriage(*snapshot.promptTriage)));
    if (snapshot.payloadVersion >= 5) {
        root.set("review_trigger", num(static_cast<unsigned>(snapshot.reviewPolicy.trigger)));
        root.set("assistance_kind", num(static_cast<unsigned>(snapshot.reviewPolicy.assistance)));
    }
    root.set("revision_text_hash", str(snapshot.revisionTextHash));
    root.set("id", str(snapshot.id));
    root.set("snapshot_hash", str(snapshot.snapshotHash));
    root.set("normalized_payload", str(encodeAnalysis(snapshot.normalized)));
    root.set("discovery_attempt_id", str(snapshot.discoveryAttemptId));
    root.set("scorer_attempt_id", str(snapshot.scorerAttemptId));
    root.set("presentation_policy", str(snapshot.presentationPolicyVersion));
    const auto& e = snapshot.evaluation;
    Value evaluation = Value::object();
    evaluation.set("success", flag(e.success));
    evaluation.set("error", str(e.errorMessage));
    evaluation.set("degraded_reason", str(e.degradedReason));
    evaluation.set("normalized_id", str(e.analysisSnapshotId));
    evaluation.set("engine", num(static_cast<std::uint64_t>(e.engine)));
    evaluation.set("execution_status", num(static_cast<std::uint64_t>(e.executionStatus)));
    evaluation.set("executed_engine", e.executedEngine ? num(static_cast<std::uint64_t>(*e.executedEngine)) : Value::null());
    evaluation.set("model_version", str(e.modelVersion));
    evaluation.set("model_inference_executed", flag(e.modelInferenceExecuted));
    evaluation.set("latency_ms", Value::number(e.totalLatencyMs));
    evaluation.set("confidence_threshold", Value::number(e.confidenceThreshold));
    evaluation.set("threshold_version", str(e.thresholdVersion));
    Value groups = Value::array();
    for (const auto& score : e.groupScores) {
        Value value = Value::object();
        value.set("group_id", str(score.groupId));
        value.set("scored", flag(score.isScored));
        value.set("noul_certainty", flag(score.noulCertainty));
        value.set("ambiguity", score.ambiguityScore ? Value::number(*score.ambiguityScore) : Value::null());
        value.set("winner", score.winningAlternativeId ? str(*score.winningAlternativeId) : Value::null());
        Value probabilities = Value::null();
        if (score.probabilities) {
            probabilities = Value::array();
            for (const auto& probability : *score.probabilities) {
                Value item = Value::object();
                item.set("alternative_id", str(probability.alternativeId));
                item.set("probability", Value::number(probability.probability));
                probabilities.push(std::move(item));
            }
        }
        value.set("probabilities", std::move(probabilities));
        groups.push(std::move(value));
    }
    evaluation.set("groups", std::move(groups));
    if (snapshot.payloadVersion >= 3) {
        evaluation.set("plan_version", num(static_cast<std::uint64_t>(e.planVersion)));
        Value assumptions = Value::array();
        for (const auto& score : e.assumptionScores) {
            Value item = Value::object();
            item.set("assumption_id", str(score.assumptionId));
            item.set("scored", flag(score.isScored));
            item.set("noul_certainty", flag(score.noulCertainty));
            item.set("unscored_reason", str(score.unscoredReason));
            item.set("uncertainty", score.uncertaintyScore ? Value::number(*score.uncertaintyScore) : Value::null());
            assumptions.push(std::move(item));
        }
        evaluation.set("assumptions", std::move(assumptions));
        root.set("assumption_order", strList(snapshot.assumptionOrder));
    }
    root.set("evaluation", std::move(evaluation));
    Value presentation = Value::array();
    for (const auto& group : snapshot.presentation) {
        Value value = Value::object();
        value.set("group_id", str(group.groupId));
        value.set("alternative_ids", strList(group.alternativeIds));
        presentation.push(std::move(value));
    }
    root.set("presentation", std::move(presentation));
    return json::serialize(root);
}

Result<an::AnalysisSnapshot> decodeAnalysisSnapshot(std::string_view payload) {
    auto parsed = json::parse(payload, kPayloadLimits);
    if (!parsed.ok()) return parsed.error();
    const auto& root = parsed.value();
    std::string schema, normalized;
    ROCLINT_TRY_ASSIGN(schema, json::requireString(root, "schema"));
    if (schema != "analysis_snapshot.v2" && schema != "analysis_snapshot.v3" && schema != "analysis_snapshot.v4" && schema != "analysis_snapshot.v5") return makeError(ErrorCode::InvalidInput, "Unsupported final analysis schema");
    an::AnalysisSnapshot snapshot;
    snapshot.payloadVersion = schema == "analysis_snapshot.v2" ? 2 : schema == "analysis_snapshot.v3" ? 3 : schema == "analysis_snapshot.v4" ? 4 : 5;
    if (snapshot.payloadVersion >= 4) {
        std::string triagePayload;
        ROCLINT_TRY_ASSIGN(triagePayload, json::requireString(root, "prompt_triage_payload"));
        an::PromptTriage triage;
        ROCLINT_TRY_ASSIGN(triage, decodePromptTriage(triagePayload));
        snapshot.promptTriage = std::move(triage);
    } else if (root.find("prompt_triage_payload")) return makeError(ErrorCode::InvalidInput, "Historical analysis cannot claim classifier evidence");
    if (snapshot.payloadVersion >= 5) {
        std::uint64_t trigger = 0, assistance = 0;
        ROCLINT_TRY_ASSIGN(trigger, json::requireUInt(root, "review_trigger"));
        ROCLINT_TRY_ASSIGN(assistance, json::requireUInt(root, "assistance_kind"));
        if (trigger > 1 || assistance > 4) return makeError(ErrorCode::InvalidInput, "Unknown review policy");
        snapshot.reviewPolicy = {static_cast<an::ReviewTrigger>(trigger), static_cast<an::AssistanceKind>(assistance)};
    } else if (root.find("review_trigger") || root.find("assistance_kind"))
        return makeError(ErrorCode::InvalidInput, "Historical analysis cannot claim a review policy");
    ROCLINT_TRY_ASSIGN(snapshot.revisionTextHash, json::requireString(root, "revision_text_hash"));
    ROCLINT_TRY_ASSIGN(snapshot.id, json::requireString(root, "id"));
    ROCLINT_TRY_ASSIGN(snapshot.snapshotHash, json::requireString(root, "snapshot_hash"));
    ROCLINT_TRY_ASSIGN(normalized, json::requireString(root, "normalized_payload"));
    ROCLINT_TRY_ASSIGN(snapshot.normalized, decodeAnalysis(normalized));
    ROCLINT_TRY_ASSIGN(snapshot.discoveryAttemptId, json::requireString(root, "discovery_attempt_id"));
    ROCLINT_TRY_ASSIGN(snapshot.scorerAttemptId, json::requireString(root, "scorer_attempt_id"));
    ROCLINT_TRY_ASSIGN(snapshot.presentationPolicyVersion, json::requireString(root, "presentation_policy"));
    const auto* value = root.find("evaluation");
    if (!value || !value->isObject()) return makeError(ErrorCode::InvalidInput, "Missing analysis evaluation");
    const auto& evaluation = *value;
    auto& e = snapshot.evaluation;
    ROCLINT_TRY_ASSIGN(e.success, json::requireBool(evaluation, "success"));
    ROCLINT_TRY_ASSIGN(e.errorMessage, json::requireString(evaluation, "error"));
    ROCLINT_TRY_ASSIGN(e.degradedReason, json::requireString(evaluation, "degraded_reason"));
    ROCLINT_TRY_ASSIGN(e.analysisSnapshotId, json::requireString(evaluation, "normalized_id"));
    std::uint64_t engine = 0, status = 0;
    ROCLINT_TRY_ASSIGN(engine, json::requireUInt(evaluation, "engine"));
    ROCLINT_TRY_ASSIGN(status, json::requireUInt(evaluation, "execution_status"));
    if (engine > 1 || status > (snapshot.payloadVersion >= 4 ? 3U : 2U)) return makeError(ErrorCode::InvalidInput, "Unknown scoring metadata");
    e.engine = static_cast<an::ScorerEngine>(engine);
    e.executionStatus = static_cast<an::ExecutionStatus>(status);
    const auto* executed = evaluation.find("executed_engine");
    if (!executed) return makeError(ErrorCode::InvalidInput, "Missing executed engine");
    if (!executed->isNull()) {
        std::uint64_t result = 0;
        ROCLINT_TRY_ASSIGN(result, json::requireUInt(evaluation, "executed_engine"));
        if (result > 1) return makeError(ErrorCode::InvalidInput, "Unknown executed engine");
        e.executedEngine = static_cast<an::ScorerEngine>(result);
    }
    ROCLINT_TRY_ASSIGN(e.modelVersion, json::requireString(evaluation, "model_version"));
    ROCLINT_TRY_ASSIGN(e.modelInferenceExecuted, json::requireBool(evaluation, "model_inference_executed"));
    ROCLINT_TRY_ASSIGN(e.thresholdVersion, json::requireString(evaluation, "threshold_version"));
    auto readNumber = [](const Value& object, std::string_view key) -> Result<double> {
        const auto* number = object.find(key);
        if (!number || !number->isNumber() || !std::isfinite(number->asNumber())) return makeError(ErrorCode::InvalidInput, "Expected finite numeric score metadata");
        return number->asNumber();
    };
    ROCLINT_TRY_ASSIGN(e.totalLatencyMs, readNumber(evaluation, "latency_ms"));
    ROCLINT_TRY_ASSIGN(e.confidenceThreshold, readNumber(evaluation, "confidence_threshold"));
    const Value* groups = nullptr;
    ROCLINT_TRY_ASSIGN(groups, json::requireArray(evaluation, "groups"));
    for (const auto& group : groups->asArray()) {
        an::GroupEvaluationScore score;
        ROCLINT_TRY_ASSIGN(score.groupId, json::requireString(group, "group_id"));
        ROCLINT_TRY_ASSIGN(score.isScored, json::requireBool(group, "scored"));
        ROCLINT_TRY_ASSIGN(score.noulCertainty, json::requireBool(group, "noul_certainty"));
        const auto* ambiguity = group.find("ambiguity");
        const auto* winner = group.find("winner");
        const auto* probabilities = group.find("probabilities");
        if (!ambiguity || !winner || !probabilities) return makeError(ErrorCode::InvalidInput, "Incomplete score payload");
        if (!ambiguity->isNull()) { double number = 0; ROCLINT_TRY_ASSIGN(number, readNumber(group, "ambiguity")); score.ambiguityScore = number; }
        if (!winner->isNull()) { std::string id; ROCLINT_TRY_ASSIGN(id, json::requireString(group, "winner")); score.winningAlternativeId = id; }
        if (!probabilities->isNull()) {
            if (!probabilities->isArray()) return makeError(ErrorCode::InvalidInput, "Malformed probabilities");
            score.probabilities.emplace();
            for (const auto& probability : probabilities->asArray()) {
                an::AlternativeProbability result;
                ROCLINT_TRY_ASSIGN(result.alternativeId, json::requireString(probability, "alternative_id"));
                ROCLINT_TRY_ASSIGN(result.probability, readNumber(probability, "probability"));
                score.probabilities->push_back(std::move(result));
            }
        }
        e.groupScores.push_back(std::move(score));
    }
    if (snapshot.payloadVersion >= 3) {
        std::uint64_t plan = 0;
        ROCLINT_TRY_ASSIGN(plan, json::requireUInt(evaluation, "plan_version"));
        if (plan != 2) return makeError(ErrorCode::InvalidInput, "Unsupported evaluation plan");
        e.planVersion = 2;
        const Value* assumptions = nullptr;
        ROCLINT_TRY_ASSIGN(assumptions, json::requireArray(evaluation, "assumptions"));
        for (const auto& item : assumptions->asArray()) {
            an::AssumptionEvaluationScore score;
            ROCLINT_TRY_ASSIGN(score.assumptionId, json::requireString(item, "assumption_id"));
            ROCLINT_TRY_ASSIGN(score.isScored, json::requireBool(item, "scored"));
            ROCLINT_TRY_ASSIGN(score.noulCertainty, json::requireBool(item, "noul_certainty"));
            ROCLINT_TRY_ASSIGN(score.unscoredReason, json::requireString(item, "unscored_reason"));
            const auto* uncertainty = item.find("uncertainty");
            if (!uncertainty) return makeError(ErrorCode::InvalidInput, "Missing assumption uncertainty");
            if (!uncertainty->isNull()) { double number = 0; ROCLINT_TRY_ASSIGN(number, readNumber(item, "uncertainty")); score.uncertaintyScore = number; }
            e.assumptionScores.push_back(std::move(score));
        }
        ROCLINT_TRY_ASSIGN(snapshot.assumptionOrder, readStrList(root, "assumption_order"));
    } else {
        e.planVersion = 1;
        if (evaluation.find("plan_version") || evaluation.find("assumptions") || root.find("assumption_order")) return makeError(ErrorCode::InvalidInput, "Legacy schema cannot carry new scoring metadata");
    }
    const Value* presentation = nullptr;
    ROCLINT_TRY_ASSIGN(presentation, json::requireArray(root, "presentation"));
    for (const auto& valueGroup : presentation->asArray()) {
        an::GroupPresentation group;
        ROCLINT_TRY_ASSIGN(group.groupId, json::requireString(valueGroup, "group_id"));
        ROCLINT_TRY_ASSIGN(group.alternativeIds, readStrList(valueGroup, "alternative_ids"));
        snapshot.presentation.push_back(std::move(group));
    }
    if (auto statusResult = an::validateAnalysisSnapshot(snapshot); !statusResult.ok()) return statusResult.error();
    return snapshot;
}

std::string encodeAnalysis(const an::NormalizedAnalysis& a) {
    Value root = Value::object();
    root.set("schema", str("analysis.v1"));
    root.set("id", str(a.id));
    root.set("session_id", str(a.sessionId));
    root.set("base_revision_id", str(a.baseRevisionId));
    root.set("snapshot_hash", str(a.snapshotHash));

    Value groups = Value::array();
    for (const auto& g : a.choiceGroups) {
        Value gv = Value::object();
        gv.set("id", str(g.id));
        gv.set("dimension", str(g.dimension));
        gv.set("question", str(g.question));
        gv.set("material", flag(g.material));
        Value alts = Value::array();
        for (const auto& alt : g.alternatives) {
            Value av = Value::object();
            av.set("id", str(alt.id));
            av.set("interpretation", str(alt.interpretation));
            av.set("why_material", str(alt.whyMaterial));
            av.set("canonical_semantics_json", str(alt.canonicalSemanticsJson));
            av.set("evidence", spanList(alt.evidence));
            alts.push(std::move(av));
        }
        gv.set("alternatives", std::move(alts));
        groups.push(std::move(gv));
    }
    root.set("choice_groups", std::move(groups));

    Value assumptions = Value::array();
    for (const auto& x : a.assumptions) {
        Value v = Value::object();
        v.set("id", str(x.id));
        v.set("statement", str(x.statement));
        v.set("material", flag(x.material));
        v.set("evidence", spanList(x.evidence));
        assumptions.push(std::move(v));
    }
    root.set("assumptions", std::move(assumptions));

    Value missing = Value::array();
    for (const auto& x : a.missingDecisions) {
        Value v = Value::object();
        v.set("id", str(x.id));
        v.set("question", str(x.question));
        v.set("blocking", flag(x.blocking));
        if (!x.suggestedAlternativeEvidence.empty())
            v.set("suggested_alternative_evidence", str(x.suggestedAlternativeEvidence));
        missing.push(std::move(v));
    }
    root.set("missing_decisions", std::move(missing));

    Value conflicts = Value::array();
    for (const auto& x : a.conflicts) {
        Value v = Value::object();
        v.set("id", str(x.id));
        v.set("description", str(x.description));
        v.set("blocking", flag(x.blocking));
        conflicts.push(std::move(v));
    }
    root.set("conflicts", std::move(conflicts));
    return json::serialize(root);
}

Result<an::NormalizedAnalysis> decodeAnalysis(std::string_view payload) {
    auto parsed = json::parse(payload, kPayloadLimits);
    if (!parsed.ok()) return parsed.error();
    const Value& root = parsed.value();
    std::string schema;
    ROCLINT_TRY_ASSIGN(schema, json::requireString(root, "schema"));
    if (schema != "analysis.v1") return makeError(ErrorCode::InvalidInput, "Unsupported normalized analysis schema");
    an::NormalizedAnalysis a;
    ROCLINT_TRY_ASSIGN(a.id, json::requireString(root, "id"));
    ROCLINT_TRY_ASSIGN(a.sessionId, json::requireString(root, "session_id"));
    ROCLINT_TRY_ASSIGN(a.baseRevisionId, json::requireString(root, "base_revision_id"));
    ROCLINT_TRY_ASSIGN(a.snapshotHash, json::requireString(root, "snapshot_hash"));

    const Value* groups = nullptr;
    ROCLINT_TRY_ASSIGN(groups, json::requireArray(root, "choice_groups"));
    for (const auto& gv : groups->asArray()) {
        an::ChoiceGroup g;
        ROCLINT_TRY_ASSIGN(g.id, json::requireString(gv, "id"));
        ROCLINT_TRY_ASSIGN(g.dimension, json::requireString(gv, "dimension"));
        ROCLINT_TRY_ASSIGN(g.question, json::requireString(gv, "question"));
        ROCLINT_TRY_ASSIGN(g.material, json::requireBool(gv, "material"));
        const Value* alts = nullptr;
        ROCLINT_TRY_ASSIGN(alts, json::requireArray(gv, "alternatives"));
        for (const auto& av : alts->asArray()) {
            an::ChoiceAlternative alt;
            ROCLINT_TRY_ASSIGN(alt.id, json::requireString(av, "id"));
            ROCLINT_TRY_ASSIGN(alt.interpretation, json::requireString(av, "interpretation"));
            ROCLINT_TRY_ASSIGN(alt.whyMaterial, json::requireString(av, "why_material"));
            ROCLINT_TRY_ASSIGN(alt.canonicalSemanticsJson, json::requireString(av, "canonical_semantics_json"));
            ROCLINT_TRY_ASSIGN(alt.evidence, readSpans(av));
            g.alternatives.push_back(std::move(alt));
        }
        a.choiceGroups.push_back(std::move(g));
    }

    const Value* assumptions = nullptr;
    ROCLINT_TRY_ASSIGN(assumptions, json::requireArray(root, "assumptions"));
    for (const auto& v : assumptions->asArray()) {
        an::Assumption x;
        ROCLINT_TRY_ASSIGN(x.id, json::requireString(v, "id"));
        ROCLINT_TRY_ASSIGN(x.statement, json::requireString(v, "statement"));
        ROCLINT_TRY_ASSIGN(x.material, json::requireBool(v, "material"));
        ROCLINT_TRY_ASSIGN(x.evidence, readSpans(v));
        a.assumptions.push_back(std::move(x));
    }

    const Value* missing = nullptr;
    ROCLINT_TRY_ASSIGN(missing, json::requireArray(root, "missing_decisions"));
    for (const auto& v : missing->asArray()) {
        an::MissingDecision x;
        ROCLINT_TRY_ASSIGN(x.id, json::requireString(v, "id"));
        ROCLINT_TRY_ASSIGN(x.question, json::requireString(v, "question"));
        ROCLINT_TRY_ASSIGN(x.blocking, json::requireBool(v, "blocking"));
        if (v.find("suggested_alternative_evidence") != nullptr)
            ROCLINT_TRY_ASSIGN(x.suggestedAlternativeEvidence, json::requireString(v, "suggested_alternative_evidence"));
        a.missingDecisions.push_back(std::move(x));
    }

    const Value* conflicts = nullptr;
    ROCLINT_TRY_ASSIGN(conflicts, json::requireArray(root, "conflicts"));
    for (const auto& v : conflicts->asArray()) {
        an::Conflict x;
        ROCLINT_TRY_ASSIGN(x.id, json::requireString(v, "id"));
        ROCLINT_TRY_ASSIGN(x.description, json::requireString(v, "description"));
        ROCLINT_TRY_ASSIGN(x.blocking, json::requireBool(v, "blocking"));
        a.conflicts.push_back(std::move(x));
    }

    if (!an::isSnapshotSealed(a)) {
        return makeError(ErrorCode::ValidationFailed, "Stored analysis snapshot hash does not match its contents");
    }
    return a;
}

std::string encodeIntent(const in::ConfirmedIntent& i) {
    Value root = Value::object();
    root.set("schema", str("intent.v1"));
    root.set("id", str(i.id));
    root.set("intent_hash", str(i.intentHash));
    root.set("session_id", str(i.sessionId));
    root.set("base_revision_id", str(i.baseRevisionId));
    root.set("snapshot_id", str(i.snapshotId));
    root.set("snapshot_hash", str(i.snapshotHash));
    root.set("visible_item_ids", strList(i.visibleItemIds));
    Value items = Value::array();
    for (const auto& item : i.items) {
        Value v = Value::object();
        v.set("item_id", str(item.itemId));
        v.set("kind", str(in::toString(item.kind)));
        v.set("chosen_alternative_id", str(item.chosenAlternativeId));
        v.set("chosen_value", str(item.chosenValue));
        v.set("custom_text", str(item.customText));
        v.set("rationale", str(item.rationale));
        items.push(std::move(v));
    }
    root.set("items", std::move(items));
    root.set("unresolved_item_ids", strList(i.unresolvedItemIds));
    root.set("rejected_item_ids", strList(i.rejectedItemIds));
    root.set("created_at_ms", num(static_cast<std::uint64_t>(i.createdAtMs < 0 ? 0 : i.createdAtMs)));
    return json::serialize(root);
}

Result<in::ConfirmedIntent> decodeIntent(std::string_view payload) {
    auto parsed = json::parse(payload, kPayloadLimits);
    if (!parsed.ok()) return parsed.error();
    const Value& root = parsed.value();
    in::ConfirmedIntent i;
    ROCLINT_TRY_ASSIGN(i.id, json::requireString(root, "id"));
    ROCLINT_TRY_ASSIGN(i.intentHash, json::requireString(root, "intent_hash"));
    ROCLINT_TRY_ASSIGN(i.sessionId, json::requireString(root, "session_id"));
    ROCLINT_TRY_ASSIGN(i.baseRevisionId, json::requireString(root, "base_revision_id"));
    ROCLINT_TRY_ASSIGN(i.snapshotId, json::requireString(root, "snapshot_id"));
    ROCLINT_TRY_ASSIGN(i.snapshotHash, json::requireString(root, "snapshot_hash"));
    ROCLINT_TRY_ASSIGN(i.visibleItemIds, readStrList(root, "visible_item_ids"));
    const Value* items = nullptr;
    ROCLINT_TRY_ASSIGN(items, json::requireArray(root, "items"));
    for (const auto& v : items->asArray()) {
        in::ConfirmedItem item;
        std::string kind;
        ROCLINT_TRY_ASSIGN(item.itemId, json::requireString(v, "item_id"));
        ROCLINT_TRY_ASSIGN(kind, json::requireString(v, "kind"));
        ROCLINT_TRY_ASSIGN(item.kind, kindFromString(kind));
        ROCLINT_TRY_ASSIGN(item.chosenAlternativeId, json::requireString(v, "chosen_alternative_id"));
        ROCLINT_TRY_ASSIGN(item.chosenValue, json::requireString(v, "chosen_value"));
        ROCLINT_TRY_ASSIGN(item.customText, json::requireString(v, "custom_text"));
        ROCLINT_TRY_ASSIGN(item.rationale, json::requireString(v, "rationale"));
        i.items.push_back(std::move(item));
    }
    ROCLINT_TRY_ASSIGN(i.unresolvedItemIds, readStrList(root, "unresolved_item_ids"));
    ROCLINT_TRY_ASSIGN(i.rejectedItemIds, readStrList(root, "rejected_item_ids"));
    ROCLINT_TRY_ASSIGN(i.createdAtMs, readInt64(root, "created_at_ms"));

    const std::string recomputed = in::IntentProvenanceBuilder::computeIntentHash(i);
    if (recomputed != i.intentHash || i.id != "intent_" + recomputed) {
        return makeError(ErrorCode::ValidationFailed, "Stored intent hash does not match its contents");
    }
    return i;
}

std::string encodeProposal(const rw::RewriteProposal& p) {
    Value root = Value::object();
    root.set("schema", str("proposal.v1"));
    root.set("id", str(p.id));
    root.set("proposal_hash", str(p.proposalHash));
    root.set("session_id", str(p.sessionId));
    root.set("base_revision_id", str(p.baseRevisionId));
    root.set("intent_id", str(p.intentId));
    root.set("intent_hash", str(p.intentHash));
    root.set("snapshot_id", str(p.snapshotId));
    root.set("snapshot_hash", str(p.snapshotHash));
    root.set("ast_snapshot_hash", str(p.astSnapshotHash));
    root.set("rewrite_profile_id", str(p.rewriteProfileId));
    root.set("model_id", str(p.modelId));
    root.set("model_digest", str(p.modelDigest));
    root.set("quantization", str(p.quantization));
    root.set("transport", str(p.transport));
    root.set("original_text", str(p.originalText));
    root.set("rewritten_text", str(p.rewrittenText));
    root.set("diff_text", str(p.diffText));
    root.set("clarification_question", str(p.clarificationQuestion));
    root.set("status", str(rw::toString(p.status)));
    Value constraints = Value::array();
    for (const auto& c : p.constraints) {
        Value v = Value::object();
        v.set("id", str(c.id));
        v.set("description", str(c.description));
        v.set("status", str(rw::toString(c.status)));
        v.set("evidence", str(c.evidence));
        constraints.push(std::move(v));
    }
    root.set("constraints", std::move(constraints));
    root.set("proposal_edit_generation", num(p.proposalEditGeneration));
    root.set("created_at_ms", num(static_cast<std::uint64_t>(p.createdAtMs < 0 ? 0 : p.createdAtMs)));
    if (p.auditResult.has_value()) {
        root.set("audit_result", core::toJson(*p.auditResult));
    }
    return json::serialize(root);
}

Result<rw::RewriteProposal> decodeProposal(std::string_view payload) {
    auto parsed = json::parse(payload, kPayloadLimits);
    if (!parsed.ok()) return parsed.error();
    const Value& root = parsed.value();
    rw::RewriteProposal p;
    std::string status;
    ROCLINT_TRY_ASSIGN(p.id, json::requireString(root, "id"));
    ROCLINT_TRY_ASSIGN(p.proposalHash, json::requireString(root, "proposal_hash"));
    ROCLINT_TRY_ASSIGN(p.sessionId, json::requireString(root, "session_id"));
    ROCLINT_TRY_ASSIGN(p.baseRevisionId, json::requireString(root, "base_revision_id"));
    ROCLINT_TRY_ASSIGN(p.intentId, json::requireString(root, "intent_id"));
    ROCLINT_TRY_ASSIGN(p.intentHash, json::requireString(root, "intent_hash"));
    ROCLINT_TRY_ASSIGN(p.snapshotId, json::requireString(root, "snapshot_id"));
    ROCLINT_TRY_ASSIGN(p.snapshotHash, json::requireString(root, "snapshot_hash"));
    // Absent on proposals written before AST grounding; those keep the v1 domain.
    if (const auto* astHash = root.find("ast_snapshot_hash")) {
        if (!astHash->isString()) return makeError(ErrorCode::InvalidInput, "Invalid ast_snapshot_hash field");
        p.astSnapshotHash = astHash->asString();
    }
    ROCLINT_TRY_ASSIGN(p.rewriteProfileId, json::requireString(root, "rewrite_profile_id"));
    ROCLINT_TRY_ASSIGN(p.modelId, json::requireString(root, "model_id"));
    ROCLINT_TRY_ASSIGN(p.modelDigest, json::requireString(root, "model_digest"));
    ROCLINT_TRY_ASSIGN(p.quantization, json::requireString(root, "quantization"));
    ROCLINT_TRY_ASSIGN(p.transport, json::requireString(root, "transport"));
    ROCLINT_TRY_ASSIGN(p.originalText, json::requireString(root, "original_text"));
    ROCLINT_TRY_ASSIGN(p.rewrittenText, json::requireString(root, "rewritten_text"));
    ROCLINT_TRY_ASSIGN(p.diffText, json::requireString(root, "diff_text"));
    ROCLINT_TRY_ASSIGN(p.clarificationQuestion, json::requireString(root, "clarification_question"));
    ROCLINT_TRY_ASSIGN(status, json::requireString(root, "status"));
    ROCLINT_TRY_ASSIGN(p.status, proposalStatusFromString(status));
    const Value* constraints = nullptr;
    ROCLINT_TRY_ASSIGN(constraints, json::requireArray(root, "constraints"));
    for (const auto& v : constraints->asArray()) {
        rw::ConstraintItem c;
        std::string cs;
        ROCLINT_TRY_ASSIGN(c.id, json::requireString(v, "id"));
        ROCLINT_TRY_ASSIGN(c.description, json::requireString(v, "description"));
        ROCLINT_TRY_ASSIGN(cs, json::requireString(v, "status"));
        ROCLINT_TRY_ASSIGN(c.status, constraintStatusFromString(cs));
        ROCLINT_TRY_ASSIGN(c.evidence, json::requireString(v, "evidence"));
        p.constraints.push_back(std::move(c));
    }
    ROCLINT_TRY_ASSIGN(p.proposalEditGeneration, json::requireUInt(root, "proposal_edit_generation"));
    ROCLINT_TRY_ASSIGN(p.createdAtMs, readInt64(root, "created_at_ms"));
    if (const auto* auditVal = root.find("audit_result")) {
        auto auditRes = core::validationResultFromJson(*auditVal);
        if (!auditRes.ok()) return auditRes.error();
        p.auditResult = std::move(auditRes).value();
    }

    const std::string recomputed = rw::ProposalHasher::computeProposalHash(p);
    if (recomputed != p.proposalHash || !p.id.starts_with("prop_") || p.id.empty()) {
        return makeError(ErrorCode::ValidationFailed, "Stored proposal hash does not match its contents");
    }
    return p;
}

} // namespace roclint::storage
