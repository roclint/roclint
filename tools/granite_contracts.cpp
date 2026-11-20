#include "roclint/analysis/MaterialityPolicy.hpp"
#include "roclint/bridge/GraniteAdapter.hpp"
#include "roclint/bridge/GraniteRewriteAdapter.hpp"
#include "roclint/bridge/GraniteGoldfishAdapter.hpp"
#include "roclint/bridge/S1TriageAdapter.hpp"
#include "roclint/intent/ConfirmedIntent.hpp"
#include "roclint/rewrite/RewriteValidator.hpp"
#include "HostController.hpp"
#include "roclint/core/crypto/Sha256.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
std::string ss(const QString& value) { return value.toUtf8().toStdString(); }
QString qs(const std::string& value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }
constexpr const char* kSession = "sess_benchmark_synthetic";
constexpr const char* kRevision = "rev_sess_benchmark_synthetic_1";

roclint::bridge::GraniteRewriteRequest rewriteRequest(const QString& prompt, const QString& profileId,
                                                      const QJsonObject& fixture = {}) {
    namespace bridge = roclint::bridge;
    namespace analysis = roclint::core::analysis;
    namespace intent = roclint::core::intent;
    const auto discovery = fixture.value(QStringLiteral("discovery_reply")).toObject();
    const auto reply = discovery.isEmpty() ? std::string("{\"status\":\"no_ambiguity\"}")
        : QJsonDocument(discovery).toJson(QJsonDocument::Compact).toStdString();
    const auto normalized = bridge::parseDiscoveryResponse(reply, kSession, kRevision, ss(prompt));
    if (!normalized.ok()) throw std::runtime_error("Synthetic literal-only fixture could not be parsed");
    const auto evaluation = bridge::S1TriageAdapter::degradedResponse(normalized.value().analysis, "Synthetic fixture preparation; no scoring execution");
    const auto final = analysis::buildAnalysisSnapshot(normalized.value().analysis, evaluation, ss(prompt), "att_synthetic_fixture_preparation");
    if (!final.ok()) throw std::runtime_error("Synthetic literal-only final artifact could not be prepared");
    const auto policy = analysis::MaterialityPolicyV1::evaluatePrompt(normalized.value().analysis, ss(prompt));
    intent::WorkingIntentState working;
    const auto decisions = fixture.value(QStringLiteral("confirmed_alternatives")).toObject();
    for (auto entry = decisions.begin(); entry != decisions.end(); ++entry)
        working.confirmedAlternatives[ss(entry.key())] = ss(entry.value().toString());
    for (const auto& value : fixture.value(QStringLiteral("keep_original_items")).toArray())
        working.keptOriginalItemIds.push_back(ss(value.toString()));
    const auto confirmed = intent::IntentProvenanceBuilder::buildConfirmedIntent(final.value(), policy, working, 1);
    if (!confirmed.ok()) throw std::runtime_error("Synthetic original intent could not be bound");
    bridge::GraniteRewriteRequest request;
    request.sessionId = kSession; request.revisionId = kRevision; request.originalText = ss(prompt);
    request.analysis = final.value(); request.confirmedIntent = confirmed.value(); request.rewriteProfileId = ss(profileId);
    request.proposalEditGeneration = 1;
    return request;
}

std::filesystem::path nativePath(const QString& path) {
#ifdef _WIN32
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(ss(path));
#endif
}

void writeFixtureFiles(const QString& root, const QJsonObject& files) {
    for (auto entry = files.begin(); entry != files.end(); ++entry) {
        auto relative = entry.key();
        relative.replace(QLatin1Char('\\'), QLatin1Char('/'));
        if (relative.isEmpty() || QFileInfo(relative).isAbsolute() || relative.contains(QLatin1Char(':')) ||
            relative.split(QLatin1Char('/')).contains(QStringLiteral("..")) || !entry.value().isString())
            throw std::runtime_error("Invalid synthetic workspace path");
        const auto path = QDir(root).filePath(relative);
        if (!QDir().mkpath(QFileInfo(path).absolutePath())) throw std::runtime_error("Cannot create fixture directory");
        QFile file(path);
        const auto bytes = entry.value().toString().toUtf8();
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size())
            throw std::runtime_error("Cannot write synthetic workspace");
    }
}

