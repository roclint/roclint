// Evaluation-only harness for the unmodified production HostController.
//
// This probe is NOT a product feature and NOT a test of a mock. It drives the
// same app/host/HostController used by the desktop app through its public
// invokables and test seams compiled into roclint_host_lib_testing. It never
// approves, inserts, sends, or touches a user editor/history database.
//
// Output is one JSON object per line (NDJSON) on stdout so an external runner
// can timestamp and archive it without reinterpreting model output.

#include "HostController.hpp"

#include "roclint/analysis/AnalysisSnapshot.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QThread>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <cstdio>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

QElapsedTimer g_clock;

qint64 nowMs() { return g_clock.elapsed(); }

QJsonObject emitLine(const QJsonObject& object) {
    std::cout << QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString() << '\n';
    std::cout.flush();
    return object;
}

QJsonValue toJson(const QVariant& value) {
    QJsonValue converted = QJsonValue::fromVariant(value);
    return converted;
}

QJsonArray toJsonArray(const QVariantList& values) {
    QJsonArray array;
    for (const auto& value : values) array.append(QJsonValue::fromVariant(value));
    return array;
}

// Pumps the event loop and reports whether predicate became true.
template <typename Predicate>
bool waitUntil(Predicate predicate, int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (predicate()) return true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return predicate();
}

QString sanitizedError(const roclint::host::HostController& host) {
    const QString code = host.lastErrorCode();
    const QString message = host.lastError();
    if (code.isEmpty()) return message;
    return code + QStringLiteral(": ") + message;
}

class Probe {
public:
    Probe(roclint::host::HostController& host, const QJsonObject& scenario)
        : host_(host), scenario_(scenario) {}
    ~Probe() {
        for (const auto& connection : connections_) QObject::disconnect(connection);
    }

    void attach() {
        connections_.push_back(QObject::connect(
            &host_, &roclint::host::HostController::analysisStatusChanged, &host_, [this]() {
                emitLine(QJsonObject{{QStringLiteral("event"), QStringLiteral("status")},
                                     {QStringLiteral("t_ms"), nowMs()},
                                     {QStringLiteral("scenario"), scenarioId()},
                                     {QStringLiteral("status"), host_.analysisStatus()},
                                     {QStringLiteral("message"), host_.analysisMessage()}});
            }));
        connections_.push_back(QObject::connect(
            &host_, &roclint::host::HostController::rewriteProposalChanged, &host_, [this]() {
                // Only status transitions are logged; full text is sampled at checkpoints.
                emitLine(QJsonObject{{QStringLiteral("event"), QStringLiteral("proposal_status")},
                                     {QStringLiteral("t_ms"), nowMs()},
                                     {QStringLiteral("scenario"), scenarioId()},
                                     {QStringLiteral("proposal_status"), host_.proposalStatus()},
                                     {QStringLiteral("message"), host_.proposalMessage()},
                                     {QStringLiteral("has_proposal"), host_.hasProposal()}});
            }));
        connections_.push_back(QObject::connect(
            &host_, &roclint::host::HostController::clarificationCheckChanged, &host_, [this]() {
                emitLine(QJsonObject{{QStringLiteral("event"), QStringLiteral("clarification_check")},
                                     {QStringLiteral("t_ms"), nowMs()},
                                     {QStringLiteral("scenario"), scenarioId()},
                                     {QStringLiteral("checking"), host_.isCheckingClarification()},
                                     {QStringLiteral("feedback"), host_.clarificationFeedback()}});
            }));
    }

    QString scenarioId() const { return scenario_.value(QStringLiteral("id")).toString(); }

