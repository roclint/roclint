#pragma once

#include <QMetaType>
#include <QObject>
#include <QRect>
#include <QString>
#include <QList>

#include <memory>

namespace roclint::host {

enum class AccessibilityAccess {
    Granted,     // The OS lets Roclint read and write other apps' focused fields
    NotGranted,  // A permission exists but the user hasn't granted it (macOS Accessibility)
    Unavailable  // No accessibility API on this system, or it failed to initialize
};

enum class FieldKind {
    GuiEditor, // A text box: the whole value is the prompt
    Terminal   // A terminal buffer: only the draft after the prompt delimiter is the prompt
};

// One observation of the focused text field in another application.
struct ObservedField {
    quint64 fieldToken{0};      // Changes whenever focus moves to a different element
    QString appIdentifier;      // Exact executable file name (Windows) or bundle identifier (macOS)
    QString appDisplayName;
    qint64 processId{0};
    FieldKind kind{FieldKind::GuiEditor};
    QString text;               // Whole field value, or the terminal's text buffer
    int caretOffset{-1};        // UTF-16 offset into text; -1 when the app doesn't report it
    QRect caretRect;            // Screen coordinates; null when the app doesn't report bounds
    QRect fieldRect;            // Screen coordinates of the field; null when unknown
    // Provider-verified word range. Empty on unsupported editors; never inferred
    // from font metrics. Offsets must match the host's exact selected span.
    int wordStartOffset{-1};
    int wordEndOffset{-1};
    QList<QRect> wordRects;
    bool isPassword{false};
    bool elevationMismatch{false}; // The target runs with higher privileges than Roclint (Windows UIPI)
    quint64 refreshRequestId{0}; // Nonzero only for an explicit focused-field refresh response.
    // Windows UIA uses physical pixels. The GUI thread normalizes exactly once
    // before storing the observation; macOS and test observations are logical.
    bool geometryInNativePixels{false};
    bool operator==(const ObservedField&) const = default;
};

struct InjectionRequest {
    quint64 requestId{0};
    quint64 fieldToken{0};
    FieldKind kind{FieldKind::GuiEditor};
    QString text;                // Replaces the whole field (GUI) or the unsubmitted draft (terminal)
    QString expectedCurrentText; // Stale guard: the field (or terminal draft) must still hold this
    bool terminalVimMode{false};
    qint64 processId{0};
    QString appIdentifier;
};

struct RangeInjectionRequest {
    quint64 requestId{0};
    quint64 fieldToken{0};
    FieldKind kind{FieldKind::GuiEditor};
    int startOffset{0};
    int endOffset{0};
    QString originalRangeText;
    QString replacementText;
    QString expectedCurrentText; // Full text before injection for stale guard
};

enum class InjectionOutcome {
    Injected,     // Written and read back unchanged
    FieldChanged, // Focus moved or the text changed since capture; nothing was written
    NotSupported, // The field accepts no programmatic input
    Failed        // An attempt was made but the read-back didn't match
};

// Reads and writes the focused text field of other applications through the OS accessibility API.
// Implementations do all accessibility I/O off the GUI thread and report through queued signals.
class IAccessibilityBridge : public QObject {
    Q_OBJECT
public:
    explicit IAccessibilityBridge(QObject* parent = nullptr);
    ~IAccessibilityBridge() override = default;

    [[nodiscard]] virtual AccessibilityAccess access() const = 0;
    // Shows the OS permission prompt where one exists. Returns false when there is nothing to request.
    virtual bool requestAccess() = 0;
    virtual void setObserving(bool observing) = 0;
    virtual void setCardVisible(bool visible) { Q_UNUSED(visible); }
    // One-shot refresh before invoking the HUD, while the external editor still owns focus.
    virtual void refreshFocusedField(quint64 requestId = 0) { Q_UNUSED(requestId); }
    // Completion is always reported through injectionFinished, including immediate refusals.
    virtual void inject(const InjectionRequest& request) = 0;
    // Specific range injection for word-level companion replacements. Implementations that do
    // not support range editing must override or inherit this explicit completion result.
    virtual void injectRange(const RangeInjectionRequest& request);

signals:
    void accessChanged();
    void fieldObserved(const roclint::host::ObservedField& field);
    void fieldLost();
    // The focused window is still current, but its editor could not be read.
    void fieldReadInterrupted();
    void injectionFinished(quint64 requestId, roclint::host::InjectionOutcome outcome, const QString& detail);
    void rangeInjectionFinished(quint64 requestId, roclint::host::InjectionOutcome outcome, const QString& detail);
};

// The accessibility bridge for this platform; an always-unavailable bridge elsewhere.
[[nodiscard]] std::unique_ptr<IAccessibilityBridge> createPlatformAccessibilityBridge();
[[nodiscard]] std::unique_ptr<IAccessibilityBridge> createUnavailableAccessibilityBridge();

[[nodiscard]] const char* toString(AccessibilityAccess access) noexcept;
[[nodiscard]] const char* toString(InjectionOutcome outcome) noexcept;

} // namespace roclint::host

Q_DECLARE_METATYPE(roclint::host::ObservedField)
Q_DECLARE_METATYPE(roclint::host::InjectionRequest)
Q_DECLARE_METATYPE(roclint::host::RangeInjectionRequest)
Q_DECLARE_METATYPE(roclint::host::InjectionOutcome)