roclint::host::GroundingInputs readGroundingInputs(const QJsonObject& fixture) {
    const auto binding = fixture.value(QStringLiteral("_grounding_inputs")).toObject();
    roclint::host::GroundingInputs inputs;
    inputs.prompt = ss(fixture.value(QStringLiteral("prompt")).toString());
    inputs.indexPath = nativePath(binding.value(QStringLiteral("index")).toString());
    inputs.workspaceRoot = nativePath(binding.value(QStringLiteral("root")).toString());
    inputs.workspaceIdentity = ss(binding.value(QStringLiteral("root")).toString());
    inputs.activeFile = ss(binding.value(QStringLiteral("activeFile")).toString());
    inputs.workspaceEpoch = 1;
    inputs.consented = true;
    inputs.ready = binding.value(QStringLiteral("ready")).toBool();
    return inputs;
}

QJsonObject contextView(const roclint::host::GroundedPromptContext& context) {
    QJsonArray evidence;
    for (const auto& item : context.evidence) {
        evidence.append(QJsonObject{{QStringLiteral("target"), qs(item.target)},
            {QStringLiteral("qualifiedTarget"), qs(item.qualifiedTarget)},
            {QStringLiteral("file"), qs(item.filePath)}, {QStringLiteral("status"), qs(item.status)},
            {QStringLiteral("hashValidated"), item.hashValidated}});
    }
    return {{QStringLiteral("xml"), qs(context.xml)},
        {QStringLiteral("ast_hash"), qs(context.astSnapshotHash)},
        {QStringLiteral("available"), context.available}, {QStringLiteral("complete"), context.complete},
        {QStringLiteral("validated"), context.validated}, {QStringLiteral("evidence"), evidence}};
}

QJsonObject prepareGrounding(const QJsonObject& fixture, const QString& outputRoot) {
    if (fixture.value(QStringLiteral("files")).toObject().isEmpty()) return {};
    QTemporaryDir owned(QDir(outputRoot).filePath(QStringLiteral("grounding-XXXXXX")));
    if (!owned.isValid()) throw std::runtime_error("Cannot create owned grounding fixture");
    const auto root = QDir(owned.path()).filePath(QStringLiteral("workspace"));
    const auto database = QDir(owned.path()).filePath(QStringLiteral("index.sqlite"));
    if (!QDir().mkpath(root)) throw std::runtime_error("Cannot create fixture root");
    writeFixtureFiles(root, fixture.value(QStringLiteral("files")).toObject());
    bool ready = false;
    {
        roclint::storage::CodebaseIndex index(nativePath(database), nativePath(root));
        roclint::storage::CodebaseIndexLimits limits;
        const auto maxFiles = fixture.value(QStringLiteral("maximum_files")).toInt(5000);
        if (maxFiles < 1 || maxFiles > 5000) throw std::runtime_error("Invalid evaluation file cap");
        limits.maximumFiles = static_cast<std::size_t>(maxFiles);
        const auto refreshed = index.refresh({nativePath(root), "", true}, limits);
        if (!refreshed.ok()) throw std::runtime_error(refreshed.error().message);
        ready = !refreshed.value().incomplete;
    }
    writeFixtureFiles(root, fixture.value(QStringLiteral("mutate_before_grounding")).toObject());
    QJsonObject inputs{{QStringLiteral("root"), root}, {QStringLiteral("index"), database},
        {QStringLiteral("activeFile"), fixture.value(QStringLiteral("activeFile"))},
        {QStringLiteral("ready"), ready}};
    auto bound = fixture;
    bound.insert(QStringLiteral("_grounding_inputs"), inputs);
    const auto context = roclint::host::computeGroundedPromptContext(readGroundingInputs(bound));
    owned.setAutoRemove(false); // Retain synthetic evidence beside the run, including for post-generation checks.
    return {{QStringLiteral("_grounding_inputs"), inputs}, {QStringLiteral("grounding"), contextView(context)}};
}

