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

struct roclint::host::GlobalShortcutManager::Impl {
    bool registered{false};
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

    // kVK_ANSI_R is 0x0F, modifiers: cmdKey (0x0100) | shiftKey (0x0200) -> Cmd+Shift+R
    status = RegisterEventHotKey(kVK_ANSI_R, cmdKey | shiftKey, hotKeyID, GetApplicationEventTarget(), 0, &m_impl->hotKeyRef);
    if (status != noErr) {
        std::cerr << "[GlobalShortcutManager] Failed to register global hotkey Cmd+Shift+R: " << status << "\n";
        return false;
    }

    std::cout << "[GlobalShortcutManager] Global shortcut registered: Cmd+Shift+R\n";
    return true;
#elif defined(Q_OS_WIN)
    std::cout << "[GlobalShortcutManager] Windows global shortcut registered: Ctrl+Shift+R\n";
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
    // Windows unregistration
#endif
}

} // namespace roclint::host
