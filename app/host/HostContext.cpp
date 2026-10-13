#include "HostContext.hpp"

#if defined(Q_OS_WIN)
#include <windows.h>
#include <psapi.h>
#endif

namespace roclint::host {

std::optional<HostContextObservation> HostContextDetector::s_mockObservation = std::nullopt;

void HostContextDetector::setMockObservation(std::optional<HostContextObservation> mock) {
    s_mockObservation = std::move(mock);
}

void HostContextDetector::resetMock() {
    s_mockObservation = std::nullopt;
}

#if defined(Q_OS_MAC)
// Implemented in HostContext_mac.mm
HostContextObservation captureFrontmostNative();
#elif defined(Q_OS_WIN)
HostContextObservation captureFrontmostNative() {
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) {
        return HostContextObservation{HostContextCategory::Unknown, QString(), QString()};
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == GetCurrentProcessId()) {
        return HostContextObservation{HostContextCategory::Unknown, QString(), QString()};
    }

    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) {
        return HostContextObservation{HostContextCategory::Unknown, QString(), QString()};
    }

    WCHAR exePath[MAX_PATH];
    DWORD size = MAX_PATH;
    QString exeName;
    if (QueryFullProcessImageNameW(hProc, 0, exePath, &size)) {
        QString fullPath = QString::fromWCharArray(exePath);
        int lastSlash = fullPath.lastIndexOf(QLatin1Char('\\'));
        exeName = (lastSlash >= 0) ? fullPath.mid(lastSlash + 1) : fullPath;
    }
    CloseHandle(hProc);

    if (exeName.isEmpty() || exeName.contains(QStringLiteral("roclint"), Qt::CaseInsensitive)) {
        return HostContextObservation{HostContextCategory::Unknown, QString(), QString()};
    }

    QString displayName = exeName;
    if (displayName.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) {
        displayName.chop(4);
    }

    HostContextCategory cat = HostContextDetector::mapIdentityToCategory(exeName, displayName);
    return HostContextObservation{cat, displayName, exeName};
}
#else
static HostContextObservation captureFrontmostNative() {
    return HostContextObservation{HostContextCategory::Unknown, QString(), QString()};
}
#endif

HostContextObservation HostContextDetector::captureFrontmost() {
    if (s_mockObservation.has_value()) {
        return *s_mockObservation;
    }
    return captureFrontmostNative();
}

HostContextCategory HostContextDetector::mapIdentityToCategory(const QString& id, const QString& name) {
    if (id.trimmed().isEmpty() && name.trimmed().isEmpty()) {
        return HostContextCategory::Unknown;
    }

    const QString lowerId = id.toLower();
    const QString lowerName = name.toLower();

    // 1. CODE_EDITOR
    if (lowerId.contains("vscode") || lowerId.contains("cursor") ||
        lowerId.contains("com.todesktop.230313mzl4wgnlh") ||
        lowerId.contains("sublime") || lowerId.contains("xcode") ||
        lowerId.contains("clion") || lowerId.contains("intellij") ||
        lowerId.contains("pycharm") || lowerId.contains("webstorm") ||
        lowerId.contains("rider") || lowerId.contains("rubymine") ||
        lowerId.contains("goland") || lowerId.contains("atom") ||
        lowerId.contains("zed") || lowerId.contains("textmate") ||
        lowerId.contains("bbedit") || lowerId.contains("neovim") ||
        lowerId.contains("emacs") || lowerId.contains("devenv") ||
        lowerName == "code" || lowerName == "visual studio code" ||
        lowerName == "cursor" || lowerName == "xcode" ||
        lowerName == "clion" || lowerName == "sublime text" ||
        lowerName == "zed" || lowerName == "intellij idea") {
        return HostContextCategory::CodeEditor;
    }

    // 2. TERMINAL
    if (lowerId.contains("terminal") || lowerId.contains("iterm") ||
        lowerId.contains("alacritty") || lowerId.contains("warp") ||
        lowerId.contains("kitty") || lowerId.contains("wezterm") ||
        lowerId.contains("hyper") || lowerId.contains("windowsterminal") ||
        lowerId.contains("powershell") || lowerId.contains("cmd.exe") ||
        lowerName == "terminal" || lowerName == "iterm2" ||
        lowerName == "alacritty" || lowerName == "warp" ||
        lowerName == "kitty" || lowerName == "wezterm") {
        return HostContextCategory::Terminal;
    }

    // 3. BROWSER
    if (lowerId.contains("chrome") || lowerId.contains("safari") ||
        lowerId.contains("firefox") || lowerId.contains("edge") ||
        lowerId.contains("brave") || lowerId.contains("arc") ||
        lowerId.contains("opera") || lowerId.contains("vivaldi") ||
        lowerName == "google chrome" || lowerName == "safari" ||
        lowerName == "firefox" || lowerName == "microsoft edge" ||
        lowerName == "arc" || lowerName == "brave browser") {
        return HostContextCategory::Browser;
    }

    // 4. CHAT
    if (lowerId.contains("slack") || lowerId.contains("discord") ||
        lowerId.contains("teams") || lowerId.contains("telegram") ||
        lowerId.contains("signal") || lowerId.contains("messages") ||
        lowerId.contains("mattermost") ||
        lowerName == "slack" || lowerName == "discord" ||
        lowerName == "microsoft teams" || lowerName == "telegram" ||
        lowerName == "messages") {
        return HostContextCategory::Chat;
    }

    // 5. DOCUMENT
    if (lowerId.contains("notion") || lowerId.contains("obsidian") ||
        lowerId.contains("pages") || lowerId.contains("word") ||
        lowerId.contains("typora") || lowerId.contains("iawriter") ||
        lowerId.contains("notes") ||
        lowerName == "notion" || lowerName == "obsidian" ||
        lowerName == "pages" || lowerName == "microsoft word" ||
        lowerName == "notes") {
        return HostContextCategory::Document;
    }

    return HostContextCategory::Other;
}

} // namespace roclint::host
