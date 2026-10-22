#include "HostController.hpp"
#include "ScreenPlacement.hpp"

#include <QGuiApplication>
#include <QClipboard>
#include <QDateTime>
#include <QStandardPaths>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <iostream>

namespace roclint::host {

HostController::HostController(QObject* parent)
    : HostController(QString(), parent) {
}

HostController::HostController(const QString& dbPathOverride, QObject* parent)
    : QObject(parent) {
    initStorage(dbPathOverride);
}

void HostController::initStorage(const QString& dbPathOverride) {
    try {
        QString finalPath;
        if (!dbPathOverride.isEmpty()) {
            finalPath = dbPathOverride;
        } else {
            const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
            if (!dataDir.isEmpty()) {
                QDir().mkpath(dataDir);
                finalPath = dataDir + QStringLiteral("/history.db");
            } else {
                finalPath = QStringLiteral(":memory:");
            }
        }
        m_databasePath = finalPath;
        m_database = std::make_unique<roclint::storage::Database>(finalPath.toStdString());
        m_repository = std::make_unique<roclint::storage::PromptRepository>(*m_database);
        m_repository->migrate();
        refreshStorageStats();
    } catch (const std::exception& e) {
        std::cerr << "[HostController] Storage initialization error: " << e.what() << "\n";
        m_databaseStats = QStringLiteral("STORAGE UNAVAILABLE: ") + QString::fromUtf8(e.what());
        emit storageChanged();
    }
}

HostState HostController::hostState() const noexcept {
    return m_hostState;
}

QString HostController::hostStateString() const {
    switch (m_hostState) {
        case HostState::Starting: return QStringLiteral("STARTING");
        case HostState::Resident: return QStringLiteral("RESIDENT");
        case HostState::Quitting: return QStringLiteral("QUITTING");
    }
    return QStringLiteral("UNKNOWN");
}

CardVisibility HostController::cardVisibility() const noexcept {
    return m_cardVisibility;
}

QString HostController::cardVisibilityString() const {
    switch (m_cardVisibility) {
        case CardVisibility::Hidden: return QStringLiteral("HIDDEN");
        case CardVisibility::Visible: return QStringLiteral("VISIBLE");
        case CardVisibility::Focused: return QStringLiteral("FOCUSED");
    }
    return QStringLiteral("UNKNOWN");
}

bool HostController::isCardVisible() const noexcept {
    return m_cardVisibility != CardVisibility::Hidden;
}

bool HostController::isDashboardVisible() const noexcept {
    return m_dashboardVisible;
}

void HostController::markResident() {
    if (m_hostState != HostState::Starting) {
        return;
    }
    m_hostState = HostState::Resident;
    emit hostStateChanged();
}

void HostController::showCard() {
    if (m_hostState == HostState::Quitting) {
        return;
    }

    m_hostContext = HostContextDetector::captureFrontmost();
    emit hostContextChanged();

    captureClipboardDraft();

    const auto placement = ScreenPlacement::determineTargetPosition(820, 520);

    setCardVisibility(CardVisibility::Focused);
    emit showCardRequested(placement.x, placement.y);
}

void HostController::hideCard() {
    if (!setCardVisibility(CardVisibility::Hidden)) {
        return;
    }
    emit hideCardRequested();
}

void HostController::toggleCard() {
    if (isCardVisible()) {
        hideCard();
    } else {
        showCard();
    }
}

void HostController::showDashboard() {
    if (m_hostState == HostState::Quitting) {
        return;
    }
    refreshStorageStats();
    emit sessionListChanged();
    setDashboardVisibility(true);
    emit showDashboardRequested();
}

void HostController::hideDashboard() {
    if (!setDashboardVisibility(false)) {
        return;
    }
    emit hideDashboardRequested();
}

void HostController::toggleDashboard() {
    if (isDashboardVisible()) {
        hideDashboard();
    } else {
        showDashboard();
    }
}

void HostController::requestQuit() {
    if (m_hostState == HostState::Quitting) {
        return;
    }
    hideCard();
    hideDashboard();
    m_hostState = HostState::Quitting;
    emit hostStateChanged();
    // The application owner decides how to exit; the controller stays testable.
    emit quitRequested();
}

void HostController::syncCardVisibility(CardVisibility visibility) {
    if (m_hostState == HostState::Quitting) {
        return;
    }
    setCardVisibility(visibility);
}

void HostController::syncDashboardVisibility(bool visible) {
    if (m_hostState == HostState::Quitting) {
        return;
    }
    setDashboardVisibility(visible);
}

quint64 HostController::stateVersion() const noexcept {
    return m_revisionState ? m_revisionState->stateVersion() : 0;
}

QString HostController::currentRevisionId() const {
    if (!m_revisionState) {
        return QStringLiteral("none");
    }
    return QString::fromStdString(m_revisionState->currentRevision().revisionId().value());
}

QString HostController::lastCopiedText() const {
    return m_lastCopiedText;
}

bool HostController::hasHostContext() const noexcept {
    return m_hostContext.isValid();
}

QString HostController::hostContextCategory() const {
    return m_hostContext.categoryString();
}

QString HostController::hostContextDisplayName() const {
    return m_hostContext.displayName;
}

QString HostController::capturedDraftText() const {
    return m_capturedDraftText;
}

QString HostController::databasePath() const {
    return m_databasePath;
}

QString HostController::databaseStats() const {
    return m_databaseStats;
}

int HostController::sessionCount() const {
    if (!m_repository) {
        return 0;
    }
    try {
        return static_cast<int>(m_repository->listSessions().size());
    } catch (...) {
        return 0;
    }
}

void HostController::clearHostContext() {
    m_hostContext = HostContextObservation{};
    emit hostContextChanged();
}

void HostController::captureClipboardDraft() {
    if (auto* cb = QGuiApplication::clipboard()) {
        const QString clip = cb->text();
        if (!clip.trimmed().isEmpty()) {
            m_capturedDraftText = clip;
            emit capturedDraftTextChanged();
            emit draftCaptured(clip);
        }
    }
}

void HostController::copyToClipboard(const QString& text) {
    if (auto* cb = QGuiApplication::clipboard()) {
        cb->setText(text);
        m_lastCopiedText = text;
        emit lastCopiedTextChanged();
        emit promptCopied(text);
    }
}

void HostController::setDirectInjectionEnabled(bool enabled) {
    if (m_directInjectionEnabled != enabled) {
        m_directInjectionEnabled = enabled;
        emit directInjectionChanged();
    }
}

bool HostController::hasAccessibilityPermission() const noexcept {
    return HostContextDetector::isAccessibilityPermissionGranted();
}

bool HostController::performDirectInjection(const QString& text) {
    if (m_directInjectionEnabled) {
        bool success = HostContextDetector::performDirectInjection(text);
        if (success) {
            m_lastCopiedText = text;
            emit lastCopiedTextChanged();
            emit promptCopied(text);
            return true;
        }
    }
    // Fallback to standard clipboard copy per Contract C09
    copyToClipboard(text);
    return false;
}

void HostController::commitPrompt(const QString& text) {
    if (text.trimmed().isEmpty()) {
        return;
    }
    const std::string textStd = text.toStdString();
    const auto nowMs = QDateTime::currentMSecsSinceEpoch();

    if (!m_revisionState) {
        const std::string sessId = "sess_" + std::to_string(nowMs);
        core::PromptRevision root(
            core::SessionId(sessId),
            core::RevisionId("rev_1"),
            std::nullopt,
            textStd,
            nowMs
        );
        m_revisionState = std::make_unique<core::RevisionState>(root);

        if (m_repository) {
            try {
                m_repository->createSession(core::SessionId(sessId), nowMs);
                m_repository->saveRevision(root);
            } catch (const std::exception& e) {
                std::cerr << "[HostController] Failed to persist session/root: " << e.what() << "\n";
            }
        }
    } else {
        const auto nextNum = m_revisionState->stateVersion() + 1;
        const std::string nextId = "rev_" + std::to_string(nextNum);
        m_revisionState->commitEdit(
            m_revisionState->stateVersion(),
            core::RevisionId(nextId),
            textStd,
            static_cast<std::uint64_t>(nowMs)
        );

        if (m_repository) {
            try {
                m_repository->saveRevision(m_revisionState->currentRevision());
            } catch (const std::exception& e) {
                std::cerr << "[HostController] Failed to persist revision: " << e.what() << "\n";
            }
        }
    }

    refreshStorageStats();
    if (m_confirmedIntent.has_value()) {
        m_confirmedIntent->isCurrent = false;
        emit intentChanged();
    }
    emit stateVersionChanged();
    emit sessionListChanged();
}

void HostController::hardBypass(const QString& text) {
    commitPrompt(text);
    copyToClipboard(text);
}

void HostController::refreshStorageStats() {
    if (!m_repository) {
        m_databaseStats = QStringLiteral("STORAGE NOT CONNECTED");
        emit storageChanged();
        return;
    }
    try {
        const auto sessions = m_repository->listSessions();
        int totalRevisions = 0;
        for (const auto& s : sessions) {
            totalRevisions += static_cast<int>(m_repository->getHistory(s.id).size());
        }
        m_databaseStats = QStringLiteral("WAL MODE ENABLED  |  %1 SESSIONS  |  %2 REVISIONS  |  SCHEMA v%3")
                              .arg(sessions.size())
                              .arg(totalRevisions)
                              .arg(m_repository->getSchemaVersion());
    } catch (const std::exception& e) {
        m_databaseStats = QStringLiteral("ERROR: ") + QString::fromUtf8(e.what());
    }
    emit storageChanged();
}

QVariantList HostController::getRecentSessions() {
    QVariantList result;
    if (!m_repository) {
        return result;
    }

    try {
        const auto sessions = m_repository->listSessions();
        for (const auto& s : sessions) {
            QVariantMap item;
            const QString sessIdStr = QString::fromStdString(s.id.value());
            item[QStringLiteral("sessionId")] = sessIdStr;
            item[QStringLiteral("createdAtMs")] = static_cast<qlonglong>(s.createdAtMs);
            item[QStringLiteral("formattedTime")] = QDateTime::fromMSecsSinceEpoch(s.createdAtMs).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));

            const auto history = m_repository->getHistory(s.id);
            item[QStringLiteral("revisionCount")] = static_cast<int>(history.size());

            const auto latest = m_repository->getLatestRevision(s.id);
            if (latest.has_value()) {
                item[QStringLiteral("latestPrompt")] = QString::fromStdString(latest->text());
                item[QStringLiteral("latestRevisionId")] = QString::fromStdString(latest->revisionId().value());
            } else {
                item[QStringLiteral("latestPrompt")] = QString();
                item[QStringLiteral("latestRevisionId")] = QString();
            }

            result.append(item);
        }
    } catch (const std::exception& e) {
        std::cerr << "[HostController] getRecentSessions error: " << e.what() << "\n";
    }
    return result;
}

