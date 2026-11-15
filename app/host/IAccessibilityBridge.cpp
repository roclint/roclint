#include "IAccessibilityBridge.hpp"

#include <QMetaObject>

namespace roclint::host {

IAccessibilityBridge::IAccessibilityBridge(QObject* parent) : QObject(parent) {
    qRegisterMetaType<ObservedField>();
    qRegisterMetaType<InjectionRequest>();
    qRegisterMetaType<RangeInjectionRequest>();
    qRegisterMetaType<InjectionOutcome>();
}

void IAccessibilityBridge::injectRange(const RangeInjectionRequest& request) {
    const quint64 id = request.requestId;
    QMetaObject::invokeMethod(
        this,
        [this, id]() {
            emit rangeInjectionFinished(id, InjectionOutcome::NotSupported,
                                        QStringLiteral("This editor surface does not support range replacement."));
        },
        Qt::QueuedConnection);
}

const char* toString(AccessibilityAccess access) noexcept {
    switch (access) {
        case AccessibilityAccess::Granted: return "GRANTED";
        case AccessibilityAccess::NotGranted: return "NOT_GRANTED";
        case AccessibilityAccess::Unavailable: return "UNAVAILABLE";
    }
    return "UNAVAILABLE";
}

const char* toString(InjectionOutcome outcome) noexcept {
    switch (outcome) {
        case InjectionOutcome::Injected: return "INJECTED";
        case InjectionOutcome::FieldChanged: return "FIELD_CHANGED";
        case InjectionOutcome::NotSupported: return "NOT_SUPPORTED";
        case InjectionOutcome::Failed: return "FAILED";
    }
    return "FAILED";
}

namespace {

class UnavailableAccessibilityBridge final : public IAccessibilityBridge {
public:
    [[nodiscard]] AccessibilityAccess access() const override { return AccessibilityAccess::Unavailable; }
    bool requestAccess() override { return false; }
    void setObserving(bool) override {}
    void inject(const InjectionRequest& request) override {
        const quint64 id = request.requestId;
        QMetaObject::invokeMethod(
            this,
            [this, id]() {
                emit injectionFinished(id, InjectionOutcome::NotSupported,
                                       QStringLiteral("Accessibility isn't available on this system."));
            },
            Qt::QueuedConnection);
    }
};

} // namespace

std::unique_ptr<IAccessibilityBridge> createUnavailableAccessibilityBridge() {
    return std::make_unique<UnavailableAccessibilityBridge>();
}

#if !defined(Q_OS_WIN) && !defined(Q_OS_MAC)
std::unique_ptr<IAccessibilityBridge> createPlatformAccessibilityBridge() {
    return createUnavailableAccessibilityBridge();
}
#endif

} // namespace roclint::host
