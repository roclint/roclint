#include "HostController.hpp"
#include "support/check.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <filesystem>
#include <iostream>

using roclint::host::HostController;

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    std::cout << "[Roclint Persistence Integration Test (I03)] Starting...\n";

    const QString testDbPath = QStringLiteral("./test_host_durability.db");
    std::filesystem::remove(testDbPath.toStdString());

    QString savedSessionId;
    const QString prompt1 = QStringLiteral("First draft of authentication flow.");
    const QString prompt2 = QStringLiteral("Refined draft: require OAuth2 PKCE constraint.");

    // =========================================================================
    // 1. Commit Prompts in Session 1 and Persist to SQLite
    // =========================================================================
    {
        std::cout << "  -> Stage 1: Creating HostController and committing prompts...\n";
        HostController controller(testDbPath);

        ROCLINT_CHECK(controller.sessionCount() == 0);
        ROCLINT_CHECK(controller.stateVersion() == 0);

        // Commit root prompt
        controller.commitPrompt(prompt1);
        ROCLINT_CHECK(controller.stateVersion() == 1);
        ROCLINT_CHECK(controller.sessionCount() == 1);

        // Commit prompt edit
        controller.commitPrompt(prompt2);
        ROCLINT_CHECK(controller.stateVersion() == 2);
        ROCLINT_CHECK(controller.sessionCount() == 1);

        auto sessions = controller.getRecentSessions();
        ROCLINT_CHECK(sessions.size() == 1);

        const auto first = sessions[0].toMap();
        savedSessionId = first[QStringLiteral("sessionId")].toString();
        ROCLINT_CHECK(!savedSessionId.isEmpty());
        ROCLINT_CHECK(first[QStringLiteral("revisionCount")].toInt() == 2);
        ROCLINT_CHECK(first[QStringLiteral("latestPrompt")].toString() == prompt2);

        std::cout << "  [PASS] Prompts committed and stored in SQLite (sessionId: "
                  << savedSessionId.toStdString() << ")\n";
    }

    // =========================================================================
    // 2. Reopen New HostController on Same DB: Durability & Lineage Check
    // =========================================================================
    {
        std::cout << "  -> Stage 2: Reopening HostController on same SQLite database...\n";
        HostController controller2(testDbPath);

        ROCLINT_CHECK(controller2.sessionCount() == 1);

        auto sessions = controller2.getRecentSessions();
        ROCLINT_CHECK(sessions.size() == 1);

        const auto first = sessions[0].toMap();
        ROCLINT_CHECK(first[QStringLiteral("sessionId")].toString() == savedSessionId);
        ROCLINT_CHECK(first[QStringLiteral("revisionCount")].toInt() == 2);
        ROCLINT_CHECK(first[QStringLiteral("latestPrompt")].toString() == prompt2);

        // Inspect revision history for the session
        auto history = controller2.getSessionHistory(savedSessionId);
        ROCLINT_CHECK(history.size() == 2);
        ROCLINT_CHECK(history[0].toMap()[QStringLiteral("text")].toString() == prompt1);
        ROCLINT_CHECK(history[1].toMap()[QStringLiteral("text")].toString() == prompt2);

        std::cout << "  [PASS] Reopen durability verified: session and revisions survived.\n";

        // =====================================================================
        // 3. Test Restore Action (B03 / I03)
        // =====================================================================
        std::cout << "  -> Stage 3: Testing session restoration...\n";
        QString restoredText;
        QObject::connect(&controller2, &HostController::revisionRestored, [&](const QString& text) {
            restoredText = text;
        });

        controller2.restoreSession(savedSessionId);
        ROCLINT_CHECK(restoredText == prompt2);
        ROCLINT_CHECK(controller2.stateVersion() > 0);

        std::cout << "  [PASS] Session restored into active controller state.\n";

        // =====================================================================
        // 4. Test Logical Delete Action (B03 / I03 / C02)
        // =====================================================================
        std::cout << "  -> Stage 4: Testing session deletion...\n";
        controller2.deleteSession(savedSessionId);

        ROCLINT_CHECK(controller2.sessionCount() == 0);
        ROCLINT_CHECK(controller2.getRecentSessions().isEmpty());
        ROCLINT_CHECK(controller2.getSessionHistory(savedSessionId).isEmpty());

        std::cout << "  [PASS] Session logically deleted and unreferenced revisions cascaded.\n";
    }

    // Clean up test database file
    std::filesystem::remove(testDbPath.toStdString());

    std::cout << "\n[Roclint Persistence Integration Test (I03)] All checks passed successfully!\n";
    return 0;
}
