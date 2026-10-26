#include "HostContext.hpp"

#include <QHash>
#include <QSet>

#if defined(Q_OS_WIN)
#include <windows.h>
#include <psapi.h>
#endif

namespace roclint::host {

#ifdef ROCLINT_ENABLE_TEST_MOCKS
namespace {
std::optional<HostContextObservation>& mockObservation() {
    static std::optional<HostContextObservation> value;
    return value;
}
}

void HostContextDetector::setMockObservation(std::optional<HostContextObservation> mock) {
    mockObservation() = std::move(mock);
}

void HostContextDetector::resetMock() {
    mockObservation().reset();
}
#endif

#if defined(Q_OS_MAC)
// Implemented in HostContext_mac.mm
HostContextObservation captureFrontmostNative();
#elif defined(Q_OS_WIN)
namespace {
HostContextObservation captureFrontmostNative() {
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) {
        return HostContextObservation{};
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == GetCurrentProcessId()) {
        return HostContextObservation{};
    }

    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) {
        return HostContextObservation{};
    }

    WCHAR exePath[MAX_PATH];
    DWORD size = MAX_PATH;
    QString exeName;
    if (QueryFullProcessImageNameW(hProc, 0, exePath, &size)) {
        const QString fullPath = QString::fromWCharArray(exePath, static_cast<qsizetype>(size));
        const qsizetype lastSlash = fullPath.lastIndexOf(QLatin1Char('\\'));
        exeName = (lastSlash >= 0) ? fullPath.mid(lastSlash + 1) : fullPath;
    }
    CloseHandle(hProc);

    if (exeName.isEmpty()) {
        return HostContextObservation{};
    }

    QString displayName = exeName;
    if (displayName.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) {
        displayName.chop(4);
    }

    return HostContextObservation{HostContextDetector::mapIdentityToCategory(exeName), displayName, exeName};
}
}
#else
namespace {
HostContextObservation captureFrontmostNative() {
    return HostContextObservation{};
}
}
#endif

HostContextObservation HostContextDetector::captureFrontmost() {
#ifdef ROCLINT_ENABLE_TEST_MOCKS
    if (mockObservation().has_value()) {
        return *mockObservation();
    }
#endif
    return captureFrontmostNative();
}

