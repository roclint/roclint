#include "HostContext.hpp"
#include "OutsideClickPolicy.hpp"
#include "GuiCapturePolicy.hpp"
#include "support/check.hpp"

#include <QGuiApplication>
#include <QWindow>

#include <iostream>

using roclint::host::HostContextCategory;
using roclint::host::HostContextDetector;
using roclint::host::HostContextObservation;

namespace {

void testPageDocumentIsNotPromptComposer() {
    using roclint::host::GuiCaptureRole;
    using roclint::host::isGuiCaptureEligible;
    // Native Antigravity reproduction: the page is focused and has a mixed
    // read-only text range. Even a wrongly writable root is never a composer.
    ROCLINT_CHECK(!isGuiCaptureEligible(GuiCaptureRole::Document, false, true));
    ROCLINT_CHECK(!isGuiCaptureEligible(GuiCaptureRole::Document, true, true));
    ROCLINT_CHECK(!isGuiCaptureEligible(GuiCaptureRole::Document, false));
    ROCLINT_CHECK(isGuiCaptureEligible(GuiCaptureRole::Document, true));
    // Antigravity exposes its actual contenteditable as a combobox.
    ROCLINT_CHECK(isGuiCaptureEligible(GuiCaptureRole::ComboBox, true));
    ROCLINT_CHECK(!isGuiCaptureEligible(GuiCaptureRole::ComboBox, false));
    ROCLINT_CHECK(isGuiCaptureEligible(GuiCaptureRole::Edit, false));
    ROCLINT_CHECK(!isGuiCaptureEligible(GuiCaptureRole::Other, true));
    using roclint::host::isGuiCaptureTextReliable;
    // A falsely empty ValuePattern must not authorize overwriting a visible
    // nonempty composer. Real empty text ranges and normal edit fields still work.
    ROCLINT_CHECK(!isGuiCaptureTextReliable(GuiCaptureRole::ComboBox, true, true, false));
    ROCLINT_CHECK(isGuiCaptureTextReliable(GuiCaptureRole::ComboBox, true, true, true));
    ROCLINT_CHECK(isGuiCaptureTextReliable(GuiCaptureRole::ComboBox, true, false, false));
    ROCLINT_CHECK(isGuiCaptureTextReliable(GuiCaptureRole::Edit, true, true, false));
    ROCLINT_CHECK(!isGuiCaptureTextReliable(GuiCaptureRole::Edit, false, true, false));
    ROCLINT_CHECK(!isGuiCaptureTextReliable(GuiCaptureRole::Edit, true, true, false, true));
    ROCLINT_CHECK(!isGuiCaptureTextReliable(GuiCaptureRole::Document, true, true, true, true));
    ROCLINT_CHECK(isGuiCaptureTextReliable(GuiCaptureRole::Edit, true, false, false, true));
    using roclint::host::isRichComposerPlaceholder;
    ROCLINT_CHECK(isRichComposerPlaceholder(u"Work with Codex\r\n", {}, {}, u"ChatGPT.exe", 0));
    ROCLINT_CHECK(isRichComposerPlaceholder(u"Ask anything\n", u"Ask anything", {}, u"Antigravity.exe", -1));
    ROCLINT_CHECK(isRichComposerPlaceholder(u"Type a message", {}, u"Type a message", u"Code.exe", 0));
    ROCLINT_CHECK(!isRichComposerPlaceholder(u"Work with Codex", {}, {}, u"ChatGPT.exe", 4));
    ROCLINT_CHECK(!isRichComposerPlaceholder(u"Work with Codex on tests", u"Work with Codex", {}, u"ChatGPT.exe", 0));
    ROCLINT_CHECK(!isRichComposerPlaceholder(u"Work with Codex", {}, {}, u"Antigravity.exe", 0));
}

HostContextCategory map(const char* id) {
    return HostContextDetector::mapIdentityToCategory(QString::fromUtf8(id));
}

void testWindowsExecutables() {
    ROCLINT_CHECK(map("Code.exe") == HostContextCategory::CodeEditor);
    ROCLINT_CHECK(map("Cursor.exe") == HostContextCategory::CodeEditor);
    ROCLINT_CHECK(map("ChatGPT.exe") == HostContextCategory::CodeEditor);
    for (const auto* identity : {"ChatGPT.exe", "codex.exe", "Code.exe", "Windsurf.exe", "devenv.exe", "studio64.exe"}) {
        ROCLINT_CHECK(HostContextDetector::isAmbientEligibleApp(QString::fromUtf8(identity)));
    }
    ROCLINT_CHECK(!HostContextDetector::isAmbientEligibleApp(QStringLiteral("notepad.exe")));
    ROCLINT_CHECK(!HostContextDetector::isAmbientEligibleApp(QStringLiteral("ChatGPT.exe.bak")));
    ROCLINT_CHECK(map("devenv.exe") == HostContextCategory::CodeEditor);
    ROCLINT_CHECK(map("WindowsTerminal.exe") == HostContextCategory::Terminal);
    ROCLINT_CHECK(map("pwsh.exe") == HostContextCategory::Terminal);
    ROCLINT_CHECK(map("conhost.exe") == HostContextCategory::Terminal);
    ROCLINT_CHECK(map("cmd.exe") == HostContextCategory::Terminal);
    ROCLINT_CHECK(map("msedge.exe") == HostContextCategory::Browser);
    ROCLINT_CHECK(map("chrome.exe") == HostContextCategory::Browser);
    ROCLINT_CHECK(map("slack.exe") == HostContextCategory::Chat);
    ROCLINT_CHECK(map("WINWORD.EXE") == HostContextCategory::Document);
}

void testMacBundleIds() {
    ROCLINT_CHECK(map("com.microsoft.VSCode") == HostContextCategory::CodeEditor);
    ROCLINT_CHECK(map("com.apple.Terminal") == HostContextCategory::Terminal);
    ROCLINT_CHECK(map("com.googlecode.iterm2") == HostContextCategory::Terminal);
    ROCLINT_CHECK(map("com.apple.Safari") == HostContextCategory::Browser);
    ROCLINT_CHECK(map("com.tinyspeck.slackmacgap") == HostContextCategory::Chat);
}

void testUnknownAppsMapToUnknown() {
    ROCLINT_CHECK(map("SearchApp.exe") == HostContextCategory::Unknown);
    ROCLINT_CHECK(map("1Password.exe") == HostContextCategory::Unknown);
    ROCLINT_CHECK(map("") == HostContextCategory::Unknown);
    // Exact matching only: no substring or prefix heuristics.
    ROCLINT_CHECK(map("NotCode.exe") == HostContextCategory::Unknown);
    ROCLINT_CHECK(map("code") == HostContextCategory::Unknown);
    ROCLINT_CHECK(map("chrome.exe.bak") == HostContextCategory::Unknown);
    ROCLINT_CHECK(map("com.microsoft.vscode.helper") == HostContextCategory::Unknown);
    // Surrounding whitespace is tolerated, case is not significant.
    ROCLINT_CHECK(map("  CODE.EXE ") == HostContextCategory::CodeEditor);
}

void testObservationStrings() {
    HostContextObservation unknown;
    ROCLINT_CHECK(!unknown.isValid());
    ROCLINT_CHECK(unknown.categoryString() == "UNKNOWN");
    HostContextObservation term{HostContextCategory::Terminal, "Windows Terminal", "WindowsTerminal.exe"};
    ROCLINT_CHECK(term.isValid());
    ROCLINT_CHECK(term.categoryString() == "TERMINAL");
}

void testMockCaptureSeam() {
    HostContextDetector::setMockObservation(
        HostContextObservation{HostContextCategory::Browser, "Edge", "msedge.exe"});
    ROCLINT_CHECK(HostContextDetector::captureFrontmost().category == HostContextCategory::Browser);
    HostContextDetector::resetMock();
}

void testOutsideClickPolicy() {
    using namespace std::chrono_literals;
    using roclint::host::shouldDismissOutsideClick;
    ROCLINT_CHECK(!shouldDismissOutsideClick(false, false, 0ms));
    ROCLINT_CHECK(!shouldDismissOutsideClick(false, false, 399ms));
    ROCLINT_CHECK(shouldDismissOutsideClick(false, false, 400ms));
    ROCLINT_CHECK(shouldDismissOutsideClick(false, false, 2s));
    ROCLINT_CHECK(!shouldDismissOutsideClick(true, false, 2s));
    ROCLINT_CHECK(!shouldDismissOutsideClick(false, true, 2s));
}

void testOutsideClickMonitorLifecycle() {
    QWindow window;
    window.setFlags(Qt::Window | Qt::FramelessWindowHint);
    int callbacks = 0;
    roclint::host::installOutsideClickMonitor(&window, [&]() { ++callbacks; });
    // Reinstallation must replace the previous monitor without leaking a hook.
    roclint::host::installOutsideClickMonitor(&window, [&]() { ++callbacks; });
    roclint::host::uninstallOutsideClickMonitor();
    roclint::host::installOutsideClickMonitor(nullptr, [&]() { ++callbacks; });
    roclint::host::uninstallOutsideClickMonitor();
    ROCLINT_CHECK(callbacks == 0);
}

} // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    std::cout << "[Test HostContext] Starting...\n";
    testWindowsExecutables();
    testPageDocumentIsNotPromptComposer();
    testMacBundleIds();
    testUnknownAppsMapToUnknown();
    testObservationStrings();
    testMockCaptureSeam();
    testOutsideClickPolicy();
    testOutsideClickMonitorLifecycle();
    std::cout << "[Test HostContext] All tests PASSED.\n";
    return 0;
}
