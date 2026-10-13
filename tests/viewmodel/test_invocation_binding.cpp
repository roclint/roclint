#include "HostController.hpp"
#include "HostContext.hpp"
#include "ScreenPlacement.hpp"
#include "support/check.hpp"

#include <QGuiApplication>
#include <QClipboard>
#include <iostream>

using namespace roclint::host;

int main(int argc, char* argv[]) {
    // Initialize QGuiApplication for clipboard and display metrics
    QGuiApplication app(argc, argv);

    std::cout << "[Roclint Invocation & Stage 01 Checkpoint Test] Starting...\n";

    // =========================================================================
    // 1. Screen Placement & Boundary Clamping Verification (B02)
    // =========================================================================
    std::cout << "\n--- 1. Screen Placement & Clamping (B02) ---\n";
    {
        const int cardW = 820;
        const int cardH = 520;
        const QRect screenBounds(0, 0, 1920, 1080);

        // Case A: Cursor in normal center area
        QPoint centerCursor(960, 540);
        auto pCenter = ScreenPlacement::calculateClampedPosition(centerCursor, cardW, cardH, screenBounds, 12);
        ROCLINT_CHECK(pCenter.x == 960 - cardW / 2); // 960 - 410 = 550
        ROCLINT_CHECK(pCenter.y == 540 - 60);        // 480
        ROCLINT_CHECK(!pCenter.clamped);
        std::cout << "  [PASS] Centered placement calculation verified.\n";

        // Case B: Cursor at extreme top-left (would overflow off-screen left/top)
        QPoint topLeftCursor(10, 10);
        auto pTopLeft = ScreenPlacement::calculateClampedPosition(topLeftCursor, cardW, cardH, screenBounds, 12);
        ROCLINT_CHECK(pTopLeft.x == 12); // Clamped to left + margin
        ROCLINT_CHECK(pTopLeft.y == 12); // Clamped to top + margin
        ROCLINT_CHECK(pTopLeft.clamped);
        std::cout << "  [PASS] Top-left boundary collision clamped correctly.\n";

        // Case C: Cursor at extreme bottom-right (would overflow off-screen right/bottom)
        QPoint botRightCursor(1910, 1070);
        auto pBotRight = ScreenPlacement::calculateClampedPosition(botRightCursor, cardW, cardH, screenBounds, 12);
        ROCLINT_CHECK(pBotRight.x == 1920 - cardW - 12); // Clamped to right - cardW - margin
        ROCLINT_CHECK(pBotRight.y == 1080 - cardH - 12); // Clamped to bottom - cardH - margin
        ROCLINT_CHECK(pBotRight.clamped);
        std::cout << "  [PASS] Bottom-right boundary collision clamped correctly.\n";

        // Case D: Fallback for empty/invalid geometry
        QRect emptyScreen;
        auto pFallback = ScreenPlacement::calculateClampedPosition(centerCursor, cardW, cardH, emptyScreen, 12);
        ROCLINT_CHECK(pFallback.x == 100);
        ROCLINT_CHECK(pFallback.y == 100);
        std::cout << "  [PASS] Empty geometry deterministic fallback verified.\n";
    }

    // =========================================================================
    // 2. Passive Host-Context Categorization Verification (B02H / C03)
    // =========================================================================
    std::cout << "\n--- 2. Host-Context Category Mapping (B02H / C03) ---\n";
    {
        // Test Code Editor mapping
        ROCLINT_CHECK(HostContextDetector::mapIdentityToCategory("com.microsoft.VSCode", "Code") == HostContextCategory::CodeEditor);
        ROCLINT_CHECK(HostContextDetector::mapIdentityToCategory("com.cursor.Cursor", "Cursor") == HostContextCategory::CodeEditor);
        ROCLINT_CHECK(HostContextDetector::mapIdentityToCategory("com.apple.dt.Xcode", "Xcode") == HostContextCategory::CodeEditor);
        ROCLINT_CHECK(HostContextDetector::mapIdentityToCategory("com.jetbrains.CLion", "CLion") == HostContextCategory::CodeEditor);
        ROCLINT_CHECK(HostContextDetector::mapIdentityToCategory("Code.exe", "Code") == HostContextCategory::CodeEditor);

        // Test Terminal mapping
        ROCLINT_CHECK(HostContextDetector::mapIdentityToCategory("com.apple.Terminal", "Terminal") == HostContextCategory::Terminal);
        ROCLINT_CHECK(HostContextDetector::mapIdentityToCategory("com.googlecode.iterm2", "iTerm2") == HostContextCategory::Terminal);
        ROCLINT_CHECK(HostContextDetector::mapIdentityToCategory("io.alacritty", "Alacritty") == HostContextCategory::Terminal);
        ROCLINT_CHECK(HostContextDetector::mapIdentityToCategory("WindowsTerminal.exe", "WindowsTerminal") == HostContextCategory::Terminal);

        // Test Browser mapping
        ROCLINT_CHECK(HostContextDetector::mapIdentityToCategory("com.google.Chrome", "Google Chrome") == HostContextCategory::Browser);
        ROCLINT_CHECK(HostContextDetector::mapIdentityToCategory("com.apple.Safari", "Safari") == HostContextCategory::Browser);
        ROCLINT_CHECK(HostContextDetector::mapIdentityToCategory("company.thebrowser.Browser", "Arc") == HostContextCategory::Browser);

        // Test Chat mapping
        ROCLINT_CHECK(HostContextDetector::mapIdentityToCategory("com.tinyspeck.slackmacgap", "Slack") == HostContextCategory::Chat);
        ROCLINT_CHECK(HostContextDetector::mapIdentityToCategory("com.hnc.Discord", "Discord") == HostContextCategory::Chat);

        // Test Document mapping
        ROCLINT_CHECK(HostContextDetector::mapIdentityToCategory("md.obsidian", "Obsidian") == HostContextCategory::Document);
        ROCLINT_CHECK(HostContextDetector::mapIdentityToCategory("notion.id", "Notion") == HostContextCategory::Document);

        // Test Other and Unknown
        ROCLINT_CHECK(HostContextDetector::mapIdentityToCategory("com.spotify.client", "Spotify") == HostContextCategory::Other);
        ROCLINT_CHECK(HostContextDetector::mapIdentityToCategory("", "") == HostContextCategory::Unknown);

        std::cout << "  [PASS] All C03 category mappings verified.\n";
    }

    // =========================================================================
    // 3. Controller Host-Context Disclosure & Clear Action (B02H)
    // =========================================================================
    std::cout << "\n--- 3. Host-Context Disclosure & Clear Action (B02H) ---\n";
    {
        HostController controller;
        controller.markResident();

        // Inject mock observation fixture
        HostContextObservation mockObs{
            HostContextCategory::CodeEditor,
            QStringLiteral("Visual Studio Code"),
            QStringLiteral("com.microsoft.VSCode")
        };
        HostContextDetector::setMockObservation(mockObs);

        int contextChanges = 0;
        QObject::connect(&controller, &HostController::hostContextChanged, [&]() {
            ++contextChanges;
        });

        // Trigger invocation
        controller.showCard();
        ROCLINT_CHECK(controller.hasHostContext());
        ROCLINT_CHECK(controller.hostContextCategory() == "CODE_EDITOR");
        ROCLINT_CHECK(controller.hostContextDisplayName() == "Visual Studio Code");
        ROCLINT_CHECK(contextChanges >= 1);
        std::cout << "  [PASS] Host context captured and exposed as friendly display name & category.\n";

        // User clears context before analyze/commit
        controller.clearHostContext();
        ROCLINT_CHECK(!controller.hasHostContext());
        ROCLINT_CHECK(controller.hostContextCategory() == "UNKNOWN");
        ROCLINT_CHECK(controller.hostContextDisplayName().isEmpty());
        std::cout << "  [PASS] User can clear/disable host context hint.\n";

        HostContextDetector::resetMock();
    }

    // =========================================================================
    // 4. Invocation Clipboard Capture & Draft Pre-fill (B02 / C09)
    // =========================================================================
    std::cout << "\n--- 4. Clipboard Draft Capture on Invocation (B02 / C09) ---\n";
    {
        HostController controller;
        controller.markResident();

        // Seed clipboard with simulated text copied from IDE
        const QString samplePrompt = QStringLiteral("Refactor SessionManager to handle async token refreshes");
        if (auto* cb = QGuiApplication::clipboard()) {
            cb->setText(samplePrompt);
        }

        QString capturedDraft;
        QObject::connect(&controller, &HostController::draftCaptured, [&](const QString& text) {
            capturedDraft = text;
        });

        // Invoking showCard captures clipboard text once
        controller.showCard();
        ROCLINT_CHECK(controller.capturedDraftText() == samplePrompt);
        ROCLINT_CHECK(capturedDraft == samplePrompt);
        std::cout << "  [PASS] One-shot clipboard capture pre-fills draft text.\n";

        // Esc/Hide does NOT mutate clipboard or core state (I01 requirement)
        controller.hideCard();
        ROCLINT_CHECK(!controller.isCardVisible());
        if (auto* cb = QGuiApplication::clipboard()) {
            ROCLINT_CHECK(cb->text() == samplePrompt);
        }
        std::cout << "  [PASS] Dismissing card leaves clipboard and core state intact.\n";
    }

    // =========================================================================
    // 5. Revision State Checkpoint (I01 / I02)
    // =========================================================================
    std::cout << "\n--- 5. Revision State Machine Checkpoint (I01 / I02) ---\n";
    {
        HostController controller;
        controller.markResident();

        ROCLINT_CHECK(controller.stateVersion() == 0);
        ROCLINT_CHECK(controller.currentRevisionId() == "none");

        // Commit first prompt
        const QString draft1 = QStringLiteral("First revision draft text");
        controller.commitPrompt(draft1);
        ROCLINT_CHECK(controller.stateVersion() == 1);
        ROCLINT_CHECK(controller.currentRevisionId() == "rev_1");
        std::cout << "  [PASS] Initial prompt revision committed (stateVersion = 1).\n";

        // Commit second prompt (edit)
        const QString draft2 = QStringLiteral("Second revision draft text");
        controller.commitPrompt(draft2);
        ROCLINT_CHECK(controller.stateVersion() == 2);
        ROCLINT_CHECK(controller.currentRevisionId() == "rev_2");
        std::cout << "  [PASS] Second prompt revision committed (stateVersion = 2).\n";

        // Verify approved text copy back matches exact text
        const QString approvedPrompt = QStringLiteral("/* ROCLINT [SCOPE: CURRENT FILE] */\nRefactored text");
        controller.copyToClipboard(approvedPrompt);
        ROCLINT_CHECK(controller.lastCopiedText() == approvedPrompt);
        if (auto* cb = QGuiApplication::clipboard()) {
            ROCLINT_CHECK(cb->text() == approvedPrompt);
        }
        std::cout << "  [PASS] Approved text copy-back matches exact supplied string.\n";
    }

    std::cout << "\n=========================================================\n";
    std::cout << "  ALL STAGE 01 / I01 / I02 INTEGRATION CHECKS PASSED!\n";
    std::cout << "=========================================================\n";
    return 0;
}
