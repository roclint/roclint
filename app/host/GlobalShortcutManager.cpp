#include "GlobalShortcutManager.hpp"
#include "HostController.hpp"
#include <QKeySequence>
#include <QDebug>

#include <iostream>

#if defined(Q_OS_MAC)
#include <Carbon/Carbon.h>

namespace {
static OSStatus hotKeyHandler(EventHandlerCallRef nextHandler, EventRef theEvent, void* userData) {
    (void)nextHandler;
    if (GetEventClass(theEvent) == kEventClassKeyboard && GetEventKind(theEvent) == kEventHotKeyPressed) {
        EventHotKeyID hotKeyID;
        GetEventParameter(theEvent, kEventParamDirectObject, typeEventHotKeyID, nullptr, sizeof(hotKeyID), nullptr, &hotKeyID);
        if (hotKeyID.signature == 'ROCL' && hotKeyID.id == 1) {
            auto* manager = static_cast<roclint::host::GlobalShortcutManager*>(userData);
            if (manager) {
                QMetaObject::invokeMethod(manager, &roclint::host::GlobalShortcutManager::activated, Qt::QueuedConnection);
            }
        }
    }
    return noErr;
}
} // namespace

struct roclint::host::GlobalShortcutManager::Impl {
    EventHotKeyRef hotKeyRef{nullptr};
    EventHandlerRef handlerRef{nullptr};
};

#elif defined(Q_OS_WIN)
#include <windows.h>
#include <QAbstractNativeEventFilter>
#include <QCoreApplication>

namespace {
class WinHotKeyFilter : public QAbstractNativeEventFilter {
public:
    explicit WinHotKeyFilter(roclint::host::GlobalShortcutManager* manager)
        : m_manager(manager) {}

    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override {
        Q_UNUSED(result);
        if (eventType == "windows_generic_MSG" || eventType == "windows_dispatcher_MSG") {
            auto* msg = static_cast<MSG*>(message);
            if (msg && msg->message == WM_HOTKEY && msg->wParam == 1) {
                if (qEnvironmentVariableIsSet("ROCLINT_CAPTURE_DIAGNOSTICS"))
                    qInfo() << "[Roclint invocation] native hotkey event=" << eventType;
                if (m_manager) {
                    QMetaObject::invokeMethod(m_manager, &roclint::host::GlobalShortcutManager::activated, Qt::QueuedConnection);
                }
                return true;
            }
        }
        return false;
    }

private:
    roclint::host::GlobalShortcutManager* m_manager{nullptr};
};
} // namespace

struct roclint::host::GlobalShortcutManager::Impl {
    bool registered{false};
    std::unique_ptr<WinHotKeyFilter> filter;
};

#else
struct roclint::host::GlobalShortcutManager::Impl {};
#endif