namespace {
const QHash<QString, HostContextCategory>& identityTable() {
    using C = HostContextCategory;
    static const QHash<QString, HostContextCategory> table = {
        // Code editors: Windows executables
        {QStringLiteral("code.exe"), C::CodeEditor},
        {QStringLiteral("code - insiders.exe"), C::CodeEditor},
        {QStringLiteral("cursor.exe"), C::CodeEditor},
        {QStringLiteral("chatgpt.exe"), C::CodeEditor}, // Codex desktop executable
        {QStringLiteral("codex.exe"), C::CodeEditor},
        {QStringLiteral("antigravity.exe"), C::CodeEditor},
        {QStringLiteral("windsurf.exe"), C::CodeEditor},
        {QStringLiteral("studio64.exe"), C::CodeEditor},
        {QStringLiteral("devenv.exe"), C::CodeEditor},
        {QStringLiteral("sublime_text.exe"), C::CodeEditor},
        {QStringLiteral("idea64.exe"), C::CodeEditor},
        {QStringLiteral("clion64.exe"), C::CodeEditor},
        {QStringLiteral("pycharm64.exe"), C::CodeEditor},
        {QStringLiteral("webstorm64.exe"), C::CodeEditor},
        {QStringLiteral("rider64.exe"), C::CodeEditor},
        {QStringLiteral("goland64.exe"), C::CodeEditor},
        {QStringLiteral("zed.exe"), C::CodeEditor},
        {QStringLiteral("notepad++.exe"), C::CodeEditor},
        // Code editors: macOS bundle identifiers
        {QStringLiteral("com.microsoft.vscode"), C::CodeEditor},
        {QStringLiteral("com.microsoft.vscodeinsiders"), C::CodeEditor},
        {QStringLiteral("com.todesktop.230313mzl4wgnlh"), C::CodeEditor},
        {QStringLiteral("com.todesktop.230313mzl4w4u92"), C::CodeEditor},
        {QStringLiteral("com.google.antigravity-ide"), C::CodeEditor},
        {QStringLiteral("com.google.antigravity"), C::CodeEditor},
        {QStringLiteral("com.apple.dt.xcode"), C::CodeEditor},
        {QStringLiteral("com.sublimetext.4"), C::CodeEditor},
        {QStringLiteral("com.jetbrains.clion"), C::CodeEditor},
        {QStringLiteral("com.jetbrains.intellij"), C::CodeEditor},
        {QStringLiteral("com.jetbrains.pycharm"), C::CodeEditor},
        {QStringLiteral("dev.zed.zed"), C::CodeEditor},

        // Terminals: Windows executables
        {QStringLiteral("windowsterminal.exe"), C::Terminal},
        {QStringLiteral("pwsh.exe"), C::Terminal},
        {QStringLiteral("powershell.exe"), C::Terminal},
        {QStringLiteral("cmd.exe"), C::Terminal},
        {QStringLiteral("conhost.exe"), C::Terminal},
        {QStringLiteral("alacritty.exe"), C::Terminal},
        {QStringLiteral("wezterm-gui.exe"), C::Terminal},
        // Terminals: macOS bundle identifiers
        {QStringLiteral("com.apple.terminal"), C::Terminal},
        {QStringLiteral("com.googlecode.iterm2"), C::Terminal},
        {QStringLiteral("io.alacritty"), C::Terminal},
        {QStringLiteral("dev.warp.warp-stable"), C::Terminal},
        {QStringLiteral("net.kovidgoyal.kitty"), C::Terminal},
        {QStringLiteral("com.github.wez.wezterm"), C::Terminal},

        // Browsers: Windows executables
        {QStringLiteral("msedge.exe"), C::Browser},
        {QStringLiteral("chrome.exe"), C::Browser},
        {QStringLiteral("firefox.exe"), C::Browser},
        {QStringLiteral("brave.exe"), C::Browser},
        {QStringLiteral("opera.exe"), C::Browser},
        {QStringLiteral("vivaldi.exe"), C::Browser},
        {QStringLiteral("arc.exe"), C::Browser},
        // Browsers: macOS bundle identifiers
        {QStringLiteral("com.google.chrome"), C::Browser},
        {QStringLiteral("com.apple.safari"), C::Browser},
        {QStringLiteral("org.mozilla.firefox"), C::Browser},
        {QStringLiteral("com.microsoft.edgemac"), C::Browser},
        {QStringLiteral("com.brave.browser"), C::Browser},
        {QStringLiteral("company.thebrowser.browser"), C::Browser},

        // Chat: Windows executables
        {QStringLiteral("slack.exe"), C::Chat},
        {QStringLiteral("discord.exe"), C::Chat},
        {QStringLiteral("ms-teams.exe"), C::Chat},
        {QStringLiteral("teams.exe"), C::Chat},
        {QStringLiteral("telegram.exe"), C::Chat},
        {QStringLiteral("signal.exe"), C::Chat},
        // Chat: macOS bundle identifiers
        {QStringLiteral("com.tinyspeck.slackmacgap"), C::Chat},
        {QStringLiteral("com.hnc.discord"), C::Chat},
        {QStringLiteral("com.microsoft.teams2"), C::Chat},
        {QStringLiteral("ru.keepcoder.telegram"), C::Chat},
        {QStringLiteral("org.whispersystems.signal-desktop"), C::Chat},
        {QStringLiteral("com.apple.mobilesms"), C::Chat},

        // Documents: Windows executables
        {QStringLiteral("winword.exe"), C::Document},
        {QStringLiteral("notion.exe"), C::Document},
        {QStringLiteral("obsidian.exe"), C::Document},
        {QStringLiteral("notepad.exe"), C::Document},
        {QStringLiteral("typora.exe"), C::Document},
        // Documents: macOS bundle identifiers
        {QStringLiteral("com.microsoft.word"), C::Document},
        {QStringLiteral("notion.id"), C::Document},
        {QStringLiteral("md.obsidian"), C::Document},
        {QStringLiteral("com.apple.iwork.pages"), C::Document},
        {QStringLiteral("com.apple.notes"), C::Document},

        // Known applications that are not prompt-writing surfaces
        {QStringLiteral("spotify.exe"), C::Other},
        {QStringLiteral("explorer.exe"), C::Other},
        {QStringLiteral("com.spotify.client"), C::Other},
        {QStringLiteral("com.apple.finder"), C::Other},
    };
    return table;
}
}