    QJsonObject observation(const QString& phase, int turn, qint64 elapsedMs = -1) {
        QJsonObject out;
        out.insert(QStringLiteral("event"), QStringLiteral("observation"));
        out.insert(QStringLiteral("phase"), phase);
        out.insert(QStringLiteral("turn"), turn);
        out.insert(QStringLiteral("t_ms"), nowMs());
        if (elapsedMs >= 0) out.insert(QStringLiteral("elapsed_ms"), elapsedMs);
        out.insert(QStringLiteral("scenario"), scenarioId());
        out.insert(QStringLiteral("analysis_status"), host_.analysisStatus());
        out.insert(QStringLiteral("analysis_message"), host_.analysisMessage());
        out.insert(QStringLiteral("is_analyzing"), host_.isAnalyzing());
        out.insert(QStringLiteral("review_outcome"), host_.reviewOutcome());
        out.insert(QStringLiteral("review_diagnostics"), host_.reviewDiagnostics());
        out.insert(QStringLiteral("is_original_route"), host_.isOriginalRoute());
        out.insert(QStringLiteral("verdict"), host_.interventionVerdict());
        out.insert(QStringLiteral("intervention_reason"), host_.interventionReason());
        out.insert(QStringLiteral("is_clarifying"), host_.isClarifying());
        out.insert(QStringLiteral("is_manual"), host_.isManual());
        out.insert(QStringLiteral("active_item_id"), host_.activeItemId());
        out.insert(QStringLiteral("active_item_kind"), host_.activeItemKind());
        out.insert(QStringLiteral("question"), host_.activeQuestion());
        out.insert(QStringLiteral("alternatives"), toJsonArray(host_.activeAlternatives()));
        out.insert(QStringLiteral("assumption_reviews"), toJsonArray(host_.assumptionReviews()));
        out.insert(QStringLiteral("recommended_option"), host_.recommendedOption());
        out.insert(QStringLiteral("recommended_confidence"), host_.recommendedConfidence());
        out.insert(QStringLiteral("is_ambiguity_scored"), host_.isAmbiguityScored());
        out.insert(QStringLiteral("ambiguity_score"), host_.ambiguityScore());
        out.insert(QStringLiteral("ranking_status"), host_.rankingStatus());
        out.insert(QStringLiteral("diagnostic_message"), host_.diagnosticMessage());
        out.insert(QStringLiteral("rounds_completed"), host_.roundsCompleted());
        out.insert(QStringLiteral("remaining_budget"), host_.remainingBudget());
        out.insert(QStringLiteral("has_confirmed_intent"), host_.hasConfirmedIntent());
        out.insert(QStringLiteral("is_intent_stale"), host_.isIntentStale());
        out.insert(QStringLiteral("confirmed_items"), toJsonArray(host_.confirmedItems()));
        out.insert(QStringLiteral("unresolved_items"), QJsonArray::fromStringList(host_.unresolvedItems()));
        out.insert(QStringLiteral("audit_outcome"), host_.auditOutcome());
        out.insert(QStringLiteral("audit_execution_status"), host_.auditExecutionStatus());
        out.insert(QStringLiteral("audit_violations"), toJsonArray(host_.auditViolations()));
        out.insert(QStringLiteral("audit_unresolved"), toJsonArray(host_.auditUnresolved()));
        out.insert(QStringLiteral("proposal_status"), host_.proposalStatus());
        out.insert(QStringLiteral("proposal_message"), host_.proposalMessage());
        out.insert(QStringLiteral("has_proposal"), host_.hasProposal());
        out.insert(QStringLiteral("proposal_text"), host_.proposalText());
        out.insert(QStringLiteral("streaming_preview"), host_.streamingPreview());
        out.insert(QStringLiteral("last_diff"), host_.lastDiffText());
        out.insert(QStringLiteral("proposal_constraints"), toJsonArray(host_.proposalConstraints()));
        out.insert(QStringLiteral("pending_review_count"), host_.pendingReviewCount());
        out.insert(QStringLiteral("can_approve"), host_.canApprove());
        out.insert(QStringLiteral("creative_alternatives"), toJsonArray(host_.creativeAlternatives()));
        out.insert(QStringLiteral("selected_alternative_index"), host_.selectedAlternativeIndex());
        out.insert(QStringLiteral("has_approval"), host_.hasApproval());
        out.insert(QStringLiteral("approval_label"), host_.approvalLabel());
        out.insert(QStringLiteral("model_runtime_state"), host_.modelRuntimeState());
        out.insert(QStringLiteral("model_engine_status"), host_.modelEngineStatus());
        out.insert(QStringLiteral("grounding_status"), host_.groundingStatus());
        out.insert(QStringLiteral("grounded_evidence"), toJsonArray(host_.groundedEvidence()));
        out.insert(QStringLiteral("codebase_index_ready"), host_.codebaseIndexReadyForTesting());
        out.insert(QStringLiteral("thinking_enabled"), host_.thinkingEnabled());
        out.insert(QStringLiteral("last_error_code"), host_.lastErrorCode());
        out.insert(QStringLiteral("last_error"), host_.lastError());

        if (const auto& snapshot = host_.finalSnapshotForTesting(); snapshot) {
            if (snapshot->promptTriage) {
                const auto& triage = *snapshot->promptTriage;
                QJsonObject triageOut;
                triageOut.insert(QStringLiteral("configuration_id"),
                                 QString::fromStdString(triage.configurationId));
                triageOut.insert(QStringLiteral("model_id"), QString::fromStdString(triage.modelId));
                triageOut.insert(QStringLiteral("model_revision"),
                                 QString::fromStdString(triage.modelRevision));
                triageOut.insert(QStringLiteral("weights_sha256"),
                                 QString::fromStdString(triage.weightsSha256));
                triageOut.insert(QStringLiteral("template_id"), QString::fromStdString(triage.templateId));
                triageOut.insert(QStringLiteral("execution"), static_cast<int>(triage.execution));
                triageOut.insert(QStringLiteral("primary_category"),
                                 QString::fromUtf8(roclint::core::analysis::toString(triage.primaryCategory)));
                const auto route = roclint::core::analysis::routePromptTriage(triage);
                triageOut.insert(QStringLiteral("route"),
                                 route.ok()
                                     ? QString::fromStdString(
                                           roclint::core::analysis::toString(route.value()))
                                     : QStringLiteral("invalid: ") +
                                           QString::fromStdString(route.error().message));
                triageOut.insert(QStringLiteral("ambiguity"), triage.ambiguityScore);
                QJsonArray scores;
                for (double score : triage.categoryScores) scores.append(score);
                triageOut.insert(QStringLiteral("category_scores"), scores);
                triageOut.insert(QStringLiteral("latency_ms"), triage.latencyMs);
                out.insert(QStringLiteral("prompt_triage"), triageOut);
            }
            out.insert(QStringLiteral("snapshot_payload_version"), snapshot->payloadVersion);
            out.insert(QStringLiteral("snapshot_id"), QString::fromStdString(snapshot->id));
            out.insert(QStringLiteral("discovery_attempt_id"),
                       QString::fromStdString(snapshot->discoveryAttemptId));
        }
        return out;
    }