QVariantList HostController::getSessionHistory(const QString& sessionId) {
    QVariantList result;
    if (!m_repository || sessionId.trimmed().isEmpty()) {
        return result;
    }

    try {
        const auto history = m_repository->getHistory(core::SessionId(sessionId.toStdString()));
        for (const auto& rev : history) {
            QVariantMap item;
            item[QStringLiteral("revisionId")] = QString::fromStdString(rev.revisionId().value());
            item[QStringLiteral("sessionId")] = QString::fromStdString(rev.sessionId().value());
            if (rev.parentRevisionId().has_value()) {
                item[QStringLiteral("parentRevisionId")] = QString::fromStdString(rev.parentRevisionId()->value());
            } else {
                item[QStringLiteral("parentRevisionId")] = QString();
            }
            item[QStringLiteral("text")] = QString::fromStdString(rev.text());
            item[QStringLiteral("createdAtMs")] = static_cast<qlonglong>(rev.createdAtMs());
            item[QStringLiteral("formattedTime")] = QDateTime::fromMSecsSinceEpoch(rev.createdAtMs()).toString(QStringLiteral("HH:mm:ss"));
            result.append(item);
        }
    } catch (const std::exception& e) {
        std::cerr << "[HostController] getSessionHistory error: " << e.what() << "\n";
    }
    return result;
}