namespace roclint::host {

GlobalShortcutManager::GlobalShortcutManager(HostController* controller, QObject* parent)
    : QObject(parent), m_controller(controller), m_impl(std::make_unique<Impl>()) {
    if (m_controller) {
        connect(this, &GlobalShortcutManager::activated, m_controller, &HostController::toggleCard);
        connect(m_controller, &HostController::shortcutChangeRequested, this, &GlobalShortcutManager::changeShortcut);
    }
}

GlobalShortcutManager::~GlobalShortcutManager() {
    unregisterShortcut();
}

#ifdef ROCLINT_ENABLE_TEST_MOCKS
bool GlobalShortcutManager::dispatchNativeHotkeyForTesting(const QByteArray& eventType, quint32 messageId, quintptr id) {
#ifdef Q_OS_WIN
    WinHotKeyFilter filter(this);
    MSG message{};
    message.message = messageId; message.wParam = id;
    qintptr result = 0;
    return filter.nativeEventFilter(eventType, &message, &result);
#else
    Q_UNUSED(eventType); Q_UNUSED(messageId); Q_UNUSED(id);
    return false;
#endif
}
#endif

bool GlobalShortcutManager::registerShortcut() {
    if (!m_registeredSequence.isEmpty()) return true;
    const QString requested = m_controller ? m_controller->invocationShortcut() : QStringLiteral("Ctrl+Shift+R");
    const QString normalized = normalizeShortcut(requested);
    if (!normalized.isEmpty() && registerSequence(normalized)) {
        if (m_controller) m_controller->acceptInvocationShortcut(normalized);
        return true;
    }

    // Fallbacks if requested shortcut is contested by another app (e.g. Win32 error 1409)
    static const QString fallbacks[] = {
        QStringLiteral("Ctrl+Shift+R"),
        QStringLiteral("Ctrl+Shift+Space"),
        QStringLiteral("Alt+Space")
    };
    for (const auto& alt : fallbacks) {
        if (alt == normalized) continue;
        if (registerSequence(alt)) {
            if (m_controller) {
                m_controller->acceptInvocationShortcut(alt);
                m_controller->setShortcutRegistrationFailed(
                    QStringLiteral("Primary shortcut %1 was unavailable; fell back to %2.")
                        .arg(normalized.isEmpty() ? requested : normalized, alt));
            }
            return true;
        }
    }
    return false;
}

QString GlobalShortcutManager::normalizeShortcut(const QString& sequence) {
    const QKeySequence parsed = QKeySequence::fromString(sequence, QKeySequence::PortableText);
    if (parsed.count() != 1) return {};
    const auto key = parsed[0].key();
    const auto modifiers = parsed[0].keyboardModifiers();
    const bool supported = (key >= Qt::Key_A && key <= Qt::Key_Z) ||
                           (key >= Qt::Key_0 && key <= Qt::Key_9) ||
                           (key >= Qt::Key_F1 && key <= Qt::Key_F12) || key == Qt::Key_Space;
    if (!supported || !(modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) return {};
    return parsed.toString(QKeySequence::PortableText);
}

void GlobalShortcutManager::changeShortcut(const QString& sequence) {
    const QString normalized = normalizeShortcut(sequence);
    if (normalized.isEmpty()) {
        if (m_controller) m_controller->setShortcutRegistrationFailed(
            QStringLiteral("Use Ctrl/Cmd, Alt or Meta with a letter, digit, F1–F12 or Space, for example Ctrl+Shift+R."));
        return;
    }
    if (normalized == m_registeredSequence) return;
    const QString previous = m_registeredSequence;
    unregisterShortcut();
    if (registerSequence(normalized)) {
        if (m_controller) m_controller->acceptInvocationShortcut(normalized);
        return;
    }
    const bool restored = !previous.isEmpty() && registerSequence(previous);
    if (m_controller) m_controller->setShortcutRegistrationFailed(
        restored ? QStringLiteral("That shortcut is unavailable. Your previous shortcut still works; choose another combination.")
                 : QStringLiteral("That shortcut is unavailable. Open Roclint from the tray icon and choose another combination."));
}

bool GlobalShortcutManager::registerSequence(const QString& sequence) {
    const auto combination = QKeySequence::fromString(sequence, QKeySequence::PortableText)[0];
#if defined(Q_OS_MAC)
    EventTypeSpec eventType;
    eventType.eventClass = kEventClassKeyboard;
    eventType.eventKind = kEventHotKeyPressed;

    OSStatus status = InstallApplicationEventHandler(&hotKeyHandler, 1, &eventType, this, &m_impl->handlerRef);
    if (status != noErr) {
        std::cerr << "[GlobalShortcutManager] Failed to install Carbon event handler: " << status << "\n";
        return false;
    }

    EventHotKeyID hotKeyID;
    hotKeyID.signature = 'ROCL';
    hotKeyID.id = 1;

    constexpr UInt32 letters[] = {kVK_ANSI_A, kVK_ANSI_B, kVK_ANSI_C, kVK_ANSI_D, kVK_ANSI_E, kVK_ANSI_F,
        kVK_ANSI_G, kVK_ANSI_H, kVK_ANSI_I, kVK_ANSI_J, kVK_ANSI_K, kVK_ANSI_L, kVK_ANSI_M, kVK_ANSI_N,
        kVK_ANSI_O, kVK_ANSI_P, kVK_ANSI_Q, kVK_ANSI_R, kVK_ANSI_S, kVK_ANSI_T, kVK_ANSI_U, kVK_ANSI_V,
        kVK_ANSI_W, kVK_ANSI_X, kVK_ANSI_Y, kVK_ANSI_Z};
    constexpr UInt32 digits[] = {kVK_ANSI_0, kVK_ANSI_1, kVK_ANSI_2, kVK_ANSI_3, kVK_ANSI_4,
        kVK_ANSI_5, kVK_ANSI_6, kVK_ANSI_7, kVK_ANSI_8, kVK_ANSI_9};
    constexpr UInt32 functions[] = {kVK_F1, kVK_F2, kVK_F3, kVK_F4, kVK_F5, kVK_F6,
        kVK_F7, kVK_F8, kVK_F9, kVK_F10, kVK_F11, kVK_F12};
    const auto key = combination.key();
    const UInt32 code = key >= Qt::Key_A && key <= Qt::Key_Z ? letters[key - Qt::Key_A]
                      : key >= Qt::Key_0 && key <= Qt::Key_9 ? digits[key - Qt::Key_0]
                      : key >= Qt::Key_F1 && key <= Qt::Key_F12 ? functions[key - Qt::Key_F1] : kVK_Space;
    const auto qtModifiers = combination.keyboardModifiers();
    // Qt's portable Ctrl key denotes Command on macOS; Meta denotes Control.
    UInt32 modifiers = 0;
    if (qtModifiers & Qt::ControlModifier) modifiers |= cmdKey;
    if (qtModifiers & Qt::MetaModifier) modifiers |= controlKey;
    if (qtModifiers & Qt::AltModifier) modifiers |= optionKey;
    if (qtModifiers & Qt::ShiftModifier) modifiers |= shiftKey;
    status = RegisterEventHotKey(code, modifiers, hotKeyID, GetApplicationEventTarget(), 0, &m_impl->hotKeyRef);
    if (status != noErr) {
        unregisterShortcut();
        return false;
    }
    m_registeredSequence = sequence;
    return true;

#elif defined(Q_OS_WIN)
    UINT modifiers = 0;
    const auto qtModifiers = combination.keyboardModifiers();
    if (qtModifiers & Qt::ControlModifier) modifiers |= MOD_CONTROL;
    if (qtModifiers & Qt::MetaModifier) modifiers |= MOD_WIN;
    if (qtModifiers & Qt::AltModifier) modifiers |= MOD_ALT;
    if (qtModifiers & Qt::ShiftModifier) modifiers |= MOD_SHIFT;
#if defined(MOD_NOREPEAT)
    modifiers |= MOD_NOREPEAT;
#endif

    const auto key = combination.key();
    const UINT nativeKey = key >= Qt::Key_F1 && key <= Qt::Key_F12
                               ? VK_F1 + static_cast<UINT>(key - Qt::Key_F1) : static_cast<UINT>(key);
    BOOL success = RegisterHotKey(nullptr, 1, modifiers, nativeKey);
    if (!success) {
        return false;
    }

    m_impl->filter = std::make_unique<WinHotKeyFilter>(this);
    if (QCoreApplication::instance()) {
        QCoreApplication::instance()->installNativeEventFilter(m_impl->filter.get());
    }
    m_impl->registered = true;

    m_registeredSequence = sequence;
    return true;

#else
    Q_UNUSED(combination);
    std::cerr << "[GlobalShortcutManager] Global shortcuts not supported on this platform.\n";
    return false;
#endif
}

void GlobalShortcutManager::unregisterShortcut() {
    m_registeredSequence.clear();
#if defined(Q_OS_MAC)
    if (m_impl->hotKeyRef) {
        UnregisterEventHotKey(m_impl->hotKeyRef);
        m_impl->hotKeyRef = nullptr;
    }
    if (m_impl->handlerRef) {
        RemoveEventHandler(m_impl->handlerRef);
        m_impl->handlerRef = nullptr;
    }
#elif defined(Q_OS_WIN)
    if (m_impl->registered) {
        UnregisterHotKey(nullptr, 1);
        m_impl->registered = false;
    }
    if (m_impl->filter && QCoreApplication::instance()) {
        QCoreApplication::instance()->removeNativeEventFilter(m_impl->filter.get());
        m_impl->filter.reset();
    }
#endif
}

} // namespace roclint::host
