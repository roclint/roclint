#include "IAccessibilityBridge.hpp"
#include "RangeInjectionPolicy.hpp"
#include "TerminalAdapter.hpp"

#include <QDebug>

#if defined(Q_OS_MAC)

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <Carbon/Carbon.h>

#include <atomic>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <vector>

namespace roclint::host {

namespace {

constexpr CFTimeInterval kTrustPollSeconds = 2.0;
constexpr CFTimeInterval kElectronWakeSeconds = 0.06;
constexpr CFIndex kMaxReadChars = 16000;
constexpr int kReadbackPolls = 40;
constexpr useconds_t kPollMicros = 30000;

bool isElectronBundle(const QString& bundleId) {
    const QString b = bundleId.toLower();
    return b == QLatin1String("com.todesktop.230313mzl4wgnlh") ||
           b == QLatin1String("com.todesktop.230313mzl4w4u92") ||
           b == QLatin1String("com.google.antigravity") ||
           b == QLatin1String("com.google.antigravity-ide") ||
           b == QLatin1String("com.microsoft.vscode") ||
           b == QLatin1String("com.microsoft.vscodeinsiders") ||
           b.contains(QLatin1String("codex")) ||
           b.contains(QLatin1String("claude")) ||
           b.contains(QLatin1String("antigravity")) ||
           b.contains(QLatin1String("todesktop")) ||
           b.contains(QLatin1String("cursor")) ||
           b.contains(QLatin1String("chrome")) ||
           b.contains(QLatin1String("brave")) ||
           b.contains(QLatin1String("edge")) ||
           b.contains(QLatin1String("arc")) ||
           b.contains(QLatin1String("safari"));
}

bool isTerminalBundle(const QString& bundleId) {
    const QString b = bundleId.toLower();
    return b == QLatin1String("com.apple.terminal") || b == QLatin1String("com.googlecode.iterm2") ||
           b == QLatin1String("com.mitchellh.ghostty") || b == QLatin1String("dev.warp.warp-stable");
}

std::optional<QString> readStringAttribute(AXUIElementRef element, CFStringRef attribute) {
    CFTypeRef value = nullptr;
    if (AXUIElementCopyAttributeValue(element, attribute, &value) != kAXErrorSuccess || value == nullptr) {
        return std::nullopt;
    }
    std::optional<QString> out;
    if (CFGetTypeID(value) == CFStringGetTypeID()) {
        out = QString::fromCFString(static_cast<CFStringRef>(value));
    }
    CFRelease(value);
    return out;
}

QString copyStringAttribute(AXUIElementRef element, CFStringRef attribute) {
    return readStringAttribute(element, attribute).value_or(QString{});
}

bool postKey(CGKeyCode key, CGEventFlags flags, pid_t targetPid) {
    CGEventRef down = CGEventCreateKeyboardEvent(nullptr, key, true);
    CGEventRef up = CGEventCreateKeyboardEvent(nullptr, key, false);
    const bool ready = targetPid > 0 && down && up;
    if (ready) {
        CGEventSetFlags(down, flags);
        CGEventSetFlags(up, flags);
        CGEventPostToPid(targetPid, down);
        CGEventPostToPid(targetPid, up);
    }
    if (down != nullptr) CFRelease(down);
    if (up != nullptr) CFRelease(up);
    return ready;
}

std::optional<CFRange> readSelection(AXUIElementRef element) {
    CFTypeRef value = nullptr;
    if (AXUIElementCopyAttributeValue(element, kAXSelectedTextRangeAttribute, &value) != kAXErrorSuccess || !value)
        return std::nullopt;
    CFRange range{};
    const bool valid = CFGetTypeID(value) == AXValueGetTypeID() &&
        AXValueGetValue(static_cast<AXValueRef>(value), static_cast<AXValueType>(kAXValueCFRangeType), &range) &&
        range.location >= 0 && range.length >= 0;
    CFRelease(value);
    return valid ? std::optional{range} : std::nullopt;
}

bool canSet(AXUIElementRef element, CFStringRef attribute) {
    Boolean settable = false;
    return AXUIElementIsAttributeSettable(element, attribute, &settable) == kAXErrorSuccess && settable;
}

bool selectRange(AXUIElementRef element, CFRange range) {
    AXValueRef value = AXValueCreate(static_cast<AXValueType>(kAXValueCFRangeType), &range);
    if (!value) return false;
    const bool selected = AXUIElementSetAttributeValue(element, kAXSelectedTextRangeAttribute, value) == kAXErrorSuccess;
    CFRelease(value);
    return selected;
}

pid_t frontmostPid() {
    @autoreleasepool {
        return [[NSWorkspace sharedWorkspace] frontmostApplication].processIdentifier;
    }
}

class MacAccessibilityBridge;
void axCallback(AXObserverRef, AXUIElementRef, CFStringRef notification, void* refcon);

class MacAccessibilityBridge final : public IAccessibilityBridge {
public:
    MacAccessibilityBridge() {
        m_trusted = AXIsProcessTrusted();
        m_thread = std::thread([this]() { run(); });
        std::unique_lock<std::mutex> lock(m_mutex);
        m_ready.wait(lock, [this]() { return m_loop != nullptr; });

        @autoreleasepool {
            id token = [[[NSWorkspace sharedWorkspace] notificationCenter]
                addObserverForName:NSWorkspaceDidActivateApplicationNotification
                            object:nil
                             queue:nil
                        usingBlock:^(NSNotification* note) {
                          NSRunningApplication* app = note.userInfo[NSWorkspaceApplicationKey];
                          const pid_t pid = app ? app.processIdentifier : 0;
                          this->perform(^{ this->onAppActivated(pid); });
                        }];
            m_activationToken = [token retain];
        }
    }