QJsonArray contracts(const QJsonObject& fixtures) {
    QJsonArray output;
    for (const auto& item : fixtures.value(QStringLiteral("analysis")).toArray()) {
        auto fixture = item.toObject();
        roclint::bridge::GraniteOptionDiscoveryRequest request{kSession, kRevision, ss(fixture.value(QStringLiteral("prompt")).toString()), "UNKNOWN"};
        fixture.insert(QStringLiteral("op"), QStringLiteral("granite_analyze"));
        fixture.insert(QStringLiteral("instructions"), qs(roclint::bridge::buildDiscoveryPrompt(request)));
        fixture.insert(QStringLiteral("schema"), qs(roclint::bridge::discoveryResponseSchema(request.promptString)));
        fixture.insert(QStringLiteral("intent_id"), QString()); fixture.insert(QStringLiteral("rewrite_profile_id"), QString());
        output.append(fixture);
    }
    const auto literal = fixtures.value(QStringLiteral("rewrite")).toObject();
    for (const auto& profile : literal.value(QStringLiteral("profiles")).toArray()) {
        const auto profileId = profile.toString();
        const auto definition = roclint::core::rewrite::ProfileRegistry::getProfile(ss(profileId));
        if (!definition) throw std::runtime_error("Unknown frozen rewrite profile");
        const auto request = rewriteRequest(literal.value(QStringLiteral("prompt")).toString(), profileId);
        output.append(QJsonObject{{QStringLiteral("id"), literal.value(QStringLiteral("id")).toString() + QLatin1Char('_') + profileId},
            {QStringLiteral("op"), QStringLiteral("granite_rewrite")}, {QStringLiteral("prompt"), literal.value(QStringLiteral("prompt"))},
            {QStringLiteral("preparation"), literal.value(QStringLiteral("preparation"))},
            {QStringLiteral("instructions"), qs(roclint::bridge::buildRewritePrompt(request, *definition))},
            {QStringLiteral("schema"), qs(roclint::bridge::rewriteResponseSchema())},
            {QStringLiteral("intent_id"), qs(request.confirmedIntent.id)}, {QStringLiteral("rewrite_profile_id"), profileId}});
    }
    return output;
}