void HostController::restoreRevision(const QString& revisionId) {
    if (!m_repository || revisionId.trimmed().isEmpty()) {
        return;
    }

    try {
        const auto revOpt = m_repository->getRevision(core::RevisionId(revisionId.toStdString()));
        if (!revOpt.has_value()) {
            return;
        }

        const auto& rev = *revOpt;
        m_revisionState = std::make_unique<core::RevisionState>(rev);

        const QString text = QString::fromStdString(rev.text());
        emit stateVersionChanged();
        emit revisionRestored(text);
    } catch (const std::exception& e) {
        std::cerr << "[HostController] restoreRevision error: " << e.what() << "\n";
    }
}

void HostController::restoreSession(const QString& sessionId) {
    if (!m_repository || sessionId.trimmed().isEmpty()) {
        return;
    }

    try {
        const auto latestOpt = m_repository->getLatestRevision(core::SessionId(sessionId.toStdString()));
        if (!latestOpt.has_value()) {
            return;
        }

        const auto& rev = *latestOpt;
        m_revisionState = std::make_unique<core::RevisionState>(rev);

        const QString text = QString::fromStdString(rev.text());
        emit stateVersionChanged();
        emit revisionRestored(text);
    } catch (const std::exception& e) {
        std::cerr << "[HostController] restoreSession error: " << e.what() << "\n";
    }
}

