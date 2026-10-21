#include "GlobalShortcutManager.hpp"
#include "HostController.hpp"

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
        if (eventType == "windows_generic_MSG") {
            auto* msg = static_cast<MSG*>(message);
            if (msg && msg->message == WM_HOTKEY && msg->wParam == 1) {
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
    }
}

GlobalShortcutManager::~GlobalShortcutManager() {
    unregisterShortcut();
}

bool GlobalShortcutManager::registerShortcut() {
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

    // kVK_ANSI_L is 0x25, modifiers: cmdKey (0x0100) | shiftKey (0x0200) -> Cmd+Shift+L
    status = RegisterEventHotKey(kVK_ANSI_L, cmdKey | shiftKey, hotKeyID, GetApplicationEventTarget(), 0, &m_impl->hotKeyRef);
    if (status != noErr) {
        std::cerr << "[GlobalShortcutManager] Failed to register global hotkey Cmd+Shift+L: " << status << "\n";
        return false;
    }

    std::cout << "[GlobalShortcutManager] macOS global shortcut registered: Cmd+Shift+L\n";
    return true;

#elif defined(Q_OS_WIN)
    // Windows 10/11: RegisterHotKey (Ctrl + Shift + L) with MOD_NOREPEAT
    UINT modifiers = MOD_CONTROL | MOD_SHIFT;
#if defined(MOD_NOREPEAT)
    modifiers |= MOD_NOREPEAT;
#endif

    // ID = 1, VK_L = 'L' (0x4C)
    BOOL success = RegisterHotKey(nullptr, 1, modifiers, 'L');
    if (!success) {
        std::cerr << "[GlobalShortcutManager] Failed to register Windows global hotkey Ctrl+Shift+L (error " << GetLastError() << ")\n";
        return false;
    }

    m_impl->filter = std::make_unique<WinHotKeyFilter>(this);
    if (QCoreApplication::instance()) {
        QCoreApplication::instance()->installNativeEventFilter(m_impl->filter.get());
    }
    m_impl->registered = true;

    std::cout << "[GlobalShortcutManager] Windows 10/11 global shortcut registered: Ctrl+Shift+L\n";
    return true;

#else
    std::cerr << "[GlobalShortcutManager] Global shortcuts not supported on this platform.\n";
    return false;
#endif
}

void GlobalShortcutManager::unregisterShortcut() {
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