    ~MacAccessibilityBridge() override {
        @autoreleasepool {
            if (m_activationToken != nil) {
                [[[NSWorkspace sharedWorkspace] notificationCenter] removeObserver:m_activationToken];
                [m_activationToken release];
                m_activationToken = nil;
            }
        }
        perform(^{
          this->applyObserving(false);
          this->clearKnownFields();
          CFRunLoopStop(CFRunLoopGetCurrent());
        });
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    [[nodiscard]] AccessibilityAccess access() const override {
        return m_trusted.load() ? AccessibilityAccess::Granted : AccessibilityAccess::NotGranted;
    }

    bool requestAccess() override {
        @autoreleasepool {
            NSDictionary* options = @{(__bridge id)kAXTrustedCheckOptionPrompt: @YES};
            m_trusted = AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)options);
        }
        emit accessChanged();
        return true;
    }

    void setObserving(bool observing) override {
        perform(^{ this->applyObserving(observing); });
    }

    void inject(const InjectionRequest& request) override {
        const InjectionRequest copy = request;
        perform(^{ this->performInjection(copy); });
    }

    void injectRange(const RangeInjectionRequest& request) override {
        const RangeInjectionRequest copy = request;
        perform(^{ this->performRangeInjection(copy); });
    }

    void refreshFocusedField(quint64 requestId = 0) override {
        perform(^{
            if (!m_observing || m_loop == nullptr) return;
            @autoreleasepool {
                NSRunningApplication* front = [[NSWorkspace sharedWorkspace] frontmostApplication];
                const pid_t frontPid = front ? front.processIdentifier : 0;
                if (frontPid == getpid()) return; // Don't switch target app or drop focus if Roclint itself is frontmost
                if (frontPid > 0 && frontPid != m_pid) {
                    this->onAppActivated(frontPid);
                }
            }
            if (m_app == nullptr) return;
            CFTypeRef value = nullptr;
            if (AXUIElementCopyAttributeValue(m_app, kAXFocusedUIElementAttribute, &value) == kAXErrorSuccess && value != nullptr) {
                AXUIElementRef element = static_cast<AXUIElementRef>(value);
                if (m_focused == nullptr || !CFEqual(m_focused, element)) {
                    detachFocused();
                    m_focused = element;
                    m_token = tokenFor(element);
                    const QString role = copyStringAttribute(element, kAXRoleAttribute);
                    m_eligible = role == QLatin1String("AXTextArea") || role == QLatin1String("AXTextField") ||
                                 role == QLatin1String("AXComboBox") || isTerminalBundle(m_bundleId);
                    if (m_eligible) {
                        AXObserverAddNotification(m_observer, element, kAXValueChangedNotification, this);
                        AXObserverAddNotification(m_observer, element, kAXSelectedTextChangedNotification, this);
                    }
                } else {
                    CFRelease(element);
                }
            }
            if (m_focused != nullptr && m_eligible) {
                auto field = readField();
                if (field.has_value()) {
                    field->refreshRequestId = requestId;
                    m_lastSignature = field->text + QChar(0x1F) + QString::number(field->caretOffset) + QChar(0x1F) +
                                      QString::number(field->fieldToken);
                    emit fieldObserved(*field);
                }
            }
        });
    }

    void onNotification(CFStringRef notification) {
        @autoreleasepool {
            const pid_t frontPid = [[NSWorkspace sharedWorkspace] frontmostApplication].processIdentifier;
            // A visible non-activating HUD does not own the editor's focus. Only
            // read notifications for the actual foreground source application.
            if (frontPid <= 0 || frontPid == getpid() || frontPid != m_pid) return;
        }
        if (CFEqual(notification, kAXFocusedUIElementChangedNotification) ||
            CFEqual(notification, kAXFocusedWindowChangedNotification) ||
            CFEqual(notification, kAXMainWindowChangedNotification)) {
            onFocusChanged();
        } else {
            readAndEmit();
        }
    }

private:
    struct KnownField { AXUIElementRef element; pid_t pid; QString bundle; quint64 token; };

    void run() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_loop = CFRunLoopGetCurrent();
            CFRetain(m_loop);
        }
        m_ready.notify_all();