void HostController::deleteSession(const QString& sessionId) {
    if (!m_repository || sessionId.trimmed().isEmpty()) {
        return;
    }

    try {
        const std::string sId = sessionId.toStdString();
        m_repository->deleteSession(core::SessionId(sId));

        if (m_revisionState && m_revisionState->currentRevision().sessionId().value() == sId) {
            m_revisionState.reset();
            emit stateVersionChanged();
        }
        refreshStorageStats();
        emit sessionListChanged();
    } catch (const std::exception& e) {
        std::cerr << "[HostController] deleteSession error: " << e.what() << "\n";
    }
}

void HostController::purgeHistory() {
    if (!m_repository) {
        return;
    }

    try {
        const auto sessions = m_repository->listSessions();
        for (const auto& s : sessions) {
            m_repository->deleteSession(s.id);
        }
        m_revisionState.reset();
        refreshStorageStats();
        emit stateVersionChanged();
        emit sessionListChanged();
    } catch (const std::exception& e) {
        std::cerr << "[HostController] purgeHistory error: " << e.what() << "\n";
    }
}

QStringList HostController::getProjectFiles(const QString& projectPath) {
    QStringList result;
    if (projectPath.trimmed().isEmpty()) {
        return result;
    }

    QString cleanPath = projectPath.trimmed();
    if (cleanPath.startsWith(QLatin1String("~/"))) {
        cleanPath = QDir::homePath() + cleanPath.mid(1);
    }

    QDir dir(cleanPath);
    if (!dir.exists()) {
        return result;
    }

    QDirIterator it(cleanPath, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString fullPath = it.next();
        const QString relPath = dir.relativeFilePath(fullPath);

        // Filter out hidden files, build artifacts, git, node_modules, binaries, etc.
        if (relPath.startsWith(QLatin1String(".")) ||
            relPath.contains(QLatin1String("/.")) ||
            relPath.startsWith(QLatin1String("build/")) ||
            relPath.startsWith(QLatin1String("build_")) ||
            relPath.contains(QLatin1String("/build/")) ||
            relPath.startsWith(QLatin1String("node_modules/")) ||
            relPath.endsWith(QLatin1String(".o")) ||
            relPath.endsWith(QLatin1String(".a")) ||
            relPath.endsWith(QLatin1String(".dylib")) ||
            relPath.endsWith(QLatin1String(".so")) ||
            relPath.endsWith(QLatin1String(".db")) ||
            relPath.endsWith(QLatin1String(".db-wal")) ||
            relPath.endsWith(QLatin1String(".db-shm")) ||
            relPath.endsWith(QLatin1String(".DS_Store"))) {
            continue;
        }

        result.append(relPath);
        if (result.size() >= 300) {
            break;
        }
    }
    result.sort();
    return result;
}

QString HostController::openFolderDialog() {
    return QFileDialog::getExistingDirectory(nullptr, QStringLiteral("Select Project Folder"), QDir::homePath());
}

bool HostController::setCardVisibility(CardVisibility visibility) {
    if (m_cardVisibility == visibility) {
        return false;
    }
    m_cardVisibility = visibility;
    emit cardVisibilityChanged();
    return true;
}

bool HostController::setDashboardVisibility(bool visible) {
    if (m_dashboardVisible == visible) {
        return false;
    }
    m_dashboardVisible = visible;
    emit dashboardVisibilityChanged();
    return true;
}

QString HostController::interventionVerdict() const {
    return QString::fromUtf8(core::intent::toString(m_currentDecision.verdict));
}

QString HostController::interventionReason() const {
    return QString::fromUtf8(core::intent::toString(m_currentDecision.reason));
}

bool HostController::isClarifying() const noexcept {
    return m_currentDecision.verdict == core::intent::InterventionVerdict::Clarify;
}

bool HostController::isManual() const noexcept {
    return m_currentDecision.verdict == core::intent::InterventionVerdict::Manual;
}

QString HostController::activeItemId() const {
    return m_currentDecision.activeItemId.has_value()
               ? QString::fromStdString(*m_currentDecision.activeItemId)
               : QString();
}

QString HostController::activeQuestion() const {
    return m_activeQuestion;
}

QString HostController::activeDimension() const {
    return m_activeDimension;
}

QVariantList HostController::activeAlternatives() const {
    return m_activeAlternatives;
}

