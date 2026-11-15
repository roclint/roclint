#pragma once

#include "IAccessibilityBridge.hpp"

#include <QMetaObject>

#include <vector>
#include <optional>
#include <utility>

namespace roclint::test {

// Scriptable accessibility bridge: tests push observations and choose injection outcomes. It never
// touches the real OS accessibility APIs, so it triggers no permission prompts.
class MockAccessibilityBridge final : public host::IAccessibilityBridge {
public:
    host::AccessibilityAccess accessValue{host::AccessibilityAccess::Granted};
    bool observing{false};
    bool cardVisible{false};
    int accessRequests{0};
    int refreshRequests{0};
    quint64 refreshId{0};
    bool echoOnRefresh{true};
    std::optional<host::ObservedField> lastObserved;
    std::vector<host::InjectionRequest> injections;
    std::vector<host::RangeInjectionRequest> rangeInjections;
    host::InjectionOutcome nextOutcome{host::InjectionOutcome::Injected};
    host::InjectionOutcome nextRangeOutcome{host::InjectionOutcome::Injected};

    [[nodiscard]] host::AccessibilityAccess access() const override { return accessValue; }

    bool requestAccess() override {
        ++accessRequests;
        return true;
    }

    void setObserving(bool value) override { observing = value; }
    void setCardVisible(bool visible) override { cardVisible = visible; }
    void refreshFocusedField(quint64 requestId = 0) override {
        ++refreshRequests; refreshId = requestId;
        if (echoOnRefresh && lastObserved) observe(*lastObserved);
    }

    void inject(const host::InjectionRequest& request) override {
        injections.push_back(request);
        const auto outcome = nextOutcome;
        const quint64 id = request.requestId;
        QMetaObject::invokeMethod(
            this,
            [this, id, outcome]() {
                emit injectionFinished(id, outcome,
                                       outcome == host::InjectionOutcome::Injected ? QString()
                                                                                   : QStringLiteral("mock refusal"));
            },
            Qt::QueuedConnection);
    }

    void injectRange(const host::RangeInjectionRequest& request) override {
        rangeInjections.push_back(request);
        const auto outcome = nextRangeOutcome;
        const quint64 id = request.requestId;
        QMetaObject::invokeMethod(
            this,
            [this, id, outcome]() {
                emit rangeInjectionFinished(id, outcome,
                    outcome == host::InjectionOutcome::Injected ? QString() : QStringLiteral("mock range refusal"));
            },
            Qt::QueuedConnection);
    }

    void setAccess(host::AccessibilityAccess value) {
        accessValue = value;
        emit accessChanged();
    }

    void observe(host::ObservedField field) {
        lastObserved = field;
        field.refreshRequestId = std::exchange(refreshId, 0);
        emit fieldObserved(field);
    }
    void lose() { emit fieldLost(); }
};

} // namespace roclint::test
