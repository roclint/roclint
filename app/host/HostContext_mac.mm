#include "HostContext.hpp"

#if defined(Q_OS_MAC)
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>

namespace roclint::host {

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
        NSView* view = (__bridge NSView*)reinterpret_cast<void*>(window->winId());
        NSWindow* nsWindow = [view window];
        if (nsWindow) {
            [nsWindow setStyleMask:([nsWindow styleMask] | NSWindowStyleMaskNonactivatingPanel)];
            [nsWindow setCollectionBehavior:(NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorFullScreenAuxiliary)];
            [nsWindow setHidesOnDeactivate:NO];
            [nsWindow setLevel:NSFloatingWindowLevel];
        }
    }
}

void configureHostProcess() {
    @autoreleasepool {
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    }
}

static id g_outsideClickMonitor = nil;

void installOutsideClickMonitor(QWindow* window, std::function<void()> onOutsideClick) {
    if (!window) return;
    @autoreleasepool {
        if (g_outsideClickMonitor != nil) {
            [NSEvent removeMonitor:g_outsideClickMonitor];
            g_outsideClickMonitor = nil;
        }
        NSView* view = (__bridge NSView*)reinterpret_cast<void*>(window->winId());
        NSWindow* nsWindow = [view window];
        const NSTimeInterval installTime = [NSDate timeIntervalSinceReferenceDate];
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
