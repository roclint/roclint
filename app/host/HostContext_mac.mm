#include "HostContext.hpp"

#if defined(Q_OS_MAC)
#include <QGuiApplication>
#include <QEvent>
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>

namespace roclint::host {

namespace {
bool usesCocoa() {
    return QGuiApplication::platformName() == QLatin1String("cocoa");
}

NSWindow* cocoaWindow(QWindow* window) {
    if (!window || !usesCocoa()) return nil;
    NSView* view = (__bridge NSView*)reinterpret_cast<void*>(window->winId());
    return [view window];
}
}

HostContextObservation captureFrontmostNative() {
    @autoreleasepool {
        NSRunningApplication* frontApp = [[NSWorkspace sharedWorkspace] frontmostApplication];
        if (!frontApp) {
            return HostContextObservation{HostContextCategory::Unknown, QString(), QString()};
        }

        NSString* bId = [frontApp bundleIdentifier];
        NSString* locName = [frontApp localizedName];

        QString bundleId = bId ? QString::fromNSString(bId) : QString();
        QString displayName = locName ? QString::fromNSString(locName) : QString();

        // Avoid self-referencing Roclint
        if (bundleId.contains("roclint", Qt::CaseInsensitive) ||
            displayName.contains("roclint", Qt::CaseInsensitive)) {
            return HostContextObservation{HostContextCategory::Unknown, QString(), QString()};
        }

        HostContextCategory cat = HostContextDetector::mapIdentityToCategory(bundleId);
        return HostContextObservation{cat, displayName, bundleId};
    }
}

void configureNonActivatingPanel(QWindow* window) {
    if (!window) return;
    @autoreleasepool {
        NSWindow* nsWindow = cocoaWindow(window);
        if (nsWindow) {
            [nsWindow setStyleMask:([nsWindow styleMask] | NSWindowStyleMaskNonactivatingPanel)];
            [nsWindow setCollectionBehavior:(NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorFullScreenAuxiliary)];
            [nsWindow setHidesOnDeactivate:NO];
            if ([nsWindow isKindOfClass:[NSPanel class]]) {
                [(NSPanel*)nsWindow setBecomesKeyOnlyIfNeeded:YES];
            }
            [nsWindow setLevel:NSFloatingWindowLevel];
        }
    }
}

void showNonActivatingWindow(QWindow* window) {
    if (!window) return;
    if (!usesCocoa()) {
        window->show();
        return;
    }
    // Temporarily suppress Qt's default [NSApp activateIgnoringOtherApps:YES] and
    // makeKeyAndOrderFront during setVisible(true), then restore keyboard focus acceptance
    // so user controls (e.g. text inputs in FloatingCard) can receive key events when clicked.
    window->setFlag(Qt::WindowDoesNotAcceptFocus, true);
    window->setVisible(true);
    window->setFlag(Qt::WindowDoesNotAcceptFocus, false);
    @autoreleasepool {
        NSWindow* nsWindow = cocoaWindow(window);
        if (nsWindow) {
            [nsWindow setStyleMask:([nsWindow styleMask] | NSWindowStyleMaskNonactivatingPanel)];
            [nsWindow setCollectionBehavior:(NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorFullScreenAuxiliary)];
            [nsWindow setHidesOnDeactivate:NO];
            if ([nsWindow isKindOfClass:[NSPanel class]]) {
                [(NSPanel*)nsWindow setBecomesKeyOnlyIfNeeded:YES];
            }
            [nsWindow setLevel:NSFloatingWindowLevel];
            [nsWindow orderFrontRegardless];
        }
    }
}

// Underlines are dynamic QML windows. Apply the same native panel setup
// as the companion each time a segment becomes visible.
class WordMarkWindowFilter final : public QObject {
public:
    using QObject::QObject;
    bool eventFilter(QObject* object, QEvent* event) override {
        if (event->type() == QEvent::Show) {
            auto* window = qobject_cast<QWindow*>(object);
            if (window && window->objectName().startsWith(QLatin1String("wordMark_")))
                configureNonActivatingPanel(window);
        }
        return false;
    }
};

void configureHostProcess() {
    if (!usesCocoa()) return;
    qGuiApp->installEventFilter(new WordMarkWindowFilter(qGuiApp));
    @autoreleasepool {
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    }
}

static id g_outsideClickMonitor = nil;

void installOutsideClickMonitor(QWindow* window, std::function<void()> onOutsideClick) {
    if (!window || !usesCocoa()) return;
    @autoreleasepool {
        if (g_outsideClickMonitor != nil) {
            [NSEvent removeMonitor:g_outsideClickMonitor];
            g_outsideClickMonitor = nil;
        }
        NSWindow* nsWindow = cocoaWindow(window);
        const NSTimeInterval installTime = [NSDate timeIntervalSinceReferenceDate];
        NSRunningApplication* hostApp = [[NSWorkspace sharedWorkspace] frontmostApplication];
        const pid_t hostPid = hostApp ? [hostApp processIdentifier] : 0;
        g_outsideClickMonitor = [NSEvent addGlobalMonitorForEventsMatchingMask:(NSEventMaskLeftMouseDown | NSEventMaskRightMouseDown)
            handler:^(NSEvent* event) {
                (void)event;
                // Ignore click events during the initial 400ms opening window (avoids capturing the triggering click)
                if ([NSDate timeIntervalSinceReferenceDate] - installTime < 0.40) {
                    return;
                }
                if (nsWindow) {
                    const NSPoint mouseLoc = [NSEvent mouseLocation];
                    if (NSPointInRect(mouseLoc, [nsWindow frame])) {
                        return; // Click is inside the floating card window; do not dismiss
                    }
                }
                // If the click occurred while interacting inside the same host application (e.g. Cursor / VS Code),
                // keep the floating HUD active so the user can navigate between prompt boxes and editor fields.
                NSRunningApplication* front = [[NSWorkspace sharedWorkspace] frontmostApplication];
                const pid_t frontPid = front ? [front processIdentifier] : 0;
                if (hostPid > 0 && frontPid == hostPid) {
                    return;
                }
                if (onOutsideClick) {
                    dispatch_async(dispatch_get_main_queue(), ^{
                        onOutsideClick();
                    });
                }
            }];
    }
}

void uninstallOutsideClickMonitor() {
    @autoreleasepool {
        if (g_outsideClickMonitor != nil) {
            [NSEvent removeMonitor:g_outsideClickMonitor];
            g_outsideClickMonitor = nil;
        }
    }
}

} // namespace roclint::host
#endif