    QJsonObject attempts() {
        QJsonArray array;
        if (!host_.repositoryForTesting()) return QJsonObject{{QStringLiteral("attempts"), array}};
        const auto session = roclint::core::SessionId(host_.currentSessionIdString().toStdString());
        auto task = host_.repositoryForTesting()->getModelAttempts(session);
        const auto records = task.waitForTesting();
        if (!records.ok()) {
            return QJsonObject{{QStringLiteral("event"), QStringLiteral("attempts")},
                               {QStringLiteral("attempts"), array},
                               {QStringLiteral("scenario"), scenarioId()},
                               {QStringLiteral("error"), QString::fromStdString(records.error().message)}};
        }
        for (const auto& attempt : records.value()) {
            QJsonObject item;
            item.insert(QStringLiteral("attempt_id"), QString::fromStdString(attempt.attemptId));
            item.insert(QStringLiteral("stage"), QString::fromStdString(attempt.stage));
            item.insert(QStringLiteral("status"), QString::fromStdString(roclint::storage::toString(attempt.status)));
            item.insert(QStringLiteral("transport"), QString::fromStdString(attempt.transport));
            item.insert(QStringLiteral("model_id"), QString::fromStdString(attempt.modelId));
            item.insert(QStringLiteral("model_detail"), QString::fromStdString(attempt.modelDetail));
            item.insert(QStringLiteral("error"), QString::fromStdString(attempt.errorMessage));
            item.insert(QStringLiteral("started_at_ms"), attempt.startedAtMs);
            item.insert(QStringLiteral("completed_at_ms"),
                        attempt.completedAtMs ? QJsonValue(*attempt.completedAtMs) : QJsonValue());
            item.insert(QStringLiteral("duration_ms"),
                        attempt.completedAtMs ? QJsonValue(*attempt.completedAtMs - attempt.startedAtMs)
                                              : QJsonValue());
            array.append(item);
        }
        return QJsonObject{{QStringLiteral("event"), QStringLiteral("attempts")},
                           {QStringLiteral("attempts"), array},
                           {QStringLiteral("scenario"), scenarioId()}};
    }