HostContextCategory HostContextDetector::mapIdentityToCategory(const QString& identifier) {
    const QString key = identifier.trimmed().toLower();
    if (key.isEmpty()) {
        return HostContextCategory::Unknown;
    }
    const auto& table = identityTable();
    const auto it = table.constFind(key);
    return it == table.constEnd() ? HostContextCategory::Unknown : it.value();
}

bool HostContextDetector::isAmbientEligibleApp(const QString& identifier) {
    const QString id = identifier.trimmed().toLower();
    if (id.isEmpty()) {
        return false;
    }
    if (id.contains(QLatin1String("antigravity")) ||
        id.contains(QLatin1String("cursor")) ||
        id.contains(QLatin1String("todesktop")) ||
        id.contains(QLatin1String("codex")) ||
        id.contains(QLatin1String("claude")) ||
        id.contains(QLatin1String("vscode")) ||
        id.contains(QLatin1String("windsurf")) ||
        id.contains(QLatin1String("terminal")) ||
        id.contains(QLatin1String("iterm")) ||
        id.contains(QLatin1String("ghostty")) ||
        id.contains(QLatin1String("warp"))) {
        return true;
    }
    static const QSet<QString> identities = {
        QStringLiteral("chatgpt.exe"), QStringLiteral("codex.exe"),
        QStringLiteral("cursor.exe"), QStringLiteral("antigravity.exe"), QStringLiteral("claude.exe"),
        QStringLiteral("windsurf.exe"), QStringLiteral("code.exe"),
        QStringLiteral("code - insiders.exe"), QStringLiteral("devenv.exe"),
        QStringLiteral("idea64.exe"), QStringLiteral("clion64.exe"),
        QStringLiteral("pycharm64.exe"), QStringLiteral("webstorm64.exe"),
        QStringLiteral("rider64.exe"), QStringLiteral("goland64.exe"),
        QStringLiteral("studio64.exe"), QStringLiteral("zed.exe"),
        QStringLiteral("windowsterminal.exe"),
        QStringLiteral("com.todesktop.230313mzl4wgnlh"), QStringLiteral("com.todesktop.230313mzl4w4u92"),
        QStringLiteral("com.google.antigravity"), QStringLiteral("com.google.antigravity-ide"),
        QStringLiteral("com.openai.codex"), QStringLiteral("com.anthropic.claude"),
        QStringLiteral("com.microsoft.vscode"), QStringLiteral("com.microsoft.vscodeinsiders"),
        QStringLiteral("com.apple.terminal"), QStringLiteral("com.googlecode.iterm2"),
        QStringLiteral("com.mitchellh.ghostty"), QStringLiteral("dev.warp.warp-stable"),
    };
    return identities.contains(id);
}

#if !defined(Q_OS_MAC)
#if defined(Q_OS_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

void configureNonActivatingPanel(QWindow* window) {
    if (!window) return;
#if defined(Q_OS_WIN)
    HWND hwnd = reinterpret_cast<HWND>(window->winId());
    if (hwnd) {
        SetWindowLong(hwnd, GWL_EXSTYLE, GetWindowLong(hwnd, GWL_EXSTYLE) | WS_EX_NOACTIVATE | WS_EX_TOPMOST);
    }
#endif
}

void configureHostProcess() {}
void installOutsideClickMonitor(QWindow*, std::function<void()>) {}
void uninstallOutsideClickMonitor() {}
#endif

} // namespace roclint::host
