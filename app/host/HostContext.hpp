#pragma once

#include <QString>
#include <QWindow>
#include <functional>
#include <optional>

namespace roclint::host {

enum class HostContextCategory {
    CodeEditor,
    Terminal,
    Browser,
    Chat,
    Document,
    Other,
    Unknown
};

struct HostContextObservation {
    HostContextCategory category{HostContextCategory::Unknown};
    QString displayName;
    QString bundleOrExecutableId;

    [[nodiscard]] QString categoryString() const {
        switch (category) {
            case HostContextCategory::CodeEditor: return QStringLiteral("CODE_EDITOR");
            case HostContextCategory::Terminal:   return QStringLiteral("TERMINAL");
            case HostContextCategory::Browser:    return QStringLiteral("BROWSER");
            case HostContextCategory::Chat:       return QStringLiteral("CHAT");
            case HostContextCategory::Document:   return QStringLiteral("DOCUMENT");
            case HostContextCategory::Other:      return QStringLiteral("OTHER");
            case HostContextCategory::Unknown:    return QStringLiteral("UNKNOWN");
        }
        return QStringLiteral("UNKNOWN");
    }

    [[nodiscard]] bool isValid() const noexcept {
        return category != HostContextCategory::Unknown;
    }
};

class HostContextDetector {
public:
    // Captures the frontmost application one-shot at invocation time.
    static HostContextObservation captureFrontmost();

    // Exact, case-insensitive lookup of a macOS bundle identifier or a Windows
    // executable file name. Identifiers that are not listed map to Unknown.
    static HostContextCategory mapIdentityToCategory(const QString& identifier);

    // Shared by the controller and native bridge, before reading any field text.
    static bool isAmbientEligibleApp(const QString& identifier);

#ifdef ROCLINT_ENABLE_TEST_MOCKS
    static void setMockObservation(std::optional<HostContextObservation> mock);
    static void resetMock();
#endif
};

void configureNonActivatingPanel(QWindow* window);
void configureHostProcess();
void showNonActivatingWindow(QWindow* window);
void installOutsideClickMonitor(QWindow* window, std::function<void()> onOutsideClick);
void uninstallOutsideClickMonitor();

} // namespace roclint::host