        // Trust can be granted in System Settings at any time; poll so the UI notices without a restart.
        CFRunLoopTimerRef trustTimer = CFRunLoopTimerCreateWithHandler(
            nullptr, CFAbsoluteTimeGetCurrent() + kTrustPollSeconds, kTrustPollSeconds, 0, 0, ^(CFRunLoopTimerRef) {
              const bool now = AXIsProcessTrusted();
              if (now != m_trusted.exchange(now)) {
                  emit accessChanged();
              }
            });
        CFRunLoopAddTimer(m_loop, trustTimer, kCFRunLoopDefaultMode);
        CFRunLoopRun();
        CFRunLoopTimerInvalidate(trustTimer);
        CFRelease(trustTimer);
        CFRelease(m_loop);
    }

    void perform(void (^block)(void)) {
        CFRunLoopPerformBlock(m_loop, kCFRunLoopDefaultMode, block);
        CFRunLoopWakeUp(m_loop);
    }

    void applyObserving(bool observing) {
        m_observing = observing && m_trusted.load();
        if (qEnvironmentVariableIsSet("ROCLINT_CAPTURE_DIAGNOSTICS")) {
            qInfo() << "[MacAX] applyObserving: observing=" << observing << "trusted=" << m_trusted.load();
        }
        if (!m_observing) {
            stopHeartbeat();
            detachApp();
            emit fieldLost();
            return;
        }
        startHeartbeat();
        @autoreleasepool {
            NSRunningApplication* front = [[NSWorkspace sharedWorkspace] frontmostApplication];
            onAppActivated(front ? front.processIdentifier : 0);
        }
    }

    void stopHeartbeat() {
        if (m_pollTimer != nullptr) {
            CFRunLoopTimerInvalidate(m_pollTimer);
            CFRelease(m_pollTimer);
            m_pollTimer = nullptr;
        }
    }

    void startHeartbeat() {
        stopHeartbeat();
        if (!m_observing || m_loop == nullptr) {
            return;
        }
        m_pollTimer = CFRunLoopTimerCreateWithHandler(
            nullptr, CFAbsoluteTimeGetCurrent() + 0.1, 0.1, 0, 0,
            ^(CFRunLoopTimerRef) {
                this->onHeartbeat();
            });
        CFRunLoopAddTimer(m_loop, m_pollTimer, kCFRunLoopDefaultMode);
    }

    void onHeartbeat() {
        if (!m_observing || m_loop == nullptr) {
            return;
        }

        // 1. Detect if the frontmost active application changed
        @autoreleasepool {
            NSRunningApplication* front = [[NSWorkspace sharedWorkspace] frontmostApplication];
            const pid_t frontPid = front ? front.processIdentifier : 0;
            if (frontPid <= 0 || frontPid == getpid()) {
                // Preserve the source target while Roclint itself is foreground.
                return;
            }
            bool roclintActive = [NSApp keyWindow] != nil || [NSApp isActive];
            if (roclintActive) {
                return;
            }
            if (frontPid > 0 && frontPid != m_pid) {
                this->onAppActivated(frontPid);
                return;
            }
        }

        if (m_app == nullptr) {
            return;
        }

        // 2. Detect if the focused element changed (e.g. user clicked into another window/project or field)
        CFTypeRef value = nullptr;
        if (AXUIElementCopyAttributeValue(m_app, kAXFocusedUIElementAttribute, &value) == kAXErrorSuccess && value != nullptr) {
            AXUIElementRef current = static_cast<AXUIElementRef>(value);
            if (m_focused == nullptr || !CFEqual(m_focused, current)) {
                CFRelease(value);
                m_consecutiveFocusMisses = 0;
                this->onFocusChanged();
                return;
            }
            CFRelease(value);
            m_consecutiveFocusMisses = 0;
        } else if (m_focused != nullptr) {
            bool roclintActive = false;
            @autoreleasepool {
                roclintActive = [NSApp keyWindow] != nil || [NSApp isActive];
            }
            if (roclintActive) {
                m_consecutiveFocusMisses = 0;
                return;
            }
            if (++m_consecutiveFocusMisses >= 5) {
                detachFocused();
                emit fieldLost();
                m_consecutiveFocusMisses = 0;
            }
            return;
        }

        // 3. Read and emit latest draft/caret/geometry changes
        if (m_focused != nullptr && m_eligible) {
            readAndEmit();
        }
    }

    quint64 tokenFor(AXUIElementRef element) {
        for (const auto& field : m_knownFields)
            if (field.pid==m_pid && field.bundle==m_bundleId && CFEqual(field.element,element)) return field.token;
        // Retain only exact native identities. Equal text, labels or geometry never rebind a field.
        if (m_knownFields.size()==32) { CFRelease(m_knownFields.front().element); m_knownFields.erase(m_knownFields.begin()); }
        const auto token=++m_nextFieldToken;
        CFRetain(element); m_knownFields.push_back({element,m_pid,m_bundleId,token});
        return token;
    }
    void clearKnownFields() {
        for (const auto& field : m_knownFields) CFRelease(field.element);
        m_knownFields.clear();
    }

    void detachApp() {
        detachFocused();
        if (m_observer != nullptr) {
            if (m_app != nullptr) {
                AXObserverRemoveNotification(m_observer, m_app, kAXFocusedUIElementChangedNotification);
                AXObserverRemoveNotification(m_observer, m_app, kAXFocusedWindowChangedNotification);
                AXObserverRemoveNotification(m_observer, m_app, kAXMainWindowChangedNotification);
                AXObserverRemoveNotification(m_observer, m_app, kAXValueChangedNotification);
                AXObserverRemoveNotification(m_observer, m_app, kAXSelectedTextChangedNotification);
            }
            CFRunLoopRemoveSource(m_loop, AXObserverGetRunLoopSource(m_observer), kCFRunLoopDefaultMode);
            CFRelease(m_observer);
            m_observer = nullptr;
        }
        if (m_app != nullptr) {
            CFRelease(m_app);
            m_app = nullptr;
        }
        m_pid = 0;
    }

    void detachFocused() {
        if (m_focused != nullptr) {
            if (m_observer != nullptr) {
                AXObserverRemoveNotification(m_observer, m_focused, kAXValueChangedNotification);
                AXObserverRemoveNotification(m_observer, m_focused, kAXSelectedTextChangedNotification);
            }
            CFRelease(m_focused);
            m_focused = nullptr;
        }
        m_consecutiveFocusMisses = 0;
        m_lastSignature.clear();
    }

    void onAppActivated(pid_t pid) {
        if (!m_observing || pid <= 0 || pid == getpid() || pid == m_pid) {
            return;
        }
        detachApp();
        m_pid = pid;
        @autoreleasepool {
            NSRunningApplication* app = [NSRunningApplication runningApplicationWithProcessIdentifier:pid];
            m_bundleId = app.bundleIdentifier ? QString::fromNSString(app.bundleIdentifier) : QString();
            m_displayName = app.localizedName ? QString::fromNSString(app.localizedName) : m_bundleId;
        }
        if (qEnvironmentVariableIsSet("ROCLINT_CAPTURE_DIAGNOSTICS")) {
            qInfo() << "[MacAX] Target app:" << m_displayName << "bundleId=" << m_bundleId << "isElectron=" << isElectronBundle(m_bundleId);
        }
        m_app = AXUIElementCreateApplication(pid);
        if (m_app == nullptr) {
            return;
        }
        if (isElectronBundle(m_bundleId)) {
            // Chromium builds its accessibility tree only when asked.
            AXUIElementSetAttributeValue(m_app, CFSTR("AXManualAccessibility"), kCFBooleanTrue);
        }
        if (AXObserverCreate(pid, axCallback, &m_observer) != kAXErrorSuccess || m_observer == nullptr) {
            m_observer = nullptr;
            return;
        }
        AXObserverAddNotification(m_observer, m_app, kAXFocusedUIElementChangedNotification, this);
        AXObserverAddNotification(m_observer, m_app, kAXFocusedWindowChangedNotification, this);
        AXObserverAddNotification(m_observer, m_app, kAXMainWindowChangedNotification, this);
        AXObserverAddNotification(m_observer, m_app, kAXValueChangedNotification, this);
        AXObserverAddNotification(m_observer, m_app, kAXSelectedTextChangedNotification, this);
        CFRunLoopAddSource(m_loop, AXObserverGetRunLoopSource(m_observer), kCFRunLoopDefaultMode);
        onFocusChanged();
        if (isElectronBundle(m_bundleId)) {
            CFRunLoopTimerRef retry = CFRunLoopTimerCreateWithHandler(
                nullptr, CFAbsoluteTimeGetCurrent() + kElectronWakeSeconds, 0, 0, 0,
                ^(CFRunLoopTimerRef) { this->onFocusChanged(); });
            CFRunLoopAddTimer(m_loop, retry, kCFRunLoopDefaultMode);
            CFRelease(retry);
        }
    }

    void onFocusChanged() {
        if (m_app == nullptr) {
            return;
        }
        bool roclintActive = false;
        @autoreleasepool {
            roclintActive = [NSApp keyWindow] != nil || [NSApp isActive];
        }
        if (roclintActive) {
            return;
        }
        CFTypeRef value = nullptr;
        if (AXUIElementCopyAttributeValue(m_app, kAXFocusedUIElementAttribute, &value) != kAXErrorSuccess ||
            value == nullptr) {
            if (qEnvironmentVariableIsSet("ROCLINT_CAPTURE_DIAGNOSTICS")) {
                qInfo() << "[MacAX] onFocusChanged: no focused UI element";
            }
            if (++m_consecutiveFocusMisses >= 5) {
                detachFocused();
                emit fieldLost();
                m_consecutiveFocusMisses = 0;
            }
            return;
        }
        m_consecutiveFocusMisses = 0;
        AXUIElementRef element = static_cast<AXUIElementRef>(value);
        if (m_focused != nullptr && CFEqual(m_focused, element)) {
            CFRelease(element);
            return;
        }
        detachFocused();
        m_focused = element;
        m_token = tokenFor(element);
        const QString role = copyStringAttribute(element, kAXRoleAttribute);
        m_eligible = role == QLatin1String("AXTextArea") || role == QLatin1String("AXTextField") ||
                     role == QLatin1String("AXComboBox") || isTerminalBundle(m_bundleId);
        if (qEnvironmentVariableIsSet("ROCLINT_CAPTURE_DIAGNOSTICS")) {
            qInfo() << "[MacAX] onFocusChanged: role=" << role << "eligible=" << m_eligible;
        }
        if (!m_eligible) {
            emit fieldLost();
            return;
        }
        AXObserverAddNotification(m_observer, element, kAXValueChangedNotification, this);
        AXObserverAddNotification(m_observer, element, kAXSelectedTextChangedNotification, this);
        readAndEmit();
    }

    std::optional<ObservedField> readField() {
        if (m_focused == nullptr || !m_eligible) {
            return std::nullopt;
        }
        ObservedField f;
        f.fieldToken = m_token;
        f.appIdentifier = m_bundleId;
        if (m_displayName.isEmpty() && m_pid > 0) {
            @autoreleasepool {
                NSRunningApplication* app = [NSRunningApplication runningApplicationWithProcessIdentifier:m_pid];
                if (app != nil) {
                    m_displayName = app.localizedName ? QString::fromNSString(app.localizedName) : m_bundleId;
                }
            }
        }
        f.appDisplayName = m_displayName;
        f.processId = m_pid;
        f.kind = isTerminalBundle(m_bundleId) ? FieldKind::Terminal : FieldKind::GuiEditor;
        f.isPassword = copyStringAttribute(m_focused, kAXSubroleAttribute) == QLatin1String("AXSecureTextField");
        if (f.isPassword) {
            return f;
        }
        f.text = copyStringAttribute(m_focused, kAXValueAttribute).left(kMaxReadChars);

        CFTypeRef rangeValue = nullptr;
        if (AXUIElementCopyAttributeValue(m_focused, kAXSelectedTextRangeAttribute, &rangeValue) == kAXErrorSuccess &&
            rangeValue != nullptr) {
            CFRange range{};
            if (AXValueGetValue(static_cast<AXValueRef>(rangeValue), static_cast<AXValueType>(kAXValueCFRangeType), &range)) {
                f.caretOffset = static_cast<int>(range.location);
                CFRange caret{range.location, 0};
                AXValueRef caretValue = AXValueCreate(static_cast<AXValueType>(kAXValueCFRangeType), &caret);
                CFTypeRef boundsValue = nullptr;
                if (caretValue != nullptr &&
                    AXUIElementCopyParameterizedAttributeValue(m_focused, kAXBoundsForRangeParameterizedAttribute,
                                                               caretValue, &boundsValue) == kAXErrorSuccess &&
                    boundsValue != nullptr) {
                    CGRect rect{};
                    if (AXValueGetValue(static_cast<AXValueRef>(boundsValue), static_cast<AXValueType>(kAXValueCGRectType), &rect)) {
                        f.caretRect = QRect(static_cast<int>(rect.origin.x), static_cast<int>(rect.origin.y),
                                            std::max(1, static_cast<int>(rect.size.width)),
                                            std::max(1, static_cast<int>(rect.size.height)));
                    }
                    CFRelease(boundsValue);
                }
                if (caretValue != nullptr) CFRelease(caretValue);
            }
            CFRelease(rangeValue);
        }

        CFTypeRef posValue = nullptr;
        CFTypeRef sizeValue = nullptr;
        CGPoint pt{};
        CGSize sz{};
        bool hasPos = false;
        bool hasSize = false;
        if (AXUIElementCopyAttributeValue(m_focused, kAXPositionAttribute, &posValue) == kAXErrorSuccess && posValue != nullptr) {
            hasPos = AXValueGetValue(static_cast<AXValueRef>(posValue), static_cast<AXValueType>(kAXValueCGPointType), &pt);
            CFRelease(posValue);
        }
        if (AXUIElementCopyAttributeValue(m_focused, kAXSizeAttribute, &sizeValue) == kAXErrorSuccess && sizeValue != nullptr) {
            hasSize = AXValueGetValue(static_cast<AXValueRef>(sizeValue), static_cast<AXValueType>(kAXValueCGSizeType), &sz);
            CFRelease(sizeValue);
        }
        if (hasPos && hasSize && sz.width > 0 && sz.height > 0) {
            f.fieldRect = QRect(static_cast<int>(pt.x), static_cast<int>(pt.y),
                                static_cast<int>(sz.width), static_cast<int>(sz.height));
        }

        if (f.kind == FieldKind::Terminal) {
            const auto rawCaret = f.caretOffset;
            const auto offset = rawCaret < 0 ? f.text.size() : std::min<qsizetype>(rawCaret, f.text.size());
            auto prefix = f.text.left(offset);
            prefix.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
            f.text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
            f.text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
            if (rawCaret >= 0) f.caretOffset = static_cast<int>(prefix.size());
        }
        if (f.caretOffset > f.text.size()) {
            f.caretOffset = static_cast<int>(f.text.size());
        }
        return f;
    }

    void readAndEmit() {
        auto field = readField();
        if (!field.has_value()) {
            return;
        }
        const QString signature = field->text + QChar(0x1F) + QString::number(field->caretOffset) + QChar(0x1F) +
                                  QString::number(field->fieldToken) + QChar(0x1F) +
                                  QString::number(field->fieldRect.x()) + QChar(0x1F) +
                                  QString::number(field->fieldRect.y()) + QChar(0x1F) +
                                  QString::number(field->fieldRect.width()) + QChar(0x1F) +
                                  QString::number(field->fieldRect.height());
        if (signature == m_lastSignature) {
            return;
        }
        m_lastSignature = signature;
        if (qEnvironmentVariableIsSet("ROCLINT_CAPTURE_DIAGNOSTICS")) {
            qInfo() << "[MacAX] readAndEmit: characters=" << field->text.size() << "fieldRect=" << field->fieldRect;
        }
        emit fieldObserved(*field);
    }

    const KnownField* capturedField(quint64 token, FieldKind kind) const {
        for (const auto& field : m_knownFields) {
            if (field.token == token &&
                isTerminalBundle(field.bundle) == (kind == FieldKind::Terminal)) return &field;
        }
        return nullptr;
    }

    static bool targetFocused(AXUIElementRef app, const KnownField& target) {
        if (frontmostPid() != target.pid) return false;
        CFTypeRef focused = nullptr;
        if (AXUIElementCopyAttributeValue(app, kAXFocusedUIElementAttribute, &focused) != kAXErrorSuccess || !focused)
            return false;
        const bool matches = CFEqual(focused, target.element);
        CFRelease(focused);
        return matches;
    }

    static bool activateCapturedApp(AXUIElementRef app, const KnownField& target) {
        const auto front = frontmostPid();
        if (front == target.pid) return targetFocused(app, target);
        if (front != getpid()) return false;

        @autoreleasepool {
            NSRunningApplication* application = [NSRunningApplication runningApplicationWithProcessIdentifier:target.pid];
            if (!application || !application.bundleIdentifier ||
                QString::fromNSString(application.bundleIdentifier) != target.bundle) return false;
            if (@available(macOS 14.0, *)) {
                [NSApp yieldActivationToApplication:application];
                [application activateFromApplication:[NSRunningApplication currentApplication] options:0];
            } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
                [application activateWithOptions:NSApplicationActivateIgnoringOtherApps];
#pragma clang diagnostic pop
            }
        }
        // The app must return to the captured element on its own. Forcing focus
        // to a previously captured field could write into a stale draft.
        for (int i = 0; i < 18; ++i) {
            const auto active = frontmostPid();
            if (active != getpid() && active != target.pid) return false;
            if (targetFocused(app, target)) return true;
            usleep(kPollMicros);
        }
        return false;
    }

    static std::optional<QString> deliveryText(const KnownField& target, FieldKind kind) {
        auto text = readStringAttribute(target.element, kAXValueAttribute);
        if (!text) return std::nullopt;
        if (kind == FieldKind::GuiEditor) return text;
        const auto selection = readSelection(target.element);
        const auto caret = selection ? std::min(selection->location, static_cast<CFIndex>(text->size())) : text->size();
        auto prefix = text->left(caret);
        prefix.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        prefix.replace(QLatin1Char('\r'), QLatin1Char('\n'));
        const auto draft = TerminalAdapter::extractDraft(prefix, prefix.size());
        return draft ? std::optional{draft->text} : std::nullopt;
    }

    static bool exactText(AXUIElementRef app, const KnownField& target, FieldKind kind, const QString& expected) {
        if (!targetFocused(app, target)) return false;
        const auto current = deliveryText(target, kind);
        return current && RangeInjectionPolicy::readbackMatches(expected, *current);
    }

    static bool waitForText(AXUIElementRef app, const KnownField& target, FieldKind kind,
                            const QString& expected, int polls = kReadbackPolls) {
        for (int i = 0; i < polls; ++i) {
            if (!targetFocused(app, target)) return false;
            const auto text = deliveryText(target, kind);
            if (text && RangeInjectionPolicy::readbackMatches(expected, *text)) return true;
            usleep(kPollMicros);
        }
        return false;
    }

    static bool selectionMatches(AXUIElementRef element, CFRange expected) {
        const auto current = readSelection(element);
        return current && current->location == expected.location && current->length == expected.length;
    }

    static std::optional<InjectionOutcome> selectionError(AXUIElementRef app, const KnownField& target,
                                                          const QString& before, CFRange range) {
        // Chromium applies an AX selection asynchronously in the renderer.
        for (int i = 0; i < 6; ++i) {
            if (!exactText(app, target, FieldKind::GuiEditor, before)) return InjectionOutcome::FieldChanged;
            if (selectionMatches(target.element, range)) return std::nullopt;
            usleep(kPollMicros);
        }
        return InjectionOutcome::NotSupported;
    }

    static InjectionOutcome pasteAndVerify(AXUIElementRef app, const KnownField& target, FieldKind kind,
                                           const QString& before, const QString& payload, const QString& expected,
                                           std::optional<CFRange> selection = std::nullopt) {
        @autoreleasepool {
            NSPasteboard* board = [NSPasteboard generalPasteboard];
            const NSInteger originalChange = board.changeCount;
            NSMutableArray<NSPasteboardItem*>* saved = [NSMutableArray array];
            for (NSPasteboardItem* item in board.pasteboardItems) {
                NSPasteboardItem* copy = [[[NSPasteboardItem alloc] init] autorelease];
                for (NSPasteboardType type in item.types) {
                    NSData* data = [item dataForType:type];
                    if (!data || ![copy setData:data forType:type]) return InjectionOutcome::NotSupported;
                }
                [saved addObject:copy];
            }
            const auto validTarget = [&]() {
                return exactText(app, target, kind, before) &&
                    (!selection || selectionMatches(target.element, *selection));
            };
            if (!validTarget() || board.changeCount != originalChange) return InjectionOutcome::FieldChanged;
            NSPasteboardItem* temporary = [[[NSPasteboardItem alloc] init] autorelease];
            if (![temporary setString:payload.toNSString() forType:NSPasteboardTypeString])
                return InjectionOutcome::Failed;
            const NSInteger clearedChange = [board clearContents];
            if (board.changeCount != clearedChange) return InjectionOutcome::FieldChanged;
            const bool copied = [board writeObjects:@[temporary]];
            // writeObjects does not claim ownership after clearContents. A
            // different change count belongs to someone else.
            const bool owned = board.changeCount == clearedChange;
            InjectionOutcome outcome = InjectionOutcome::Failed;
            if (owned && copied) {
                if (!validTarget() || board.changeCount != clearedChange) {
                    outcome = InjectionOutcome::FieldChanged;
                } else if (postKey(kVK_ANSI_V, kCGEventFlagMaskCommand, target.pid) &&
                           waitForText(app, target, kind, expected)) {
                    outcome = InjectionOutcome::Injected;
                }
            }
            if (board.changeCount == clearedChange) {
                const NSInteger restoreChange = [board clearContents];
                if (saved.count && board.changeCount == restoreChange) [board writeObjects:saved];
            }
            return outcome;
        }
    }

    static InjectionOutcome replaceGuiValue(AXUIElementRef app, const KnownField& target,
                                            const QString& before, const QString& expected) {
        if (!exactText(app, target, FieldKind::GuiEditor, before)) return InjectionOutcome::FieldChanged;
        if (!isElectronBundle(target.bundle) && canSet(target.element, kAXValueAttribute)) {
            if (!exactText(app, target, FieldKind::GuiEditor, before)) return InjectionOutcome::FieldChanged;
            AXUIElementSetAttributeValue(target.element, kAXValueAttribute, (__bridge CFTypeRef)expected.toNSString());
            return waitForText(app, target, FieldKind::GuiEditor, expected)
                ? InjectionOutcome::Injected : InjectionOutcome::Failed;
        }

        const CFRange whole{0, static_cast<CFIndex>(before.size())};
        // Select-all is safe only when this provider can show us what it selected.
        if (!readSelection(target.element)) return InjectionOutcome::NotSupported;
        if (!exactText(app, target, FieldKind::GuiEditor, before)) return InjectionOutcome::FieldChanged;
        if (!selectRange(target.element, whole) && !postKey(kVK_ANSI_A, kCGEventFlagMaskCommand, target.pid))
            return InjectionOutcome::Failed;
        if (const auto error = selectionError(app, target, before, whole)) return *error;
        return pasteAndVerify(app, target, FieldKind::GuiEditor, before, expected, expected, whole);
    }

    static InjectionOutcome replaceTerminalDraft(AXUIElementRef app, const KnownField& target, const InjectionRequest& r) {
        for (const auto key : TerminalAdapter::replacementKeys(r.terminalVimMode)) {
            if (!targetFocused(app, target)) return InjectionOutcome::FieldChanged;
            bool posted = false;
            switch (key) {
                case TerminalAdapter::Key::Escape: posted = postKey(kVK_Escape, 0, target.pid); break;
                case TerminalAdapter::Key::AppendAtLineEnd: posted = postKey(kVK_ANSI_A, kCGEventFlagMaskShift, target.pid); break;
                case TerminalAdapter::Key::LineEnd: posted = postKey(kVK_ANSI_E, kCGEventFlagMaskControl, target.pid); break;
                case TerminalAdapter::Key::ClearLine: posted = postKey(kVK_ANSI_U, kCGEventFlagMaskControl, target.pid); break;
                case TerminalAdapter::Key::Paste:
                    if (!waitForText(app, target, FieldKind::Terminal, QString{}, 10)) return InjectionOutcome::Failed;
                    return pasteAndVerify(app, target, FieldKind::Terminal, QString{}, r.text, r.text);
            }
            if (!posted) return InjectionOutcome::Failed;
            usleep(kPollMicros);
        }
        return InjectionOutcome::Failed;
    }

    template<class Write>
    InjectionOutcome withCapturedField(quint64 token, FieldKind kind, const QString& before, Write write) {
        if (!m_observing || !AXIsProcessTrusted()) return InjectionOutcome::NotSupported;
        const auto* target = capturedField(token, kind);
        if (!target) return InjectionOutcome::FieldChanged;
        const auto subrole = copyStringAttribute(target->element, kAXSubroleAttribute);
        if (subrole == QLatin1String("AXSecureTextField")) return InjectionOutcome::NotSupported;
        const auto current = deliveryText(*target, kind);
        if (!current) return InjectionOutcome::NotSupported;
        if (!RangeInjectionPolicy::preconditionsHold(token, target->token, before, *current))
            return InjectionOutcome::FieldChanged;

        const std::unique_ptr<std::remove_pointer_t<AXUIElementRef>, decltype(&CFRelease)>
            app(AXUIElementCreateApplication(target->pid), CFRelease);
        if (!app || !activateCapturedApp(app.get(), *target) || !exactText(app.get(), *target, kind, before))
            return InjectionOutcome::FieldChanged;
        return write(app.get(), *target);
    }

    static QString deliveryDetail(InjectionOutcome outcome) {
        switch (outcome) {
            case InjectionOutcome::Injected: return {};
            case InjectionOutcome::FieldChanged: return QStringLiteral("The captured target or its text changed.");
            case InjectionOutcome::NotSupported: return QStringLiteral("This field does not support verified replacement.");
            case InjectionOutcome::Failed: return QStringLiteral("The editor did not confirm the exact replacement. Check the field before retrying.");
        }
        return {};
    }

    void performInjection(const InjectionRequest& r) {
        @autoreleasepool {
            const auto outcome = withCapturedField(r.fieldToken, r.kind, r.expectedCurrentText,
                [&](AXUIElementRef app, const KnownField& target) {
                    if ((r.processId > 0 && r.processId != target.pid) ||
                        (!r.appIdentifier.isEmpty() && r.appIdentifier != target.bundle))
                        return InjectionOutcome::FieldChanged;
                    if (r.kind == FieldKind::Terminal) return replaceTerminalDraft(app, target, r);
                    return replaceGuiValue(app, target, r.expectedCurrentText, r.text);
                });
            emit injectionFinished(r.requestId, outcome, deliveryDetail(outcome));
        }
    }

    void performRangeInjection(const RangeInjectionRequest& r) {
        @autoreleasepool {
            if (r.kind != FieldKind::GuiEditor) {
                emit rangeInjectionFinished(r.requestId, InjectionOutcome::NotSupported,
                    QStringLiteral("Inline replacement is unavailable in terminal buffers."));
                return;
            }
            if (!RangeInjectionPolicy::rangeMatches(r.expectedCurrentText, r.startOffset, r.endOffset, r.originalRangeText) ||
                r.replacementText.isEmpty()) {
                emit rangeInjectionFinished(r.requestId, InjectionOutcome::FieldChanged,
                    QStringLiteral("The captured text range is invalid."));
                return;
            }
            QString expected = r.expectedCurrentText;
            expected.replace(r.startOffset, r.endOffset - r.startOffset, r.replacementText);
            const auto outcome = withCapturedField(r.fieldToken, r.kind, r.expectedCurrentText,
                [&](AXUIElementRef app, const KnownField& target) {
                    const CFRange range{r.startOffset, r.endOffset - r.startOffset};
                    if (!selectRange(target.element, range)) {
                        // Selection alone doesn't write text. The whole-field
                        // route checks the original value and focus again.
                        return replaceGuiValue(app, target, r.expectedCurrentText, expected);
                    }
                    if (const auto error = selectionError(app, target, r.expectedCurrentText, range)) return *error;
                    // Chromium exposes a settable AXSelectedText and can return
                    // success from its setter without editing the actual field.
                    if (!isElectronBundle(target.bundle) && canSet(target.element, kAXSelectedTextAttribute)) {
                        if (!exactText(app, target, r.kind, r.expectedCurrentText) ||
                            !selectionMatches(target.element, range)) return InjectionOutcome::FieldChanged;
                        AXUIElementSetAttributeValue(target.element, kAXSelectedTextAttribute,
                            (__bridge CFTypeRef)r.replacementText.toNSString());
                        return waitForText(app, target, r.kind, expected)
                            ? InjectionOutcome::Injected : InjectionOutcome::Failed;
                    }
                    return pasteAndVerify(app, target, r.kind, r.expectedCurrentText, r.replacementText, expected, range);
                });
            emit rangeInjectionFinished(r.requestId, outcome, deliveryDetail(outcome));
        }
    }

    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_ready;
    CFRunLoopRef m_loop{nullptr};
    std::atomic<bool> m_trusted{false};
    id m_activationToken{nil};

    // Run-loop thread state.
    bool m_observing{false};
    pid_t m_pid{0};
    QString m_bundleId;
    QString m_displayName;
    AXUIElementRef m_app{nullptr};
    AXObserverRef m_observer{nullptr};
    AXUIElementRef m_focused{nullptr};
    bool m_eligible{false};
    std::vector<KnownField> m_knownFields;
    quint64 m_token{0}, m_nextFieldToken{0};
    QString m_lastSignature;
    CFRunLoopTimerRef m_pollTimer{nullptr};
    int m_consecutiveFocusMisses{0};
};

void axCallback(AXObserverRef, AXUIElementRef, CFStringRef notification, void* refcon) {
    static_cast<MacAccessibilityBridge*>(refcon)->onNotification(notification);
}

} // namespace

std::unique_ptr<IAccessibilityBridge> createPlatformAccessibilityBridge() {
    return std::make_unique<MacAccessibilityBridge>();
}

} // namespace roclint::host

#endif
