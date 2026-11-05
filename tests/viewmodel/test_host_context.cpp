#include "HostContext.hpp"
#include "OutsideClickPolicy.hpp"
#include "support/check.hpp"

#include <QGuiApplication>
#include <QWindow>

#include <iostream>

using roclint::host::HostContextCategory;
using roclint::host::HostContextDetector;
using roclint::host::HostContextObservation;

namespace {

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
    testMacBundleIds();
    testUnknownAppsMapToUnknown();
    testObservationStrings();
    testMockCaptureSeam();
    testOutsideClickPolicy();
    testOutsideClickMonitorLifecycle();
    std::cout << "[Test HostContext] All tests PASSED.\n";
    return 0;
}