int HostController::roundsCompleted() const noexcept {
    return static_cast<int>(m_currentDecision.roundsCompleted);
}

int HostController::remainingBudget() const noexcept {
    return static_cast<int>(m_currentDecision.remainingBudget);
}

double HostController::ambiguityScore() const noexcept {
    return m_ambiguityScore;
}

QString HostController::diagnosticMessage() const {
    return QString::fromStdString(m_currentDecision.diagnosticMessage);
}

bool HostController::hasConfirmedIntent() const noexcept {
    return m_confirmedIntent.has_value();
}

bool HostController::isIntentStale() const noexcept {
    if (!m_confirmedIntent.has_value()) return false;
    if (!m_confirmedIntent->isCurrent) return true;
    const std::string revId = m_revisionState ? m_revisionState->currentRevision().revisionId().value() : "none";
    return !core::intent::IntentProvenanceBuilder::isIntentValid(*m_confirmedIntent, revId, m_currentAnalysis);
}

QString HostController::confirmedIntentSummary() const {
    if (!m_confirmedIntent.has_value()) {
        return QStringLiteral("NO CONFIRMED INTENT");
    }
    if (isIntentStale()) {
        return QStringLiteral("INTENT STALE (PROMPT EDITED)");
    }
    return QString("CONFIRMED [%1] ∙ %2 ITEM(S) ∙ %3")
        .arg(QString::fromStdString(m_confirmedIntent->id))
        .arg(m_confirmedIntent->items.size())
        .arg(m_rewriteProfile);
}

QVariantList HostController::confirmedItems() const {
    QVariantList list;
    if (!m_confirmedIntent.has_value()) return list;

    for (const auto& item : m_confirmedIntent->items) {
        QVariantMap map;
        map[QStringLiteral("itemId")] = QString::fromStdString(item.itemId);
        map[QStringLiteral("kind")] = QString::fromStdString(core::intent::toString(item.kind));
        map[QStringLiteral("chosenValue")] = QString::fromStdString(item.chosenValue);
        map[QStringLiteral("rationale")] = QString::fromStdString(item.rationale);
        list.append(map);
    }
    return list;
}

QString HostController::rewriteProfile() const {
    return m_rewriteProfile;
}

bool HostController::isRewriteEnabled() const noexcept {
    return m_confirmedIntent.has_value() && !isIntentStale() && m_confirmedIntent->isCurrent && !isClarifying();
}

QString HostController::lastCompiledPrompt() const {
    return m_currentProposal.has_value() ? QString::fromStdString(m_currentProposal->compiledPrompt) : QString();
}

QString HostController::lastDiffText() const {
    return m_currentProposal.has_value() ? QString::fromStdString(m_currentProposal->diffText) : QString();
}

bool HostController::hasProposal() const noexcept {
    return m_currentProposal.has_value() && m_currentProposal->status == core::rewrite::ProposalStatus::ProposalReady;
}

QVariantList HostController::proposalConstraints() const {
    QVariantList list;
    if (!m_currentProposal.has_value()) return list;

    for (const auto& c : m_currentProposal->constraints) {
        QVariantMap map;
        map[QStringLiteral("id")] = QString::fromStdString(c.id);
        map[QStringLiteral("description")] = QString::fromStdString(c.description);
        map[QStringLiteral("status")] = QString::fromStdString(core::rewrite::toString(c.status));
        map[QStringLiteral("evidence")] = QString::fromStdString(c.evidence);
        list.append(map);
    }
    return list;
}

