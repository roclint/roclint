#include "roclint/analysis/MaterialityPolicy.hpp"
#include "roclint/bridge/GraniteAdapter.hpp"
#include "roclint/bridge/GraniteRewriteAdapter.hpp"
#include "roclint/bridge/S1TriageAdapter.hpp"
#include "roclint/intent/ConfirmedIntent.hpp"
#include "roclint/rewrite/RewriteValidator.hpp"
#include "HostController.hpp"

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
    if (argc != 2 && !validate && !rewriteFixture) { std::cerr << "Usage: roclint_granite_contracts <fixtures.json> | --validate <benchmark-report.json> | --rewrite-fixture <fixture.json>\n"; return 2; }
    try {
        QFile file(QString::fromLocal8Bit(argv[validate || rewriteFixture ? 2 : 1]));
        if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Could not read the frozen fixture corpus");
        const auto fixtures = QJsonDocument::fromJson(file.readAll()).object();
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