// Replay a saved discovery through the native repair loop. The driver supplies
// fresh worker replies on stdin so the auditor controls pruning and regeneration.
QJsonObject replayDiscovery(const roclint::bridge::GraniteOptionDiscoveryRequest& request,
                            const std::string& raw) {
    namespace bridge = roclint::bridge;
    namespace core = roclint::core;
    auto parsed = bridge::parseDiscoveryResponse(raw, request.sessionId, request.revisionId, request.promptString);
    if (!parsed.ok()) throw std::runtime_error(parsed.error().message);
    const auto& original = parsed.value();
    auto exchange = [&](const char* stage, std::string_view instructions, const std::string& schema)
        -> core::Result<std::string> {
        QJsonObject call{{"stage", stage}, {"instructions", qs(std::string(instructions))}, {"schema", qs(schema)}};
        std::cout << QJsonDocument(call).toJson(QJsonDocument::Compact).toStdString() << std::endl;
        std::string reply;
        if (!std::getline(std::cin, reply)) throw std::runtime_error("Missing worker reply");
        const auto frame = QJsonDocument::fromJson(QByteArray::fromStdString(reply)).object();
        if (!frame.value("ok").toBool()) return core::makeError(core::ErrorCode::ProviderUnavailable, "Worker failed");
        return ss(frame.value("result").toObject().value("text").toString());
    };
    bridge::GraniteAdapter adapter;
    if (!adapter.beginLoading() || !adapter.completeLoading({})) throw std::runtime_error("Adapter unavailable");
    bridge::GoldfishDiscoveryAuditRequest audit;
    audit.originalPrompt = request.promptString;
    audit.sessionId = request.sessionId;
    audit.proposedAnalysis = original.analysis;
    const auto result = bridge::GraniteGoldfishAdapter().correctDiscoveryWithRepairLoop(audit,
        [&](std::string_view instructions) {
            return exchange("audit", instructions, bridge::goldfishDiscoveryAuditResponseSchema());
        }, 1, 0, [&](const bridge::GoldfishDiscoveryAuditResult& findings) -> core::Result<core::analysis::NormalizedAnalysis> {
            auto retry = request;
            for (const auto* list : {&findings.violations, &findings.unresolved}) {
                for (const auto& item : *list) {
                    retry.repairFeedback += "- " + item.itemId + ": " + item.explanation + "\n";
                    if (retry.repairFeedback.size() > 1536) break;
                }
            }
            auto next = adapter.discoverOptions(retry, [&](std::string_view instructions) {
                return exchange("regenerate", instructions, bridge::discoveryResponseSchema(request.promptString, 1));
            });
            if (!next.ok()) return next.error();
            if (next.value().assistanceKind != original.assistanceKind)
                return core::makeError(core::ErrorCode::ValidationFailed, "Optionality changed");
            return std::move(next).value().analysis;
        });
    const auto& analysis = result.analysis;
    QJsonArray groups, missing, conflicts, assumptions, restored;
    for (const auto& group : analysis.choiceGroups) {
        QJsonArray choices;
        for (const auto& alt : group.alternatives) choices.append(qs(alt.interpretation));
        groups.append(QJsonObject{{"question", qs(group.question)}, {"choices", choices}});
    }
    for (const auto& item : analysis.missingDecisions) missing.append(qs(item.question));
    for (const auto& item : analysis.conflicts) conflicts.append(qs(item.description));
    for (const auto& item : analysis.assumptions) assumptions.append(qs(item.statement));
    if (groups.empty() && missing.empty() && conflicts.empty() && assumptions.empty())
        for (const auto& group : original.analysis.choiceGroups) restored.append(qs(group.question));
    auto presentation = [&](core::analysis::NormalizedAnalysis candidate) -> QJsonObject {
        core::analysis::sealSnapshot(candidate);
        if (!request.promptTriage) throw std::runtime_error("Replay requires bound triage");
        auto snapshot = core::analysis::buildRoutedAnalysisSnapshot(candidate, *request.promptTriage,
            request.promptString, "att_synthetic_replay", std::nullopt,
            core::analysis::ReviewPolicy{request.reviewTrigger, original.assistanceKind});
        if (!snapshot.ok()) return {{"error", qs(snapshot.error().message)}};
        const auto policy = core::analysis::MaterialityPolicyV1::evaluatePrompt(candidate, request.promptString);
        const auto decision = core::intent::InterventionStateMachine::evaluate(candidate, policy,
            snapshot.value().evaluation, {});
        const bool manual = decision.verdict == core::intent::InterventionVerdict::Manual;
        const bool optional = policy.isEligible() && core::analysis::isOptionalAssistance(original.assistanceKind);
        const char* outcome = manual && !decision.activeItemId ? "UNAVAILABLE" : optional ? "OPTIONAL_HELP"
            : manual || decision.requiresUserIntervention() ? "QUESTION" : "READY";
        return {{"review_outcome", outcome}, {"policy", core::analysis::toString(policy.overallVerdict)},
                {"verdict", core::intent::toString(decision.verdict)}, {"reason", core::intent::toString(decision.reason)}};
    };
    auto hosted = analysis;
    for (const auto& question : restored) {
        core::analysis::MissingDecision item;
        item.id = core::analysis::mintMissingDecisionId(hosted.missingDecisions.size());
        item.question = ss(question.toString());
        hosted.missingDecisions.push_back(std::move(item));
    }
    return {{"audit", qs(core::toString(result.auditVerdict.outcome))},
            {"status", qs(core::toString(result.auditVerdict.executionStatus))}, {"groups", groups},
            {"missing", missing}, {"conflicts", conflicts}, {"assumptions", assumptions},
            {"before", presentation(original.analysis)}, {"after", presentation(hosted)},
            {"host_fallback_questions", restored}};
}

