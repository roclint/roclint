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

} // namespace roclint::host
#endif
