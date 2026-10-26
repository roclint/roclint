#include "HostController.hpp"
#include "roclint/storage/PayloadCodec.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUuid>

namespace roclint::host {
namespace an = core::analysis;
namespace {
QString qs(const std::string& value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }

QJsonObject extractDynamicAstContext(const QString& promptText, const QString& projectRoot, const QStringList& projectFiles) {
    QJsonObject meta;
    meta[QStringLiteral("prompt")] = promptText;
    const QString pLower = promptText.toLower();

    // 1. Matched files
    QStringList matchedFiles;
    for (const auto& file : projectFiles) {
        const QFileInfo fi(file);
        const QString baseName = fi.fileName().toLower();
        if (pLower.contains(baseName)) {
            matchedFiles.append(file);
        }
    }
    if (!matchedFiles.isEmpty()) {
        meta[QStringLiteral("matched_files")] = QJsonArray::fromStringList(matchedFiles);
        meta[QStringLiteral("workspace_matches")] = QJsonArray::fromStringList(matchedFiles);
    }

    // 2. Unbounded Target Files (Multi-file collisions)
    QStringList targetCollisionFiles;
    const bool mentionsNav = pLower.contains(QStringLiteral("navigation")) || pLower.contains(QStringLiteral("nav link")) ||
                             pLower.contains(QStringLiteral("site navigation")) || pLower.contains(QStringLiteral("header nav"));
    if (mentionsNav && !projectRoot.isEmpty()) {
        for (const auto& relFile : projectFiles) {
            if (relFile.endsWith(QStringLiteral(".html"), Qt::CaseInsensitive) ||
                relFile.endsWith(QStringLiteral(".qml"), Qt::CaseInsensitive)) {
                QFile f(QDir(projectRoot).filePath(relFile));
                if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                    const QString content = QString::fromUtf8(f.read(8192));
                    if (content.contains(QStringLiteral("<nav"), Qt::CaseInsensitive) ||
                        content.contains(QStringLiteral("class=\"nav"), Qt::CaseInsensitive)) {
                        targetCollisionFiles.append(relFile);
                    }
                }
            }
        }
    }
    if (targetCollisionFiles.size() > 1) {
        meta[QStringLiteral("unbounded_target_files")] = QJsonArray::fromStringList(targetCollisionFiles);
    }