    int run() {
        const QString prompt = scenario_.value(QStringLiteral("prompt")).toString();
        const bool explicitReview = scenario_.value(QStringLiteral("explicit_review")).toBool(false);
        const bool wantRewrite = scenario_.value(QStringLiteral("rewrite")).toBool(true);
        const int maxTurns = scenario_.value(QStringLiteral("max_turns")).toInt(3);
        const int analysisTimeout = scenario_.value(QStringLiteral("analysis_timeout_ms")).toInt(400000);
        const int rewriteTimeout = scenario_.value(QStringLiteral("rewrite_timeout_ms")).toInt(240000);
        const QJsonArray answerPlan = scenario_.value(QStringLiteral("answers")).toArray();

        emitLine(QJsonObject{{QStringLiteral("event"), QStringLiteral("scenario_start")},
                             {QStringLiteral("t_ms"), nowMs()},
                             {QStringLiteral("scenario"), scenarioId()},
                             {QStringLiteral("prompt"), prompt},
                             {QStringLiteral("explicit_review"), explicitReview},
                             {QStringLiteral("rewrite_requested"), wantRewrite}});

        const QString projectDirectory = scenario_.value(QStringLiteral("project_directory")).toString();
        if (!projectDirectory.isEmpty()) {
            host_.getProjectFiles(projectDirectory);
            const int indexTimeout = scenario_.value(QStringLiteral("index_timeout_ms")).toInt(180000);
            const bool idle = host_.waitForIdleForTesting(indexTimeout);
            const bool ready = waitUntil([&]() { return host_.codebaseIndexReadyForTesting(); }, indexTimeout);
            emitLine(QJsonObject{{QStringLiteral("event"), QStringLiteral("project_indexed")},
                                 {QStringLiteral("t_ms"), nowMs()},
                                 {QStringLiteral("scenario"), scenarioId()},
                                 {QStringLiteral("project"), projectDirectory},
                                 {QStringLiteral("idle"), idle},
                                 {QStringLiteral("index_ready"), ready}});
        }

        const qint64 started = nowMs();
        quint64 version = host_.stateVersion();
        const quint64 requestId = explicitReview ? host_.reviewPrompt(prompt, version)
                                                 : host_.analyzePrompt(prompt, version);
        const bool posted = host_.waitForCommandForTesting(requestId, 30000);
        emitLine(QJsonObject{{QStringLiteral("event"), QStringLiteral("analysis_posted")},
                             {QStringLiteral("t_ms"), nowMs()},
                             {QStringLiteral("scenario"), scenarioId()},
                             {QStringLiteral("command_id"), QJsonValue::fromVariant(requestId)},
                             {QStringLiteral("posted_ok"), posted}});
        const bool analysisIdle = waitUntil([&]() {
            const QString status = host_.analysisStatus();
            return !host_.isAnalyzing() &&
                   (status == QLatin1String("READY") || status == QLatin1String("FAILED") ||
                    status == QLatin1String("ANALYZING"));
        }, analysisTimeout) && waitUntil([&]() { return !host_.isAnalyzing(); }, analysisTimeout);
        const qint64 analysisElapsed = nowMs() - started;
        {
            QJsonObject out = observation(QStringLiteral("analysis_complete"), -1, analysisElapsed);
            out.insert(QStringLiteral("analysis_idle_ok"), analysisIdle);
            emitLine(out);
        }
        emitLine(attempts());

        int turn = 0;
        while (host_.isClarifying() && turn < maxTurns) {
            QJsonObject plan = (turn < answerPlan.size()) ? answerPlan.at(turn).toObject() : QJsonObject();
            const QString type = plan.value(QStringLiteral("type")).toString();
            const qint64 turnStart = nowMs();
            quint64 commandId = 0;
            QJsonObject requested;
            requested.insert(QStringLiteral("type"), type);
            if (type == QLatin1String("choice")) {
                const QString pick = plan.value(QStringLiteral("pick")).toString();
                QString chosenId;
                QString chosenLabel;
                int fallbackIndex = plan.value(QStringLiteral("index")).toInt(0);
                const auto alternatives = host_.activeAlternatives();
                for (int i = 0; i < alternatives.size(); ++i) {
                    const auto map = alternatives.at(i).toMap();
                    const QString id = map.value(QStringLiteral("id")).toString();
                    QString label = map.value(QStringLiteral("label")).toString();
                    const QString interpretation = map.value(QStringLiteral("interpretation")).toString();
                    const QString haystack = (label + QLatin1Char(' ') + interpretation).toLower();
                    if (!pick.isEmpty() && haystack.contains(pick.toLower())) {
                        chosenId = id;
                        chosenLabel = label;
                        break;
                    }
                    if (i == fallbackIndex && chosenId.isEmpty()) {
                        chosenId = id;
                        chosenLabel = label;
                    }
                }
                requested.insert(QStringLiteral("pick"), pick);
                requested.insert(QStringLiteral("resolved_id"), chosenId);
                requested.insert(QStringLiteral("resolved_label"), chosenLabel);
                commandId = host_.confirmChoice(chosenId, host_.stateVersion());
            } else if (type == QLatin1String("custom")) {
                const QString text = plan.value(QStringLiteral("text")).toString();
                requested.insert(QStringLiteral("text"), text);
                commandId = host_.submitCustomIntent(text, host_.stateVersion());
            } else if (type == QLatin1String("unknown")) {
                const QString text = plan.value(QStringLiteral("text")).toString(QStringLiteral("I don't know"));
                requested.insert(QStringLiteral("text"), text);
                commandId = host_.submitClarification(text, host_.stateVersion());
            } else if (type == QLatin1String("skip")) {
                commandId = host_.skipCurrentItem(host_.stateVersion());
            } else {
                requested.insert(QStringLiteral("type"), QStringLiteral("skip_default"));
                commandId = host_.skipCurrentItem(host_.stateVersion());
            }
            const bool completed = host_.waitForCommandForTesting(commandId, 120000);
            const bool idle = host_.waitForIdleForTesting(240000);
            QJsonObject answer = observation(QStringLiteral("answer"), turn, nowMs() - turnStart);
            answer.insert(QStringLiteral("requested"), requested);
            answer.insert(QStringLiteral("command_id"), QJsonValue::fromVariant(commandId));
            answer.insert(QStringLiteral("command_completed"), completed);
            answer.insert(QStringLiteral("idle_ok"), idle);
            emitLine(answer);
            emitLine(attempts());
            ++turn;
        }

        // Any remaining clarification is an unanswered/skipped item; rewrite without
        // questions makes the deferral explicit rather than silently inventing an answer.
        bool usedRewriteWithoutQuestions = false;
        if (host_.isClarifying() && wantRewrite) {
            usedRewriteWithoutQuestions = true;
            const quint64 id = host_.rewriteWithoutQuestions(host_.stateVersion());
            host_.waitForCommandForTesting(id, 30000);
            const bool idle = host_.waitForIdleForTesting(rewriteTimeout);
            QJsonObject out = observation(QStringLiteral("rewrite_without_questions"), turn);
            out.insert(QStringLiteral("command_id"), QJsonValue::fromVariant(id));
            out.insert(QStringLiteral("idle_ok"), idle);
            emitLine(out);
            emitLine(attempts());
        }

        if (wantRewrite && !host_.hasProposal() && !host_.isClarifying() && !host_.isOriginalRoute()) {
            const qint64 rewriteStart = nowMs();
            const quint64 id = host_.synthesizeRewrite(host_.stateVersion());
            const bool posted = host_.waitForCommandForTesting(id, 30000);
            // A native rewrite may warm the model first; wait for actual idle.
            const bool idle = host_.waitForIdleForTesting(rewriteTimeout);
            if (!host_.hasProposal() && host_.proposalStatus() == QLatin1String("Generating")) {
                waitUntil([&]() { return host_.hasProposal() || host_.proposalStatus() != QLatin1String("Generating"); },
                          rewriteTimeout);
            }
            QJsonObject out = observation(QStringLiteral("rewrite_complete"), turn, nowMs() - rewriteStart);
            out.insert(QStringLiteral("command_id"), QJsonValue::fromVariant(id));
            out.insert(QStringLiteral("posted_ok"), posted);
            out.insert(QStringLiteral("idle_ok"), idle);
            emitLine(out);
            emitLine(attempts());
        }

        QJsonObject summary = observation(QStringLiteral("scenario_end"), turn, nowMs() - started);
        summary.insert(QStringLiteral("used_rewrite_without_questions"), usedRewriteWithoutQuestions);
        emitLine(summary);
        return 0;
    }

private:
    roclint::host::HostController& host_;
    QJsonObject scenario_;
    std::vector<QMetaObject::Connection> connections_;
};

