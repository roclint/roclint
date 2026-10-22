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

        HostContextCategory cat = HostContextDetector::mapIdentityToCategory(bundleId, displayName);
        return HostContextObservation{cat, displayName, bundleId};
    }
}

bool HostContextDetector::isAccessibilityPermissionGranted() noexcept {
    NSDictionary* options = @{(__bridge id)kAXTrustedCheckOptionPrompt: @NO};
    return AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)options);
}

bool HostContextDetector::performDirectInjection(const QString& text) {
    @autoreleasepool {
        if (!isAccessibilityPermissionGranted()) {
            return false;
        }

        AXUIElementRef systemWide = AXUIElementCreateSystemWide();
        if (!systemWide) {
            return false;
        }

        AXUIElementRef focusedElement = NULL;
        AXError err = AXUIElementCopyAttributeValue(systemWide, kAXFocusedUIElementAttribute, (CFTypeRef*)&focusedElement);
        CFRelease(systemWide);

        if (err != kAXErrorSuccess || !focusedElement) {
            return false;
        }

        NSString* nsText = text.toNSString();
        err = AXUIElementSetAttributeValue(focusedElement, kAXValueAttribute, (__bridge CFTypeRef)nsText);
        CFRelease(focusedElement);

        return (err == kAXErrorSuccess);
    }
}

} // namespace roclint::host
#endif