QJsonArray validateRuns(const QJsonObject& report) {
    QJsonArray output;
    for (const auto& run : report.value(QStringLiteral("runs")).toArray()) {
        for (const auto& item : run.toObject().value(QStringLiteral("cases")).toArray()) {
            const auto fixture = item.toObject();
            const auto response = fixture.value(QStringLiteral("response")).toObject();
            QJsonObject result{{QStringLiteral("quantization"), run.toObject().value(QStringLiteral("quantization"))},
                {QStringLiteral("id"), fixture.value(QStringLiteral("id"))}, {QStringLiteral("transport_ok"), response.value(QStringLiteral("ok"))}};
            if (!response.value(QStringLiteral("ok")).toBool()) { output.append(result); continue; }
            const auto text = ss(response.value(QStringLiteral("result")).toObject().value(QStringLiteral("text")).toString());
            const auto prompt = fixture.value(QStringLiteral("prompt")).toString();
            if (fixture.value(QStringLiteral("op")) == QLatin1String("granite_analyze")) {
                const auto parsed = roclint::bridge::parseDiscoveryResponse(text, kSession, kRevision, ss(prompt));
                result.insert(QStringLiteral("parse_valid"), parsed.ok());
                if (parsed.ok()) {
                    const auto policy = roclint::core::analysis::MaterialityPolicyV1::evaluatePrompt(parsed.value().analysis, ss(prompt));
                    result.insert(QStringLiteral("outcome"), qs(std::string(roclint::bridge::toString(parsed.value().outcome))));
                    result.insert(QStringLiteral("ungrounded_evidence_count"), static_cast<qint64>(parsed.value().ungroundedEvidenceCount));
                    result.insert(QStringLiteral("policy_verdict"), QString::fromLatin1(roclint::core::analysis::toString(policy.overallVerdict)));
                    result.insert(QStringLiteral("policy_reason"), QString::fromLatin1(roclint::core::analysis::toString(policy.primaryReasonCode)));
                } else result.insert(QStringLiteral("error"), qs(parsed.error().message));
            } else {
                const auto profileId = fixture.value(QStringLiteral("rewrite_profile_id")).toString();
                const auto profile = roclint::core::rewrite::ProfileRegistry::getProfile(ss(profileId));
                if (!profile) throw std::runtime_error("Benchmark response has an unknown profile");
                auto request = rewriteRequest(prompt, profileId, fixture.value(QStringLiteral("request_fixture")).toObject());
                const auto ground = fixture.value(QStringLiteral("grounding")).toObject();
                request.groundedCanonicalIntentXml = ss(ground.value(QStringLiteral("xml")).toString());
                request.astSnapshotHash = ss(ground.value(QStringLiteral("ast_hash")).toString());
                if (fixture.contains(QStringLiteral("_grounding_inputs"))) {
                    const auto current = roclint::host::computeGroundedPromptContext(readGroundingInputs(fixture));
                    const bool beforeValidated = ground.value(QStringLiteral("available")).toBool() &&
                        ground.value(QStringLiteral("validated")).toBool();
                    const bool currentEvidence = !beforeValidated || (current.available && current.validated &&
                        current.astSnapshotHash == request.astSnapshotHash);
                    result.insert(QStringLiteral("post_grounding"), contextView(current));
                    result.insert(QStringLiteral("evidence_current"), currentEvidence);
                    if (!currentEvidence) {
                        result.insert(QStringLiteral("withheld"), true);
                        result.insert(QStringLiteral("proposal_published"), false);
                        output.append(result);
                        continue;
                    }
                }
                const roclint::bridge::GraniteModelInfo model{ss(response.value(QStringLiteral("model_id")).toString()),
                    ss(response.value(QStringLiteral("weights_sha256")).toString()), ss(response.value(QStringLiteral("quantization")).toString()), "local_native_llama_cpp"};
                const auto parsed = roclint::bridge::GraniteRewriteAdapter::parseRewriteResponse(text, request, *profile, model, 1);
                result.insert(QStringLiteral("parse_valid"), parsed.ok());
                if (parsed.ok()) {
                    result.insert(QStringLiteral("model_outcome"), parsed.value().status == roclint::core::rewrite::ProposalStatus::ProposalReady
                        ? QStringLiteral("rewritten") : QStringLiteral("needs_clarification"));
                    const auto validation = roclint::core::rewrite::RewriteValidator::validateProposal(parsed.value(), kSession, kRevision, request.confirmedIntent, {});
                    result.insert(QStringLiteral("structurally_valid"), validation.structurallyValid);
                    result.insert(QStringLiteral("pending_review_count"), static_cast<qint64>(validation.pendingAcknowledgments.size()));
                    result.insert(QStringLiteral("rejection_reason"), qs(validation.rejectionReason));
                    result.insert(QStringLiteral("proposal_ast_hash"), qs(parsed.value().astSnapshotHash));
                } else result.insert(QStringLiteral("error"), qs(parsed.error().message));
            }
            output.append(result);
        }
    }
    return output;
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const bool validate = argc == 3 && std::string_view(argv[1]) == "--validate";
    const bool rewriteFixture = argc == 3 && std::string_view(argv[1]) == "--rewrite-fixture";
    const bool discoveryFixture = argc == 3 && std::string_view(argv[1]) == "--discovery-fixture";
    const bool wordFixture = argc == 3 && std::string_view(argv[1]) == "--word-fixture";
    const bool auditFixture = argc == 3 && std::string_view(argv[1]) == "--audit-fixture";
    if (argc != 2 && !validate && !rewriteFixture && !discoveryFixture && !auditFixture && !wordFixture) { std::cerr << "Usage: roclint_granite_contracts <fixtures.json> | --validate <benchmark-report.json> | --rewrite-fixture <fixture.json> | --discovery-fixture <fixture.json> | --audit-fixture <fixture.json>\n"; return 2; }
    try {
        QFile file(QString::fromLocal8Bit(argv[validate || rewriteFixture || discoveryFixture || auditFixture || wordFixture ? 2 : 1]));
        if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Could not read the frozen fixture corpus");
        const auto fixtures = QJsonDocument::fromJson(file.readAll()).object();
        if (wordFixture) {
            if (!fixtures.value("synthetic").toBool()) throw std::runtime_error("Only synthetic word fixtures are supported");
            roclint::bridge::WordRefinementRequest request;
            request.sessionId = kSession; request.revisionId = kRevision;
            const auto prompt = fixtures.value("prompt").toString(), selected = fixtures.value("selected").toString();
            const int start = fixtures.contains("start") ? fixtures.value("start").toInt(-1) : prompt.indexOf(selected);
            if (prompt.isEmpty() || selected.isEmpty() || start < 0 || prompt.mid(start, selected.size()) != selected)
                throw std::runtime_error("Invalid synthetic word selection");
            request.draftText = ss(prompt); request.selectedText = ss(selected); request.selectionStart = start; request.selectionEnd = start + static_cast<int>(selected.size());
            for (const auto& value : fixtures.value("approved_decisions").toArray()) request.approvedDecisions.push_back(ss(value.toString()));
            if (!request.approvedDecisions.empty()) request.intervention = roclint::bridge::WordIntervention::Rewrite;
            std::string instructions = roclint::bridge::buildWordRefinementPrompt(request);
            std::string schema = roclint::bridge::wordRefinementResponseSchema(request.draftText);
            if (fixtures.contains("model_response") && !fixtures.value("check_answer").toBool()) {
                auto parsed = roclint::bridge::parseWordRefinementResponse(ss(fixtures.value("model_response").toString()), request);
                if (!parsed.ok()) throw std::runtime_error(parsed.error().message);
                if (fixtures.value("export_audit").toBool()) {
                    instructions = roclint::bridge::buildWordRefinementAuditPrompt(request, parsed.value());
                    schema = roclint::bridge::wordRefinementAuditResponseSchema();
                } else {
                    QJsonArray choices, replacements;
                    for (const auto& value : parsed.value().clarificationChoices) choices.append(qs(value));
                    for (const auto& value : parsed.value().candidates) replacements.append(qs(value.replacement));
                    std::cout << QJsonDocument(QJsonObject{{"needs_clarification", parsed.value().needsClarification},
                        {"question", qs(parsed.value().clarificationQuestion)}, {"choices", choices}, {"replacements", replacements}}).toJson(QJsonDocument::Compact).toStdString() << '\n';
                    return 0;
                }
            }
            if (fixtures.value("check_answer").toBool()) {
                roclint::bridge::ClarificationCheckRequest check;
                check.prompt = request.draftText; check.question = ss(fixtures.value("question").toString()); check.answer = ss(fixtures.value("answer").toString());
                if (fixtures.contains("model_response")) {
                    const auto verdict = roclint::bridge::parseClarificationCheckResponse(ss(fixtures.value("model_response").toString()));
                    if (!verdict.ok()) throw std::runtime_error(verdict.error().message);
                    std::cout << QJsonDocument(QJsonObject{{"accepted", verdict.value().accepted}, {"feedback", qs(verdict.value().feedback)}}).toJson(QJsonDocument::Compact).toStdString() << '\n';
                    return 0;
                }
                instructions = roclint::bridge::buildClarificationCheckPrompt(check);
                schema = roclint::bridge::clarificationCheckResponseSchema();
            }
            std::cout << QJsonDocument(QJsonObject{{"id", fixtures.value("id")}, {"op", "granite_word_refinement"},
                {"prompt", prompt}, {"instructions", qs(instructions)}, {"schema", qs(schema)}}).toJson(QJsonDocument::Compact).toStdString() << '\n';
            return 0;
        }
        if (auditFixture) {
            if (!fixtures.value(QStringLiteral("synthetic")).toBool())
                throw std::runtime_error("Only explicitly synthetic fixtures are supported");
            const auto prompt = fixtures.value(QStringLiteral("prompt")).toString();
            roclint::bridge::GoldfishAuditRequest request;
            request.originalPrompt = ss(prompt);
            request.candidateRewrite = ss(fixtures.value(QStringLiteral("candidate")).toString());
            request.sessionId = kSession;
            request.constraints = roclint::host::HostController::extractSessionConstraints(request.originalPrompt, {});
            if (fixtures.contains(QStringLiteral("model_response"))) {
                const auto reply = ss(fixtures.value(QStringLiteral("model_response")).toString());
                const auto verdict = roclint::bridge::GraniteGoldfishAdapter().audit(request,
                    [reply](std::string_view) -> roclint::core::Result<std::string> { return reply; });
                QJsonArray violations, unresolved;
                for (const auto& finding : verdict.violations)
                    violations.append(QJsonObject{{"id", qs(finding.constraintId)}, {"explanation", qs(finding.explanation)}});
                for (const auto& finding : verdict.unresolved)
                    unresolved.append(QJsonObject{{"id", qs(finding.constraintId)}, {"explanation", qs(finding.explanation)}});
                const QJsonObject result{{"outcome", qs(roclint::core::toString(verdict.outcome))},
                    {"violations", violations}, {"unresolved", unresolved}};
                std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).toStdString() << '\n';
                return 0;
            }
            const QJsonObject result{{QStringLiteral("id"), fixtures.value(QStringLiteral("id"))},
                {QStringLiteral("op"), QStringLiteral("granite_rewrite")}, {QStringLiteral("prompt"), prompt},
                {QStringLiteral("intent_id"), QStringLiteral("intent_synthetic_task_audit")},
                {QStringLiteral("rewrite_profile_id"), QStringLiteral("GENERIC@1")},
                {QStringLiteral("instructions"), qs(roclint::bridge::buildGoldfishAuditPrompt(request))},
                {QStringLiteral("schema"), qs(roclint::bridge::goldfishAuditResponseSchema())}};
            std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).toStdString() << '\n';
            return 0;
        }
        if (discoveryFixture) {
            if (!fixtures.value(QStringLiteral("synthetic")).toBool())
                throw std::runtime_error("Only explicitly synthetic fixtures are supported");
            const auto prompt = fixtures.value(QStringLiteral("prompt")).toString();
            if (prompt.trimmed().isEmpty()) throw std::runtime_error("Empty discovery fixture");
            roclint::bridge::GraniteOptionDiscoveryRequest request{kSession, kRevision, ss(prompt), "UNKNOWN"};
            request.hostContextCategory = ss(fixtures.value("host").toString("UNKNOWN"));
            for (const auto& file : fixtures.value("workspace_files").toArray())
                request.workspaceFiles.push_back(ss(file.toString()));
            for (const auto& symbol : fixtures.value("workspace_symbols").toArray())
                request.workspaceSymbols.push_back(ss(symbol.toString()));
            QJsonObject routes;
            if (fixtures.contains("category_scores")) {
                namespace analysis = roclint::core::analysis;
                analysis::PromptTriage triage;
                triage.sessionId = kSession;
                triage.revisionId = kRevision;
                triage.revisionTextHash = roclint::core::crypto::Sha256::hexDigest(request.promptString);
                triage.requestGroupId = "fixture_group";
                triage.attemptId = "fixture_attempt";
                triage.stateVersion = 1;
                triage.modelId = "SupersonicLabs/Julia-1";
                triage.modelRevision = ss(fixtures.value("model_revision").toString());
                triage.weightsSha256 = ss(fixtures.value("weights_sha256").toString());
                triage.configurationId = ss(fixtures.value("configuration_id").toString());
                triage.execution = analysis::TriageExecution::Model;
                const auto scores = fixtures.value("category_scores").toArray();
                if (scores.size() != 4) throw std::runtime_error("Expected four category scores");
                for (int i = 0; i < scores.size(); ++i) triage.categoryScores[i] = scores[i].toDouble(-1);
                auto sealed = analysis::sealPromptTriage(triage, request.promptString);
                if (!sealed.ok()) throw std::runtime_error(sealed.error().message);
                request.promptTriage = sealed.value();
                routes["default"] = analysis::toString(analysis::routePromptTriage(sealed.value()).value());
                for (double threshold : {0.65, 0.70, 0.85}) {
                    auto policy = analysis::TriagePolicy::defaults();
                    policy.thresholds[0] = policy.thresholds[1] = threshold;
                    routes[QString::number(threshold, 'f', 2)] = analysis::toString(
                        analysis::routePromptTriage(sealed.value(), policy).value());
                }
            }
            request.reviewTrigger = fixtures.value(QStringLiteral("explicit_review")).toBool(true)
                ? roclint::core::analysis::ReviewTrigger::ExplicitReview
                : roclint::core::analysis::ReviewTrigger::Automatic;
            request.maximumQuestions = 1;
            request.substantialInterview = fixtures.value(QStringLiteral("substantial_interview")).toBool();
            request.isNonTechnicalPrompt = fixtures.value(QStringLiteral("non_technical")).toBool(true);
            if (fixtures.contains("saved_discovery")) {
                const auto replay = replayDiscovery(request, ss(fixtures.value("saved_discovery").toString()));
                std::cout << QJsonDocument(replay).toJson(QJsonDocument::Compact).toStdString() << std::endl;
                return 0;
            }
            QJsonObject result{{QStringLiteral("id"), fixtures.value(QStringLiteral("id"))},
                {QStringLiteral("op"), QStringLiteral("granite_analyze")}, {QStringLiteral("prompt"), prompt},
                {QStringLiteral("instructions"), qs(roclint::bridge::buildDiscoveryPrompt(request))},
                {QStringLiteral("schema"), qs(roclint::bridge::discoveryResponseSchema(request.promptString, 1))}};
            if (!routes.isEmpty()) result["routes"] = routes;
            std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).toStdString() << '\n';
            return 0;
        }
        if (rewriteFixture) {
            if (!fixtures.value(QStringLiteral("synthetic")).toBool())
                throw std::runtime_error("Only explicitly synthetic fixtures are supported");
            const auto prompt = fixtures.value(QStringLiteral("prompt")).toString();
            const auto profileId = fixtures.value(QStringLiteral("profile")).toString(QStringLiteral("GENERIC@1"));
            const auto profile = roclint::core::rewrite::ProfileRegistry::getProfile(ss(profileId));
            if (prompt.trimmed().isEmpty() || !profile) throw std::runtime_error("Invalid rewrite fixture");
            auto request = rewriteRequest(prompt, profileId, fixtures);
            const auto grounding = prepareGrounding(fixtures, QFileInfo(file).absolutePath());
            const auto context = grounding.value(QStringLiteral("grounding")).toObject();
            request.groundedCanonicalIntentXml = ss(context.value(QStringLiteral("xml")).toString());
            request.astSnapshotHash = ss(context.value(QStringLiteral("ast_hash")).toString());
            QJsonObject result{{QStringLiteral("id"), fixtures.value(QStringLiteral("id"))},
                {QStringLiteral("op"), QStringLiteral("granite_rewrite")},
                {QStringLiteral("prompt"), prompt}, {QStringLiteral("rewrite_profile_id"), profileId},
                {QStringLiteral("intent_id"), qs(request.confirmedIntent.id)},
                {QStringLiteral("instructions"), qs(roclint::bridge::buildRewritePrompt(request, *profile))},
                {QStringLiteral("schema"), qs(roclint::bridge::rewriteResponseSchema())},
                {QStringLiteral("request_fixture"), fixtures}};
            for (auto field = grounding.begin(); field != grounding.end(); ++field) result.insert(field.key(), field.value());
            std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).toStdString() << '\n';
            return 0;
        }
        if (validate) {
            if (fixtures.value(QStringLiteral("runs")).toArray().isEmpty()) throw std::runtime_error("Benchmark report contains no runs");
            const auto results = validateRuns(fixtures);
            if (results.isEmpty()) throw std::runtime_error("Benchmark report contains no cases");
            std::cout << QJsonDocument(results).toJson(QJsonDocument::Indented).toStdString();
            return 0;
        }
        if (fixtures.value(QStringLiteral("fixture_version")) != QJsonValue(1) || fixtures.value(QStringLiteral("analysis")).toArray().size() != 6 ||
            fixtures.value(QStringLiteral("rewrite")).toObject().value(QStringLiteral("profiles")).toArray().size() != 4)
            throw std::runtime_error("Frozen fixture corpus is malformed or incomplete");
        const QJsonObject result{{QStringLiteral("fixture_version"), fixtures.value(QStringLiteral("fixture_version"))},
            {QStringLiteral("session_id"), QString::fromLatin1(kSession)}, {QStringLiteral("draft_revision_id"), QString::fromLatin1(kRevision)},
            {QStringLiteral("cases"), contracts(fixtures)}, {QStringLiteral("review_scope"), fixtures.value(QStringLiteral("review_scope"))}};
        std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).toStdString() << '\n';
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