QJsonObject readConfig(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        std::cerr << "Cannot open config: " << path.toStdString() << '\n';
        std::exit(2);
    }
    const auto document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) {
        std::cerr << "Config must be a JSON object\n";
        std::exit(2);
    }
    return document.object();
}

} // namespace

int main(int argc, char** argv) {
    g_clock.start();
    QCoreApplication app(argc, argv);
    if (argc != 2) {
        std::cerr << "Usage: companion_probe <config.json>\n";
        return 2;
    }
    const auto config = readConfig(QString::fromLocal8Bit(argv[1]));
    const auto settingsObject = config.value(QStringLiteral("settings")).toObject();
    const QString settingsDirectory = config.value(QStringLiteral("settings_directory")).toString();
    if (settingsDirectory.isEmpty()) {
        std::cerr << "settings_directory is required\n";
        return 2;
    }
    QDir().mkpath(settingsDirectory);
    QCoreApplication::setOrganizationName(QStringLiteral("RoclintCompanionProbe"));
    QCoreApplication::setApplicationName(QStringLiteral("isolated"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory);
    {
        QSettings settings;
        settings.setValue(QStringLiteral("scoring/engine"), QStringLiteral("julia"));
        settings.setValue(QStringLiteral("scoring/juliaPython"),
                          settingsObject.value(QStringLiteral("python")).toString());
        settings.setValue(QStringLiteral("scoring/juliaDirectory"),
                          settingsObject.value(QStringLiteral("julia_directory")).toString());
        settings.setValue(QStringLiteral("models/nativeDirectory"),
                          settingsObject.value(QStringLiteral("granite_directory")).toString());
        settings.setValue(QStringLiteral("models/active"),
                          settingsObject.value(QStringLiteral("active_model")).toString(
                              QStringLiteral("native-granite:Q8_0")));
        settings.sync();
    }

    const int indexTimeout = config.value(QStringLiteral("index_timeout_ms")).toInt(120000);
    const QString worker = settingsObject.value(QStringLiteral("worker")).toString();
    roclint::host::HostController host(QStringLiteral(":memory:"));
    if (!host.waitForStorageForTesting()) {
        emitLine(QJsonObject{{QStringLiteral("event"), QStringLiteral("probe_error")},
                             {QStringLiteral("message"), QStringLiteral("storage did not initialize")}});
        return 3;
    }
    if (!host.setNativeWorkerForTesting(worker)) {
        emitLine(QJsonObject{{QStringLiteral("event"), QStringLiteral("probe_error")},
                             {QStringLiteral("message"), QStringLiteral("native worker was rejected")}});
        return 3;
    }

    emitLine(QJsonObject{{QStringLiteral("event"), QStringLiteral("probe_start")},
                         {QStringLiteral("t_ms"), nowMs()},
                         {QStringLiteral("worker"), worker},
                         {QStringLiteral("index_timeout_ms"), indexTimeout},
                         {QStringLiteral("scenario_count"), config.value(QStringLiteral("scenarios")).toArray().size()}});

    bool failed = false;
    for (const auto& value : config.value(QStringLiteral("scenarios")).toArray()) {
        Probe probe(host, value.toObject());
        probe.attach();
        if (probe.run() != 0) failed = true;
        host.waitForIdleForTesting(30000);
    }

    emitLine(QJsonObject{{QStringLiteral("event"), QStringLiteral("probe_end")},
                         {QStringLiteral("t_ms"), nowMs()},
                         {QStringLiteral("failed"), failed}});
    return failed ? 1 : 0;
}
