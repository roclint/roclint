#include "HostController.hpp"
#include "roclint/storage/PayloadCodec.hpp"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QUuid>

namespace roclint::host {
namespace an = core::analysis;
namespace {
QString qs(const std::string& value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }
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
    QJsonObject metadata;
    metadata[QStringLiteral("prompt")] = promptText;

    QStringList candidateChoices;
    if (!m_projectFiles.isEmpty()) {
        QStringList workspaceMatches;
        for (const auto& file : m_projectFiles) {
            const QFileInfo fi(file);
            const QString baseName = fi.fileName();
            const QString baseStem = fi.completeBaseName();
            if (promptText.contains(baseName, Qt::CaseInsensitive) ||
                (!baseStem.isEmpty() && baseStem.length() > 3 && promptText.contains(baseStem, Qt::CaseInsensitive))) {
                workspaceMatches.append(file);
                if (workspaceMatches.size() >= 10) break;
            }
        }
        if (!workspaceMatches.isEmpty()) {
            metadata[QStringLiteral("workspace_matches")] = QJsonArray::fromStringList(workspaceMatches);
            if (workspaceMatches.size() >= 2 && workspaceMatches.size() <= 20) {
                candidateChoices = workspaceMatches;
            }
        }
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
    if (route.value() == an::PromptTriageRoute::Original) {
        ticket.analysisGroup->terminal = true;
        an::NormalizedAnalysis empty;
        empty.sessionId = ticket.sessionId; empty.baseRevisionId = ticket.revisionId;
        an::sealSnapshot(empty);
        if (applyAnalysis(std::move(empty), {}, {}, std::nullopt, *triage))
            setAnalysisStatus(AnalysisStatus::Ready, QStringLiteral("Prompt is clear (S1 score %1) — original wording retained (Granite skipped to save battery/latency).")
                .arg(triage->ambiguityScore, 0, 'f', 3));
    } else if (route.value() == an::PromptTriageRoute::UncertainBuffer) {
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