QString HostController::synthesizeRewrite(const QString& promptOverride) {
    QString promptText = promptOverride;
    if (promptText.isEmpty() && m_revisionState) {
        promptText = QString::fromStdString(m_revisionState->currentRevision().text());
    }
    if (promptText.isEmpty()) {
        promptText = m_capturedDraftText;
    }

    if (!m_confirmedIntent.has_value()) {
        rebuildConfirmedIntent();
    }
    if (!m_confirmedIntent.has_value()) {
        std::cerr << "[HostController] Cannot synthesize rewrite: no confirmed intent\n";
        return QString();
    }

    const std::string revId = m_revisionState ? m_revisionState->currentRevision().revisionId().value() : "rev_1";
    const std::string sessId = m_revisionState ? m_revisionState->currentRevision().sessionId().value() : "sess_1";

    bridge::GraniteRewriteRequest req{
        .sessionId = sessId,
        .revisionId = revId,
        .promptString = promptText.toStdString(),
        .confirmedIntent = *m_confirmedIntent,
        .rewriteProfileId = m_rewriteProfile.toStdString(),
        .modelQuantization = "Q8_0"
    };

    auto resp = m_rewriteAdapter.synthesizeRewrite(req);
    if (!resp.success || !resp.proposal.has_value()) {
        std::cerr << "[HostController] Rewrite synthesis failed: " << resp.errorMessage << "\n";
        return QString();
    }

    auto proposal = *resp.proposal;

    auto validation = core::rewrite::RewriteValidator::validateProposal(proposal, revId, *m_confirmedIntent);
    if (!validation.isValid) {
        std::cerr << "[HostController] Proposal failed validation: " << validation.rejectionReason << "\n";
        m_currentProposal = proposal;
        m_currentProposal->status = validation.finalStatus;
        emit rewriteProposalChanged();
        return QString();
    }

    m_currentProposal = proposal;
    const QString compiled = QString::fromStdString(proposal.compiledPrompt);
    const QString diff = QString::fromStdString(proposal.diffText);

    commitPrompt(compiled);
    copyToClipboard(compiled);

    emit rewriteProposalChanged();
    emit rewriteSynthesized(compiled, diff);

    return compiled;
}

void HostController::analyzePrompt(const QString& text) {
    m_confirmedIntent.reset();
    emit intentChanged();

    if (text.trimmed().isEmpty()) {
        m_currentDecision = core::intent::InterventionDecision{};
        m_currentDecision.verdict = core::intent::InterventionVerdict::Quiet;
        m_currentDecision.reason = core::intent::InterventionReasonCode::PromptAlreadyClear;
        m_activeAlternatives.clear();
        m_activeQuestion.clear();
        m_activeDimension.clear();
        m_ambiguityScore = 0.0;
        emit interventionChanged();
        return;
    }

    const std::string promptStr = text.toStdString();

    auto triage = m_s1Adapter.triagePrompt(promptStr);
    m_ambiguityScore = triage.ambiguityScore;

    if (triage.fastPathClear) {
        m_currentDecision = core::intent::InterventionDecision{};
        m_currentDecision.verdict = core::intent::InterventionVerdict::Quiet;
        m_currentDecision.reason = core::intent::InterventionReasonCode::PromptAlreadyClear;
        m_activeAlternatives.clear();
        m_activeQuestion.clear();
        m_activeDimension.clear();
        emit interventionChanged();
        return;
    }

    bridge::GraniteOptionDiscoveryRequest req{
        .sessionId = m_revisionState ? m_revisionState->currentRevision().sessionId().value() : "sess_default",
        .revisionId = m_revisionState ? m_revisionState->currentRevision().revisionId().value() : "rev_default",
        .promptString = promptStr,
        .s1Category = triage.dimension,
        .hostContextCategory = m_hostContext.categoryString().toStdString()
    };

    auto discovery = m_graniteAdapter.discoverOptions(req);
    if (!discovery.success || !discovery.choiceGroup.has_value()) {
        m_currentDecision.verdict = core::intent::InterventionVerdict::Manual;
        m_currentDecision.reason = core::intent::InterventionReasonCode::ProviderUnavailable;
        emit interventionChanged();
        return;
    }

    auto group = *discovery.choiceGroup;
    for (auto& alt : group.alternatives) {
        auto match = core::analysis::findUniqueSpan(promptStr, alt.interpretation);
        if (match.status == core::analysis::SpanMatchStatus::ExactMatch && match.span) {
            alt.evidence.push_back(*match.span);
        } else {
            const std::size_t spacePos = alt.interpretation.find(' ');
            const std::string token = (spacePos != std::string::npos) ? alt.interpretation.substr(0, spacePos) : alt.interpretation;
            auto tokenMatch = core::analysis::findUniqueSpan(promptStr, token);
            if (tokenMatch.status == core::analysis::SpanMatchStatus::ExactMatch && tokenMatch.span) {
                alt.evidence.push_back(*tokenMatch.span);
            }
        }
    }

    bool hasAnySpan = false;
    for (const auto& alt : group.alternatives) {
        if (!alt.evidence.empty()) {
            hasAnySpan = true;
            break;
        }
    }
    if (!hasAnySpan && !promptStr.empty()) {
        const auto spacePos = promptStr.find(' ');
        const std::size_t spanEnd = (spacePos != std::string::npos && spacePos > 0) ? spacePos : promptStr.size();
        for (auto& alt : group.alternatives) {
            alt.evidence.push_back(core::analysis::SourceSpan{0, spanEnd});
        }
    }

    m_currentAnalysis = core::analysis::NormalizedAnalysis{};
    m_currentAnalysis.id = "snap_" + req.revisionId;
    m_currentAnalysis.baseRevisionId = req.revisionId;
    m_currentAnalysis.choiceGroups.push_back(group);

    m_currentPolicyResult = core::analysis::MaterialityPolicyV1::evaluatePrompt(m_currentAnalysis, promptStr);
    m_currentSnapshotEval = m_s1Adapter.evaluateSnapshot(m_currentAnalysis);

    m_workingIntent = core::intent::WorkingIntentState{};

    m_currentDecision = core::intent::InterventionStateMachine::evaluate(
        m_currentAnalysis,
        m_currentPolicyResult,
        m_currentSnapshotEval,
        m_workingIntent
    );

    m_activeAlternatives.clear();
    if (m_currentDecision.activeItemId.has_value()) {
        const std::string activeId = *m_currentDecision.activeItemId;
        for (const auto& g : m_currentAnalysis.choiceGroups) {
            if (g.id == activeId) {
                m_activeQuestion = QString::fromStdString(g.question);
                m_activeDimension = QString::fromStdString(g.dimension);

                for (const auto& alt : g.alternatives) {
                    QVariantMap altMap;
                    altMap[QStringLiteral("id")] = QString::fromStdString(alt.id);
                    altMap[QStringLiteral("label")] = QString::fromStdString(alt.interpretation);
                    altMap[QStringLiteral("whyMaterial")] = QString::fromStdString(alt.whyMaterial);

                    double prob = 0.5;
                    for (const auto& gs : m_currentSnapshotEval.groupScores) {
                        if (gs.groupId == g.id) {
                            for (const auto& ap : gs.probabilities) {
                                if (ap.alternativeId == alt.id) {
                                    prob = ap.probability;
                                    break;
                                }
                            }
                            break;
                        }
                    }
                    altMap[QStringLiteral("probability")] = prob;
                    m_activeAlternatives.append(altMap);
                }
                break;
            }
        }
    }

    emit interventionChanged();
}

