#pragma once

namespace roclint::host {

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
