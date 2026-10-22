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

    // =========================================================================
    // 5. Clarification & Dynamic Linter Binding Verification (B06)
    // =========================================================================
    std::cout << "\n--- 5. Clarification & Dynamic Linter Binding (B06) ---\n";
    {
        HostController controller;

        // Clear prompt test
        controller.analyzePrompt("Set red background color");
        ROCLINT_CHECK(!controller.isClarifying());
        ROCLINT_CHECK(controller.interventionVerdict() == "QUIET");
        std::cout << "  [PASS] Clear prompt results in QUIET state.\n";

        // Ambiguous prompt test
        controller.analyzePrompt("Which component should handle connection timeout: FloatingCard or HostController?");
        std::cout << "Verdict: " << controller.interventionVerdict().toStdString()
                  << ", Reason: " << controller.interventionReason().toStdString()
                  << ", Diagnostic: " << controller.diagnosticMessage().toStdString() << std::endl;
        ROCLINT_CHECK(controller.isClarifying());
        ROCLINT_CHECK(controller.interventionVerdict() == "CLARIFY");
        ROCLINT_CHECK(controller.activeAlternatives().size() == 2);
        ROCLINT_CHECK(controller.roundsCompleted() == 0);
        ROCLINT_CHECK(controller.remainingBudget() == 3);
        std::cout << "  [PASS] Ambiguous prompt triggers CLARIFY with 2 alternatives.\n";

        // User confirmation test
        const QString firstAltId = controller.activeAlternatives()[0].toMap()["id"].toString();
        controller.confirmChoice(firstAltId);
        ROCLINT_CHECK(controller.roundsCompleted() == 1);
        ROCLINT_CHECK(controller.remainingBudget() == 2);
        ROCLINT_CHECK(controller.interventionVerdict() == "QUIET");
        std::cout << "  [PASS] User confirmation advances rounds and reaches QUIET state.\n";
    }

    // =========================================================================
    // 6. Confirmed Intent Review & Provenance UX (B07 / C05 / C06)
    // =========================================================================
    std::cout << "\n--- 6. Confirmed Intent Review & Provenance UX (B07 / C05 / C06) ---\n";
    {
        HostController controller;
        controller.markResident();

        // 1. Initial state: No confirmed intent, rewrite disabled
        ROCLINT_CHECK(!controller.hasConfirmedIntent());
        ROCLINT_CHECK(!controller.isIntentStale());
        ROCLINT_CHECK(!controller.isRewriteEnabled());
        ROCLINT_CHECK(controller.rewriteProfile() == "GENERIC@1");

        // 2. Commit initial prompt and analyze ambiguous question
        const QString draft = "Which component should handle connection timeout: FloatingCard or HostController?";
        controller.commitPrompt(draft);
        controller.analyzePrompt(draft);

        ROCLINT_CHECK(controller.isClarifying());
        ROCLINT_CHECK(!controller.isRewriteEnabled());

        // 3. Confirm alternative choice -> produces ConfirmedIntent
        const QString firstAltId = controller.activeAlternatives()[0].toMap()["id"].toString();
        controller.confirmChoice(firstAltId);

        ROCLINT_CHECK(controller.interventionVerdict() == "QUIET");
        ROCLINT_CHECK(controller.hasConfirmedIntent());
        ROCLINT_CHECK(!controller.isIntentStale());
        ROCLINT_CHECK(controller.isRewriteEnabled());
        ROCLINT_CHECK(controller.confirmedItems().size() == 1);

        // Verify payload matches visible confirmed items
        QVariantMap payload = controller.getConfirmedIntentPayload();
        ROCLINT_CHECK(!payload["intentId"].toString().isEmpty());
        ROCLINT_CHECK(payload["baseRevisionId"].toString() == "rev_1");
        ROCLINT_CHECK(payload["isCurrent"].toBool() == true);
        ROCLINT_CHECK(payload["rewriteProfile"].toString() == "GENERIC@1");
        QVariantList items = payload["items"].toList();
        ROCLINT_CHECK(items.size() == 1);
        ROCLINT_CHECK(items[0].toMap()["chosenValue"].toString() == firstAltId);
        std::cout << "  [PASS] Confirmed intent built with accurate item payload and rewrite enabled.\n";

        // 4. Test target profile selection
        controller.setRewriteProfile("ANTHROPIC_STRUCTURED@1");
        ROCLINT_CHECK(controller.rewriteProfile() == "ANTHROPIC_STRUCTURED@1");
        ROCLINT_CHECK(controller.getConfirmedIntentPayload()["rewriteProfile"].toString() == "ANTHROPIC_STRUCTURED@1");
        std::cout << "  [PASS] Target profile selection updates rewrite configuration.\n";

        // 5. Test prompt edit -> instant stale invalidation (Contract C06)
        controller.commitPrompt("Modified prompt after confirmation");
        ROCLINT_CHECK(controller.isIntentStale());
        ROCLINT_CHECK(!controller.isRewriteEnabled()); // Rewrite must remain disabled!
        std::cout << "  [PASS] Editing prompt immediately invalidates intent and disables rewrite.\n";
    }

    // =========================================================================
    // 7. Granite Prompt Rewrite Synthesis & Validation (B08 / B08P / A10)
    // =========================================================================
    std::cout << "\n--- 7. Granite Prompt Rewrite Synthesis & Validation (B08 / B08P / A10) ---\n";
    {
        HostController controller;
        controller.markResident();

        const QString prompt = "Which component should handle connection timeout: FloatingCard or HostController?";
        controller.commitPrompt(prompt);
        controller.analyzePrompt(prompt);

        // Confirm first choice
        const QString firstAltId = controller.activeAlternatives()[0].toMap()["id"].toString();
        controller.confirmChoice(firstAltId);

        ROCLINT_CHECK(controller.hasConfirmedIntent());
        ROCLINT_CHECK(controller.isRewriteEnabled());

        // Test synthesizeRewrite
        QString compiled = controller.synthesizeRewrite();
        ROCLINT_CHECK(!compiled.isEmpty());
        ROCLINT_CHECK(controller.hasProposal());
        ROCLINT_CHECK(!controller.lastDiffText().isEmpty());
        ROCLINT_CHECK(controller.proposalConstraints().size() >= 2);
        ROCLINT_CHECK(controller.lastCopiedText() == compiled);
        std::cout << "  [PASS] Granite rewrite successfully synthesized, verified by A10, and copied to clipboard.\n";
    }

    std::cout << "\n=========================================================\n";
    std::cout << "  ALL STAGE 01 / I01 / I02 / B06 / B07 / B08 INTEGRATION CHECKS PASSED!\n";
    std::cout << "=========================================================\n";
    return 0;
}