    // 3. Subjective / Unbounded Action
    static const QRegularExpression subjectiveRe(QStringLiteral("\\b(much nicer|nicer|more modern|modern|look better|cleaner look|beautify)\\b"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression numericCssRe(QStringLiteral("\\b\\d+(?:\\.\\d+)?(?:rem|em|px|pt|%|vh|vw|s|ms)\\b"), QRegularExpression::CaseInsensitiveOption);
    if (subjectiveRe.match(pLower).hasMatch() && !numericCssRe.match(pLower).hasMatch()) {
        meta[QStringLiteral("unbounded_action")] = true;
    }

    // 4. Constraint Violations / Rule Conflict
    static const QRegularExpression negativeRe(QStringLiteral("(?:without modifying|without changing|do not modify|don't change|must not modify)\\s+([^.,;]+)"), QRegularExpression::CaseInsensitiveOption);
    const auto negMatch = negativeRe.match(pLower);
    if (negMatch.hasMatch()) {
        const QString forbidden = negMatch.captured(1).trimmed();
        QJsonArray violations;
        violations.append(forbidden);
        meta[QStringLiteral("constraint_violations")] = violations;
    }

    // 5. Missing Backend Sink in Static Workspace
    static const QRegularExpression sinkRe(QStringLiteral("\\b(send(?:s)? and save(?:d)?|sent and saved|wire up .*form|submissions? get sent|save submissions?)\\b"), QRegularExpression::CaseInsensitiveOption);
    if (sinkRe.match(pLower).hasMatch()) {
        bool hasBackend = false;
        bool hasForm = false;
        for (const auto& file : projectFiles) {
            const QString ext = QFileInfo(file).suffix().toLower();
            if (ext == QStringLiteral("py") || ext == QStringLiteral("go") || ext == QStringLiteral("rs") ||
                ext == QStringLiteral("php") || ext == QStringLiteral("rb") || file.contains(QStringLiteral("server"))) {
                hasBackend = true;
            }
            if (ext == QStringLiteral("html")) {
                hasForm = true;
            }
        }
        if (hasForm && !hasBackend) {
            meta[QStringLiteral("missing_backend_sink")] = true;
        }
    }

    return meta;
}
}

bool HostController::promptTriageRoutingEnabled() const noexcept {
#ifdef ROCLINT_ENABLE_TEST_MOCKS
    return m_promptTriageRouting;
#else
    return true;
#endif
}

core::Status HostController::startPromptTriage() {
    if (!m_revisionState) return core::makeError(core::ErrorCode::PreconditionFailed, "No frozen prompt to classify");
    if (m_promptTriageLoad || !m_pendingPromptTriage.isEmpty()) return core::Status::success();
    if (m_s1Adapter.engine() != bridge::ScorerEngine::JuliaLocal) {
        setAnalysisStatus(AnalysisStatus::Failed, QStringLiteral("S1 requires local Julia. Choose Julia in Settings; the original prompt remains available."));
        return core::makeError(core::ErrorCode::ProviderUnavailable, "Local Julia is required for S1 prompt classification");
    }
    m_promptTriageLoad = makeTicket();
    if (m_julia->isReady()) { continuePromptTriageLoad(); return core::Status::success(); }
    setAnalysisStatus(AnalysisStatus::Analyzing, QStringLiteral("Loading Julia before classifying the prompt. Granite has not been requested."));
    if (!m_julia->isRunning()) {
        // Loading an existing local installation is reversible. Downloads/installations remain explicit.
        if (!QFileInfo(m_juliaModelDirectory).isDir() || !QFileInfo(m_juliaPython).isExecutable()) {
            m_promptTriageLoad.reset();
            setAnalysisStatus(AnalysisStatus::Failed, QStringLiteral("Julia is missing. Load its local model in Settings before analyzing; the original prompt remains available."));
            return core::makeError(core::ErrorCode::ProviderUnavailable, "Julia's local model or Python runtime is missing");
        }
        m_julia->load(m_juliaModelDirectory, m_juliaPython);
    } else if (m_julia->state() == QLatin1String("FAILED") || m_julia->state() == QLatin1String("STOPPING")) {
        m_promptTriageLoad.reset();
        setAnalysisStatus(AnalysisStatus::Failed, QStringLiteral("Julia is stopping after an earlier operation. Reload Julia in Settings, then retry Analyze."));
        return core::makeError(core::ErrorCode::ProviderUnavailable, "Julia is not ready for prompt classification");
    }
    return core::Status::success();
}

void HostController::continuePromptTriageLoad() {
    if (!m_promptTriageLoad) return;
    if (!isTicketCurrent(*m_promptTriageLoad, false)) { m_promptTriageLoad.reset(); return; }
    if (!m_julia->isReady()) {
        if (m_julia->state() == QLatin1String("FAILED") || m_julia->state() == QLatin1String("NOT_LOADED")) {
            m_promptTriageLoad.reset();
            const auto reason = m_julia->detail() + QStringLiteral(" Retry Analyze or reload Julia in Settings; no clear route was assumed.");
            setAnalysisStatus(AnalysisStatus::Failed, reason);
            setLastError(reason);
        } else {
            setAnalysisStatus(AnalysisStatus::Analyzing,
                QStringLiteral("S1: %1 First load can take up to 3 minutes. Granite runs only after classification.").arg(m_julia->detail()));
        }
        return;
    }
    auto ticket = std::move(*m_promptTriageLoad);
    m_promptTriageLoad.reset();
    if (!m_julia->supportsPromptTriage()) {
        setAnalysisStatus(AnalysisStatus::Failed, QStringLiteral("Update the Julia helper: it does not support S1 prompt classification."));
        return;
    }
    ticket.analysisGroup = std::make_shared<AnalysisGroup>(m_analysisGroupTimeoutMs);
    ticket.analysisGroup->id = "grp_" + QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    if (const auto status = beginAttempt(ticket, "julia_triage"); !status.ok()) {
        setLastError(status.error()); setAnalysisStatus(AnalysisStatus::Failed, qs(status.error().message)); return;
    }
    const JuliaRuntime::RequestContext context{qs(ticket.analysisGroup->id), qs(ticket.attemptId),
        qs(ticket.sessionId), qs(ticket.revisionId), ticket.stateVersion};

    const QString promptText = qs(m_revisionState->currentRevision().text());
    const QJsonObject metadata = extractDynamicAstContext(promptText, m_projectScanPath, m_projectFiles);

    QStringList candidateChoices;
    if (metadata.contains(QStringLiteral("unbounded_target_files"))) {
        const auto arr = metadata.value(QStringLiteral("unbounded_target_files")).toArray();
        for (const auto& v : arr) candidateChoices.append(v.toString());
    } else if (metadata.contains(QStringLiteral("matched_files"))) {
        const auto arr = metadata.value(QStringLiteral("matched_files")).toArray();
        for (const auto& v : arr) candidateChoices.append(v.toString());
    }

    const auto id = m_julia->classifyPrompt(promptText, context, metadata, candidateChoices);
    if (!id) {
        completeAttempt(ticket, storage::AttemptStatus::Failed, "Julia did not accept the S1 request");
        setAnalysisStatus(AnalysisStatus::Failed, QStringLiteral("Julia could not classify this prompt. Retry explicitly.")); return;
    }
    m_pendingPromptTriage.insert(id, ticket);
    ++m_inFlight;
    setAnalysisStatus(AnalysisStatus::Analyzing, QStringLiteral("S1: Julia is checking action, target, rules and rule conflicts."));
    armAnalysisDeadline(ticket);
}

void HostController::onPromptClassified(quint64 id, const an::PromptTriage* triage, const QString& error) {
    auto found = m_pendingPromptTriage.find(id);
    if (found == m_pendingPromptTriage.end()) return;
    auto ticket = std::move(found.value());
    m_pendingPromptTriage.erase(found); --m_inFlight;
    if (!isTicketCurrent(ticket, false)) {
        ticket.analysisGroup->terminal = true;
        completeAttempt(ticket, storage::AttemptStatus::DiscardedStale, "Prompt changed before S1 classification completed");
        finishQuitWhenRuntimesStopped(); return;
    }
    if (!triage || ticket.analysisGroup->terminal || ticket.analysisGroup->deadline.hasExpired()) {
        ticket.analysisGroup->terminal = true;
        const auto reason = error.isEmpty() ? QStringLiteral("S1 classification exceeded the analysis budget.") : error;
        completeAttempt(ticket, storage::AttemptStatus::Failed, reason.toUtf8().toStdString());
        setAnalysisStatus(AnalysisStatus::Failed, reason + QStringLiteral(" The original prompt remains available."));
        finishQuitWhenRuntimesStopped(); return;
    }
    const auto route = an::routePromptTriage(*triage);
    if (!route.ok() || triage->sessionId != ticket.sessionId || triage->revisionId != ticket.revisionId ||
        triage->attemptId != ticket.attemptId || triage->stateVersion != ticket.stateVersion ||
        triage->requestGroupId != ticket.analysisGroup->id) {
        ticket.analysisGroup->terminal = true;
        completeAttempt(ticket, storage::AttemptStatus::Failed, "S1 returned invalid classification bindings");
        setAnalysisStatus(AnalysisStatus::Failed, QStringLiteral("S1 returned invalid classification evidence. No route was selected.")); return;
    }
    // Store the sealed evidence in the attempt even if subsequent Granite generation fails.
    ticket.configuration += "; prompt_triage_payload=" + storage::encodePromptTriage(*triage);
    completeAttempt(ticket, storage::AttemptStatus::Succeeded, "");
    m_promptTriage = *triage;
    m_promptTriageLastRoute = route.value();
    m_routedAnalysisGroup = ticket.analysisGroup;

    const auto recOpt = m_julia->lastRecommendedOption();
    const auto recConf = m_julia->lastChoiceConfidence();
    if (m_recommendedOption != recOpt || !qFuzzyCompare(m_recommendedConfidence, recConf)) {
        m_recommendedOption = recOpt;
        m_recommendedConfidence = recConf;
        emit recommendedOptionChanged();
    }

    auto effectiveRoute = route.value();
    if (effectiveRoute == an::PromptTriageRoute::Clarify) {
        const double actionScore = triage->categoryScores[0];
        const double targetScore = triage->categoryScores[1];
        const double ruleScore   = triage->categoryScores[2];
        const double conflictScore = triage->categoryScores[3];

        // If Action and Target are clear (<= 0.50) and Rule is bounded (<= 0.70),
        // but conflictScore is elevated: check if this is an affirmative prompt with strict negative guardrails
        // rather than an actual unresolvable conflict.
        if (targetScore < 0.50 && actionScore < 0.50 && ruleScore < 0.70 && conflictScore >= 0.60) {
            const QString text = qs(m_revisionState->currentRevision().text()).toLower();
            const bool hasStrictGuardrail = text.contains(QStringLiteral("do not ")) ||
                                            text.contains(QStringLiteral("don't ")) ||
                                            text.contains(QStringLiteral("must not ")) ||
                                            text.contains(QStringLiteral("keep the ")) ||
                                            text.contains(QStringLiteral("preserve "));
            const bool hasAffirmativeGoal = text.contains(QStringLiteral("implement ")) ||
                                            text.contains(QStringLiteral("refactor ")) ||
                                            text.contains(QStringLiteral("decouple ")) ||
                                            text.contains(QStringLiteral("add ")) ||
                                            text.contains(QStringLiteral("create ")) ||
                                            text.contains(QStringLiteral("update ")) ||
                                            text.contains(QStringLiteral("goal:")) ||
                                            text.contains(QStringLiteral("desired behavior:"));
            if (hasStrictGuardrail && hasAffirmativeGoal) {
                if (m_recommendedOption.isEmpty() && m_activeAlternatives.isEmpty()) {
                    effectiveRoute = an::PromptTriageRoute::Original;
                }
            }
        }
    }

    if (effectiveRoute == an::PromptTriageRoute::Original) {
        ticket.analysisGroup->terminal = true;
        an::NormalizedAnalysis empty;
        empty.sessionId = ticket.sessionId; empty.baseRevisionId = ticket.revisionId;
        an::sealSnapshot(empty);
        if (applyAnalysis(std::move(empty), {}, {}, std::nullopt, *triage))
            setAnalysisStatus(AnalysisStatus::Ready, QStringLiteral("Prompt is clear (S1 score %1) — original wording retained (Granite skipped to save battery/latency).")
                .arg(triage->ambiguityScore, 0, 'f', 3));
    } else if (effectiveRoute == an::PromptTriageRoute::UncertainBuffer) {
        ticket.analysisGroup->terminal = true;
        an::NormalizedAnalysis empty;
        empty.sessionId = ticket.sessionId; empty.baseRevisionId = ticket.revisionId;
        an::sealSnapshot(empty);
        if (applyAnalysis(std::move(empty), {}, {}, std::nullopt, *triage))
            setAnalysisStatus(AnalysisStatus::Ready,
                QStringLiteral("S1 score is borderline (%1 on %2) — original wording retained "
                               "but clarity is not confirmed. Use Analyze to review with Granite.")
                    .arg(triage->ambiguityScore, 0, 'f', 3)
                    .arg(QString::fromUtf8(an::toString(triage->primaryCategory))));
    } else {
        if (const auto status = startDiscovery(); !status.ok()) setLastError(status.error());
    }
}
} // namespace roclint::host
