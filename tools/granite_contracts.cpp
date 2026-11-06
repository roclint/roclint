#include "roclint/analysis/MaterialityPolicy.hpp"
#include "roclint/bridge/GraniteAdapter.hpp"
#include "roclint/bridge/GraniteRewriteAdapter.hpp"
#include "roclint/bridge/S1TriageAdapter.hpp"
#include "roclint/intent/ConfirmedIntent.hpp"
#include "roclint/rewrite/RewriteValidator.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
std::string ss(const QString& value) { return value.toUtf8().toStdString(); }
QString qs(const std::string& value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }
constexpr const char* kSession = "sess_benchmark_synthetic";
constexpr const char* kRevision = "rev_sess_benchmark_synthetic_1";

roclint::bridge::GraniteRewriteRequest rewriteRequest(const QString& prompt, const QString& profileId) {
    namespace bridge = roclint::bridge;
    namespace analysis = roclint::core::analysis;
    namespace intent = roclint::core::intent;
    const auto normalized = bridge::parseDiscoveryResponse("{\"status\":\"no_ambiguity\"}", kSession, kRevision, ss(prompt));
    if (!normalized.ok()) throw std::runtime_error("Synthetic literal-only fixture could not be parsed");
    const auto evaluation = bridge::S1TriageAdapter::degradedResponse(normalized.value().analysis, "Synthetic fixture preparation; no scoring execution");
    const auto final = analysis::buildAnalysisSnapshot(normalized.value().analysis, evaluation, ss(prompt), "att_synthetic_fixture_preparation");
    if (!final.ok()) throw std::runtime_error("Synthetic literal-only final artifact could not be prepared");
    const auto policy = analysis::MaterialityPolicyV1::evaluatePrompt(normalized.value().analysis, ss(prompt));
    const auto confirmed = intent::IntentProvenanceBuilder::buildConfirmedIntent(final.value(), policy, intent::WorkingIntentState{}, 1);
    if (!confirmed.ok()) throw std::runtime_error("Synthetic original intent could not be bound");
    bridge::GraniteRewriteRequest request;
    request.sessionId = kSession; request.revisionId = kRevision; request.originalText = ss(prompt);
    request.analysis = final.value(); request.confirmedIntent = confirmed.value(); request.rewriteProfileId = ss(profileId);
    request.proposalEditGeneration = 1;
    return request;
}

QJsonArray contracts(const QJsonObject& fixtures) {
    QJsonArray output;
    for (const auto& item : fixtures.value(QStringLiteral("analysis")).toArray()) {
        auto fixture = item.toObject();
        roclint::bridge::GraniteOptionDiscoveryRequest request{kSession, kRevision, ss(fixture.value(QStringLiteral("prompt")).toString()), "UNKNOWN"};
        fixture.insert(QStringLiteral("op"), QStringLiteral("granite_analyze"));
        fixture.insert(QStringLiteral("instructions"), qs(roclint::bridge::buildDiscoveryPrompt(request)));
        fixture.insert(QStringLiteral("schema"), qs(roclint::bridge::discoveryResponseSchema()));
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
                const auto request = rewriteRequest(prompt, profileId);
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
            const auto request = rewriteRequest(prompt, profileId);
            const QJsonObject result{{QStringLiteral("id"), fixtures.value(QStringLiteral("id"))},
                {QStringLiteral("op"), QStringLiteral("granite_rewrite")},
                {QStringLiteral("prompt"), prompt}, {QStringLiteral("rewrite_profile_id"), profileId},
                {QStringLiteral("intent_id"), qs(request.confirmedIntent.id)},
                {QStringLiteral("instructions"), qs(roclint::bridge::buildRewritePrompt(request, *profile))},
                {QStringLiteral("schema"), qs(roclint::bridge::rewriteResponseSchema())}};
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
