#include "HostController.hpp"
#include "support/check.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>
#include <iostream>

using roclint::host::HostController;
namespace storage = roclint::storage;

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    std::cout << "[Test Host Persistence] Starting...\n";

    QTemporaryDir dir;
    ROCLINT_CHECK(dir.isValid());
    const QString folder = dir.path() + QString::fromUtf8("/donn" "\xC3\xA9" "es");
    ROCLINT_CHECK(QDir().mkpath(folder));
    const QString dbPath = folder + QStringLiteral("/history.db");

    const QString prompt1 = QStringLiteral("First draft of authentication flow.");
    const QString prompt2 = QStringLiteral("Refined draft: require OAuth2 PKCE.");
    QString sessionId;
    QString firstRevisionId;

    {
        HostController c(dbPath);
    ROCLINT_CHECK(c.waitForStorageForTesting());
        c.setLegacyDiscoveryForTesting(true);
        ROCLINT_CHECK(c.isStorageAvailable());
        ROCLINT_CHECK(c.sessionCount() == 0);
        ROCLINT_CHECK(c.stateVersion() == 0);

        ROCLINT_CHECK(c.waitForCommandForTesting(c.commitPrompt(prompt1, 0)));
        ROCLINT_CHECK(c.stateVersion() == 1);
        firstRevisionId = c.currentRevisionId();
        ROCLINT_CHECK(c.waitForCommandForTesting(c.commitPrompt(prompt2, 1)));
        ROCLINT_CHECK(c.stateVersion() == 2);
        ROCLINT_CHECK(c.sessionCount() == 1);

        const auto sessions = c.getRecentSessions();
        ROCLINT_CHECK(sessions.size() == 1);
        const auto first = sessions[0].toMap();
        sessionId = first[QStringLiteral("sessionId")].toString();
        ROCLINT_CHECK(sessionId == c.currentSessionIdString());
        ROCLINT_CHECK(first[QStringLiteral("revisionCount")].toInt() == 2);
        ROCLINT_CHECK(first[QStringLiteral("latestPrompt")].toString() == prompt2);
        ROCLINT_CHECK(first[QStringLiteral("latestRevisionId")].toString() == c.currentRevisionId());

        // Leave a model attempt open, as if the app were killed mid-request.
        storage::ModelAttemptRecord attempt;
        attempt.attemptId = "att_orphan";
        attempt.sessionId = sessionId.toStdString();
        attempt.revisionId = c.currentRevisionId().toStdString();
        attempt.stage = "discovery";
        attempt.transport = "ollama_loopback";
        attempt.startedAtMs = 1;
        ROCLINT_CHECK(c.repositoryForTesting()->beginModelAttempt(attempt).waitForTesting().ok());
    }
    std::cout << "  [PASS] Revisions are committed with optimistic versions and persisted.\n";

    {
        HostController c(dbPath);
    ROCLINT_CHECK(c.waitForStorageForTesting());
        c.setLegacyDiscoveryForTesting(true);
        ROCLINT_CHECK(c.sessionCount() == 1);
        const auto history = c.getSessionHistory(sessionId);
        ROCLINT_CHECK(history.size() == 2);
        const auto h0 = history[0].toMap();
        const auto h1 = history[1].toMap();
        ROCLINT_CHECK(h0[QStringLiteral("text")].toString() == prompt1);
        ROCLINT_CHECK(h0[QStringLiteral("sequence")].toInt() == 1);
        ROCLINT_CHECK(h0[QStringLiteral("parentRevisionId")].toString().isEmpty());
        ROCLINT_CHECK(h1[QStringLiteral("text")].toString() == prompt2);
        ROCLINT_CHECK(h1[QStringLiteral("sequence")].toInt() == 2);
        ROCLINT_CHECK(h1[QStringLiteral("parentRevisionId")].toString() == firstRevisionId);

        // Startup recovery closed the orphaned attempt.
        auto attempts = c.repositoryForTesting()->getModelAttempts(roclint::core::SessionId(sessionId.toStdString())).waitForTesting();
        ROCLINT_CHECK(attempts.ok() && attempts.value().size() == 1);
        ROCLINT_CHECK(attempts.value()[0].status == storage::AttemptStatus::Interrupted);
        ROCLINT_CHECK(attempts.value()[0].completedAtMs.has_value());
        std::cout << "  [PASS] Reopen keeps lineage and marks orphaned model attempts interrupted.\n";

        QString restoredText;
        QObject::connect(&c, &HostController::revisionRestored, [&](const QString& text) { restoredText = text; });
        ROCLINT_CHECK(c.waitForCommandForTesting(c.restoreSession(sessionId, c.stateVersion())));
        ROCLINT_CHECK(restoredText == prompt2);
        ROCLINT_CHECK(c.stateVersion() == 2);
        ROCLINT_CHECK(c.currentSessionIdString() == sessionId);

        // Restoring an older revision appends a new one; nothing is rewritten in place.
        ROCLINT_CHECK(c.waitForCommandForTesting(c.restoreRevision(firstRevisionId, c.stateVersion())));
        ROCLINT_CHECK(restoredText == prompt1);
        ROCLINT_CHECK(c.stateVersion() == 3);
        ROCLINT_CHECK(c.currentRevisionId() == QStringLiteral("rev_") + sessionId + QStringLiteral("_3"));
        const auto after = c.getSessionHistory(sessionId);
        ROCLINT_CHECK(after.size() == 3);
        ROCLINT_CHECK(after[2].toMap()[QStringLiteral("origin")].toString() == "RESTORE");
        ROCLINT_CHECK(!c.waitForCommandForTesting(c.restoreRevision(QStringLiteral("rev_other_1"), c.stateVersion())));
        std::cout << "  [PASS] Restore rebuilds the session and appends restored revisions.\n";

        ROCLINT_CHECK(c.waitForCommandForTesting(c.deleteSession(sessionId)));
        ROCLINT_CHECK(c.sessionCount() == 0);
        ROCLINT_CHECK(c.getRecentSessions().isEmpty());
        ROCLINT_CHECK(c.getSessionHistory(sessionId).isEmpty());
        ROCLINT_CHECK(c.stateVersion() == 0);
        ROCLINT_CHECK(c.currentSessionIdString().isEmpty());
        ROCLINT_CHECK(!c.waitForCommandForTesting(c.restoreSession(sessionId, c.stateVersion())));
        std::cout << "  [PASS] Deleting the open session removes it and resets the controller.\n";
    }

    std::cout << "[Test Host Persistence] All tests PASSED.\n";
    return 0;
}
