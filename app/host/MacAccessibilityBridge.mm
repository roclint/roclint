#include "IAccessibilityBridge.hpp"
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

namespace roclint::host {

namespace {

constexpr CFTimeInterval kTrustPollSeconds = 2.0;
constexpr CFTimeInterval kElectronWakeSeconds = 0.06;
constexpr useconds_t kPasteSettleMicros = 150000;
constexpr useconds_t kTerminalEchoMicros = 300000;
constexpr CFIndex kMaxReadChars = 16000;

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
           b.contains(QLatin1String("cursor"));
}

bool isTerminalBundle(const QString& bundleId) {
    const QString b = bundleId.toLower();
    return b == QLatin1String("com.apple.terminal") || b == QLatin1String("com.googlecode.iterm2") ||
           b == QLatin1String("com.mitchellh.ghostty") || b == QLatin1String("dev.warp.warp-stable");
}

QString copyStringAttribute(AXUIElementRef element, CFStringRef attribute) {
    CFTypeRef value = nullptr;
    if (AXUIElementCopyAttributeValue(element, attribute, &value) != kAXErrorSuccess || value == nullptr) {
        return {};
    }
    QString out;
    if (CFGetTypeID(value) == CFStringGetTypeID()) {
        out = QString::fromCFString(static_cast<CFStringRef>(value));
    }
    CFRelease(value);
    return out;
}

void postKey(CGKeyCode key, CGEventFlags flags) {
    CGEventRef down = CGEventCreateKeyboardEvent(nullptr, key, true);
    CGEventRef up = CGEventCreateKeyboardEvent(nullptr, key, false);
    if (down != nullptr && up != nullptr) {
        CGEventSetFlags(down, flags);
        CGEventSetFlags(up, flags);
        CGEventPost(kCGHIDEventTap, down);
        CGEventPost(kCGHIDEventTap, up);
    }
    if (down != nullptr) CFRelease(down);
    if (up != nullptr) CFRelease(up);
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
                    ++m_token;
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
        if (CFEqual(notification, kAXFocusedUIElementChangedNotification) ||
            CFEqual(notification, kAXFocusedWindowChangedNotification) ||
            CFEqual(notification, kAXMainWindowChangedNotification)) {
            onFocusChanged();
        } else {
            readAndEmit();
        }
    }

private:
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
            if (frontPid == getpid()) {
                // User is interacting with Roclint (clicking the badge or reviewing the HUD).
                // Do NOT query m_app for focus or drop the target editor session!
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
                this->onFocusChanged();
                return;
            }
            CFRelease(value);
        } else if (m_focused != nullptr) {
            detachFocused();
            emit fieldLost();
            return;
        }

        // 3. Read and emit latest draft/caret/geometry changes
        if (m_focused != nullptr && m_eligible) {
            readAndEmit();
        }
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
        CFTypeRef value = nullptr;
        if (AXUIElementCopyAttributeValue(m_app, kAXFocusedUIElementAttribute, &value) != kAXErrorSuccess ||
            value == nullptr) {
            if (qEnvironmentVariableIsSet("ROCLINT_CAPTURE_DIAGNOSTICS")) {
                qInfo() << "[MacAX] onFocusChanged: no focused UI element";
            }
            detachFocused();
            emit fieldLost();
            return;
        }
        AXUIElementRef element = static_cast<AXUIElementRef>(value);
        if (m_focused != nullptr && CFEqual(m_focused, element)) {
            CFRelease(element);
            return;
        }
        detachFocused();
        m_focused = element;
        ++m_token;
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

        f.text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        f.text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
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
            qInfo() << "[MacAX] readAndEmit: text=" << field->text << "fieldRect=" << field->fieldRect;
        }
        emit fieldObserved(*field);
    }

    QString currentDraft(const ObservedField& field) const {
        if (field.kind == FieldKind::Terminal) {
            const auto draft = TerminalAdapter::extractDraft(field.text, field.caretOffset);
            return draft.has_value() ? draft->text : QString();
        }
        return field.text;
    }

    bool activateTarget() {
        @autoreleasepool {
            NSRunningApplication* app = [NSRunningApplication runningApplicationWithProcessIdentifier:m_pid];
            if (app == nil) {
                return false;
            }
            [app activateWithOptions:0];
        }
        usleep(80000);
        return true;
    }

    bool paste(const QString& text) {
        @autoreleasepool {
            NSPasteboard* board = [NSPasteboard generalPasteboard];
            NSString* saved = [[board stringForType:NSPasteboardTypeString] copy];
            [board clearContents];
            if (![board setString:text.toNSString() forType:NSPasteboardTypeString]) {
                [saved release];
                return false;
            }
            postKey(kVK_ANSI_V, kCGEventFlagMaskCommand);
            usleep(kPasteSettleMicros);
            if (saved != nil) {
                [board clearContents];
                [board setString:saved forType:NSPasteboardTypeString];
                [saved release];
            }
        }
        return true;
    }

    void performInjection(const InjectionRequest& r) {
        if (m_focused == nullptr || r.fieldToken != m_token) {
            emit injectionFinished(r.requestId, InjectionOutcome::FieldChanged, QStringLiteral("Focus moved to a different field."));
            return;
        }
        const auto before = readField();
        if (!before.has_value() || before->isPassword) {
            emit injectionFinished(r.requestId, InjectionOutcome::NotSupported, QStringLiteral("That field can't be written to."));
            return;
        }
        if (currentDraft(*before) != r.expectedCurrentText) {
            emit injectionFinished(r.requestId, InjectionOutcome::FieldChanged,
                                   QStringLiteral("The text changed after Roclint captured it."));
            return;
        }

        if (r.kind == FieldKind::Terminal) {
            if (!activateTarget()) {
                emit injectionFinished(r.requestId, InjectionOutcome::Failed, QStringLiteral("Couldn't bring the terminal to the front."));
                return;
            }
            bool pasted = true;
            for (const auto key : TerminalAdapter::replacementKeys(r.terminalVimMode)) {
                switch (key) {
                    case TerminalAdapter::Key::Escape: postKey(kVK_Escape, 0); break;
                    case TerminalAdapter::Key::AppendAtLineEnd: postKey(kVK_ANSI_A, kCGEventFlagMaskShift); break;
                    case TerminalAdapter::Key::LineEnd: postKey(kVK_ANSI_E, kCGEventFlagMaskControl); break;
                    case TerminalAdapter::Key::ClearLine: postKey(kVK_ANSI_U, kCGEventFlagMaskControl); break;
                    case TerminalAdapter::Key::Paste: pasted = paste(r.text); break;
                }
            }
            usleep(kTerminalEchoMicros);
            const auto after = readField();
            const bool ok = pasted && after.has_value() && currentDraft(*after) == r.text;
            emit injectionFinished(r.requestId, ok ? InjectionOutcome::Injected : InjectionOutcome::Failed,
                                   ok ? QString() : QStringLiteral("The terminal didn't show the new text."));
            return;
        }

        @autoreleasepool {
            if (AXUIElementSetAttributeValue(m_focused, kAXValueAttribute, (__bridge CFTypeRef)r.text.toNSString()) ==
                kAXErrorSuccess) {
                const auto after = readField();
                if (after.has_value() && after->text == r.text) {
                    emit injectionFinished(r.requestId, InjectionOutcome::Injected, QString());
                    return;
                }
            }
        }
        // Electron editors often ignore AXValue writes: select all and paste instead.
        if (!activateTarget()) {
            emit injectionFinished(r.requestId, InjectionOutcome::Failed, QStringLiteral("Couldn't bring the app to the front."));
            return;
        }
        postKey(kVK_ANSI_A, kCGEventFlagMaskCommand);
        usleep(25000);
        const bool pasted = paste(r.text);
        const auto after = readField();
        const bool ok = pasted && after.has_value() && after->text == r.text;
        emit injectionFinished(r.requestId, ok ? InjectionOutcome::Injected : InjectionOutcome::Failed,
                               ok ? QString() : QStringLiteral("The field didn't accept the new text."));
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
    quint64 m_token{0};
    QString m_lastSignature;
    CFRunLoopTimerRef m_pollTimer{nullptr};
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
