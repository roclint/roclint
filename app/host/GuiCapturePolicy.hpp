#pragma once

#include <QString>
#include <QStringView>
#include <optional>

namespace roclint::host {

inline constexpr qsizetype kMacGuiCaptureLimit = 16000;

// AX providers can report an empty value while exposing the actual draft
// through their character count and parameterized text range.
inline std::optional<QString> verifiedMacGuiText(const std::optional<QString>& value, bool verifyEmpty,
                                                  std::optional<qsizetype> count, const std::optional<QString>& range,
                                                  const QString& placeholder) {
    if (count && (*count < 0 || *count > kMacGuiCaptureLimit)) return std::nullopt;
    if (value && !value->isEmpty()) {
        if (value->size() > kMacGuiCaptureLimit || *value == placeholder) return std::nullopt;
        if (count && *count != value->size()) {
            if (range && range->size() == *count && range->startsWith(*value) && *range != placeholder) return range;
            return std::nullopt;
        }
        return value;
    }
    if (count && range && range->size() == *count && (range->isEmpty() || *range != placeholder)) return range;
    if (!verifyEmpty && value && (!count || *count == 0)) return value;
    return std::nullopt;
}

enum class GuiCaptureRole { Edit, Document, ComboBox, Group, Custom, Other };

[[nodiscard]] constexpr bool isGuiCaptureTextReliable(GuiCaptureRole role,
                                                      bool textRead,
                                                      bool textEmpty,
                                                      bool textRangeRead,
                                                      bool richEditor = false) noexcept {
    // Chromium can return an empty value even when its composer visibly holds
    // text. Until an adapter can distinguish these states, use Copy mode.
    if (richEditor && textEmpty) return false;
    return textRead && !(role == GuiCaptureRole::ComboBox && textEmpty && !textRangeRead);
}

// A rich editor's fallback range may contain its visible hint. A nonempty
// writable ValuePattern is handled by the caller before consulting fallback
// metadata. A caret inside the candidate is evidence that it is editable text.
[[nodiscard]] inline bool isRichComposerPlaceholder(QStringView candidate,
                                                      QStringView name,
                                                      QStringView helpText,
                                                      QStringView executable,
                                                      int caretOffset) noexcept {
    candidate = candidate.trimmed();
    if (candidate.isEmpty() || caretOffset > 0) return false;
    if (candidate == name.trimmed() || candidate == helpText.trimmed()) return true;

    // Recognize the Codex composer hint even if its provider omits label metadata.
    return candidate == u"Work with Codex" &&
           (executable.compare(u"codex.exe", Qt::CaseInsensitive) == 0 ||
            executable.compare(u"chatgpt.exe", Qt::CaseInsensitive) == 0);
}

// A page's document range may contain editable descendants without being an
// editor itself. Unknown/mixed read-only attributes do not establish editability.
[[nodiscard]] constexpr bool isGuiCaptureEligible(GuiCaptureRole role,
                                                  bool explicitlyWritable,
                                                  bool webDocumentRoot = false) noexcept {
    if (webDocumentRoot) return false;
    if (role == GuiCaptureRole::Edit) return true;
    return explicitlyWritable && (role == GuiCaptureRole::Document ||
                                  role == GuiCaptureRole::ComboBox ||
                                  role == GuiCaptureRole::Group ||
                                  role == GuiCaptureRole::Custom);
}

} // namespace roclint::host
