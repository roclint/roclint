#pragma once

#include <QString>
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
    // Captures the frontmost application one-shot at invocation time
    static HostContextObservation captureFrontmost();

    // Pure mapping function from process/bundle ID and display name to coarse category
    static HostContextCategory mapIdentityToCategory(const QString& id, const QString& name);

    // Testing / fixture injection support (B02H step 1)
    static void setMockObservation(std::optional<HostContextObservation> mock);
    static void resetMock();

private:
    static std::optional<HostContextObservation> s_mockObservation;
};

} // namespace roclint::host