void HostController::confirmChoice(const QString& alternativeId) {
    if (!m_currentDecision.activeItemId.has_value()) return;

    const std::string activeId = *m_currentDecision.activeItemId;
    m_workingIntent.confirmChoice(activeId, alternativeId.toStdString());

    m_currentDecision = core::intent::InterventionStateMachine::evaluate(
        m_currentAnalysis,
        m_currentPolicyResult,
        m_currentSnapshotEval,
        m_workingIntent
    );

    m_activeAlternatives.clear();
    if (m_currentDecision.activeItemId.has_value()) {
        const std::string nextId = *m_currentDecision.activeItemId;
        for (const auto& g : m_currentAnalysis.choiceGroups) {
            if (g.id == nextId) {
                m_activeQuestion = QString::fromStdString(g.question);
                m_activeDimension = QString::fromStdString(g.dimension);

                for (const auto& alt : g.alternatives) {
                    QVariantMap altMap;
                    altMap[QStringLiteral("id")] = QString::fromStdString(alt.id);
                    altMap[QStringLiteral("label")] = QString::fromStdString(alt.interpretation);
                    altMap[QStringLiteral("whyMaterial")] = QString::fromStdString(alt.whyMaterial);
                    m_activeAlternatives.append(altMap);
                }
                break;
            }
        }
    } else {
        m_activeQuestion.clear();
        m_activeDimension.clear();
    }

    if (m_currentDecision.verdict == core::intent::InterventionVerdict::Quiet) {
        rebuildConfirmedIntent();
    }

    emit choiceConfirmed(QString::fromStdString(activeId), alternativeId);
    emit interventionChanged();
}

void HostController::submitCustomIntent(const QString& customText) {
    if (!m_currentDecision.activeItemId.has_value()) return;
    const std::string activeId = *m_currentDecision.activeItemId;
    m_workingIntent.confirmChoice(activeId, customText.toStdString());
    m_currentDecision = core::intent::InterventionStateMachine::evaluate(
        m_currentAnalysis,
        m_currentPolicyResult,
        m_currentSnapshotEval,
        m_workingIntent
    );
    if (m_currentDecision.verdict == core::intent::InterventionVerdict::Quiet) {
        rebuildConfirmedIntent();
    }
    emit interventionChanged();
}

void HostController::skipCurrentItem() {
    if (!m_currentDecision.activeItemId.has_value()) return;
    const std::string activeId = *m_currentDecision.activeItemId;
    m_workingIntent.markResolved(activeId);
    ++m_workingIntent.roundsCompleted;
    m_currentDecision = core::intent::InterventionStateMachine::evaluate(
        m_currentAnalysis,
        m_currentPolicyResult,
        m_currentSnapshotEval,
        m_workingIntent
    );
    if (m_currentDecision.verdict == core::intent::InterventionVerdict::Quiet) {
        rebuildConfirmedIntent();
    }
    emit interventionChanged();
}

void HostController::switchToManual() {
    m_currentDecision.verdict = core::intent::InterventionVerdict::Manual;
    m_currentDecision.reason = core::intent::InterventionReasonCode::PolicyRequiresManualReview;
    emit interventionChanged();
}

void HostController::rebuildConfirmedIntent() {
    std::vector<core::intent::ConfirmedItem> items;
    for (const auto& [groupId, altId] : m_workingIntent.confirmedAlternatives) {
        std::string label = altId;
        std::string rationale;
        for (const auto& g : m_currentAnalysis.choiceGroups) {
            if (g.id == groupId) {
                for (const auto& alt : g.alternatives) {
                    if (alt.id == altId) {
                        label = alt.interpretation;
                        rationale = alt.whyMaterial;
                        break;
                    }
                }
                break;
            }
        }
        items.push_back(core::intent::ConfirmedItem{
            .itemId = groupId,
            .kind = core::intent::IntentResolutionKind::SelectedAlternative,
            .chosenValue = altId,
            .rationale = rationale.empty() ? label : rationale
        });
    }

    const std::string revId = m_revisionState ? m_revisionState->currentRevision().revisionId().value() : "rev_1";
    m_confirmedIntent = core::intent::IntentProvenanceBuilder::buildConfirmedIntent(
        revId,
        m_currentAnalysis,
        items,
        m_customInstructions.toStdString()
    );
    emit intentChanged();
}

void HostController::confirmLooksRight() {
    rebuildConfirmedIntent();
}

void HostController::keepOriginalPrompt() {
    std::vector<core::intent::ConfirmedItem> items;
    for (const auto& g : m_currentAnalysis.choiceGroups) {
        items.push_back(core::intent::ConfirmedItem{
            .itemId = g.id,
            .kind = core::intent::IntentResolutionKind::UseOriginal,
            .chosenValue = "ORIGINAL_PROMPT",
            .rationale = "User explicitly requested to keep original phrasing"
        });
    }
    const std::string revId = m_revisionState ? m_revisionState->currentRevision().revisionId().value() : "rev_1";
    m_confirmedIntent = core::intent::IntentProvenanceBuilder::buildConfirmedIntent(
        revId,
        m_currentAnalysis,
        items,
        m_customInstructions.toStdString()
    );
    m_currentDecision.verdict = core::intent::InterventionVerdict::Quiet;
    m_currentDecision.reason = core::intent::InterventionReasonCode::AllMaterialItemsResolved;
    emit interventionChanged();
    emit intentChanged();
}

void HostController::invalidateIntent() {
    if (m_confirmedIntent.has_value()) {
        m_confirmedIntent->isCurrent = false;
        emit intentChanged();
    }
}

void HostController::setRewriteProfile(const QString& profile) {
    if (m_rewriteProfile != profile) {
        m_rewriteProfile = profile;
        emit rewriteProfileChanged(profile);
        emit intentChanged();
    }
}

QVariantMap HostController::getConfirmedIntentPayload() const {
    QVariantMap payload;
    if (!m_confirmedIntent.has_value()) return payload;

    payload[QStringLiteral("intentId")] = QString::fromStdString(m_confirmedIntent->id);
    payload[QStringLiteral("baseRevisionId")] = QString::fromStdString(m_confirmedIntent->baseRevisionId);
    payload[QStringLiteral("sourceSnapshotId")] = QString::fromStdString(m_confirmedIntent->sourceSnapshotId);
    payload[QStringLiteral("sourceSnapshotHash")] = QString::number(m_confirmedIntent->sourceSnapshotHash, 16);
    payload[QStringLiteral("isCurrent")] = m_confirmedIntent->isCurrent && !isIntentStale();
    payload[QStringLiteral("rewriteProfile")] = m_rewriteProfile;
    payload[QStringLiteral("customInstructions")] = QString::fromStdString(m_confirmedIntent->customInstructions);
    payload[QStringLiteral("items")] = confirmedItems();
    return payload;
}

} // namespace roclint::host
