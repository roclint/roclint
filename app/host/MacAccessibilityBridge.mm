#include "IAccessibilityBridge.hpp"
#include "GuiCapturePolicy.hpp"
#include "HostContext.hpp"
#include "RangeInjectionPolicy.hpp"
#include "TerminalAdapter.hpp"
#include "WordSpan.hpp"
#include <algorithm>
#include <QStringList>

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
constexpr CFIndex kMaxReadChars = kMacGuiCaptureLimit;
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

// Objects returned by these readers live in the caller's autorelease pool.
id axAttribute(AXUIElementRef element, CFStringRef key) {
    CFTypeRef value = nullptr;
    if (AXUIElementCopyAttributeValue(element, key, &value) != kAXErrorSuccess) return nil;
    return [(id)value autorelease];
}

id axParameter(AXUIElementRef element, CFStringRef key, id parameter) {
    CFTypeRef value = nullptr;
    if (!parameter || AXUIElementCopyParameterizedAttributeValue(element, key, (CFTypeRef)parameter, &value) != kAXErrorSuccess)
        return nil;
    return [(id)value autorelease];
}

id axRange(CFRange range) {
    return [(id)AXValueCreate(static_cast<AXValueType>(kAXValueCFRangeType), &range) autorelease];
}

std::optional<QString> guiText(AXUIElementRef element, bool rich) {
    NSNumber* length = axAttribute(element, kAXNumberOfCharactersAttribute);
    std::optional<qsizetype> count;
    if ([length isKindOfClass:NSNumber.class]) count = length.longLongValue;
    if (count && (*count < 0 || *count > kMaxReadChars)) return std::nullopt;
    const auto value = readStringAttribute(element, kAXValueAttribute);
    // Chromium can cap both AXValue and AXNumberOfCharacters at 10,000
    // UTF-16 units. The marker range belongs to this exact focused control.
    if (rich && (!value || value->isEmpty() || value->size() >= 10000 || (count && *count != value->size()))) {
        id markers = axParameter(element, CFSTR("AXTextMarkerRangeForUIElement"), (id)element);
        NSNumber* complete = axParameter(element, CFSTR("AXLengthForTextMarkerRange"), markers);
        if ([complete isKindOfClass:NSNumber.class]) count = complete.longLongValue;
        else if ((value && value->size() == 10000) || count == 10000) return std::nullopt;
    }
    if (count && (*count < 0 || *count > kMaxReadChars)) return std::nullopt;
    const bool verifyEmpty = rich || copyStringAttribute(element, kAXRoleAttribute) == QLatin1String("AXComboBox");
    std::optional<QString> range;
    if (count && (!value || value->isEmpty() || *count != value->size())) {
        NSString* text = axParameter(element, kAXStringForRangeParameterizedAttribute, axRange({0, *count}));
        if ([text isKindOfClass:NSString.class]) range = QString::fromNSString(text);
    }
    return verifiedMacGuiText(value, verifyEmpty, count, range, copyStringAttribute(element, kAXPlaceholderValueAttribute));
}

bool hasClass(AXUIElementRef el, NSString* name) {
    NSArray* classes = axAttribute(el, CFSTR("AXDOMClassList"));
    return [classes isKindOfClass:NSArray.class] && [classes containsObject:name];
}

AXUIElementRef xtermPane(AXUIElementRef el) {
    for (int depth = 0; el && depth < 12; ++depth) {
        if (hasClass(el, @"xterm")) return el;
        el = (AXUIElementRef)axAttribute(el, kAXParentAttribute);
    }
    return nullptr;
}

AXUIElementRef findRowTree(AXUIElementRef el, int depth, int& budget) {
    if (--budget < 0 || depth > 8) return nullptr;
    if (hasClass(el, @"xterm-accessibility-tree")) return el;
    NSArray* children = axAttribute(el, kAXChildrenAttribute);
    for (id child in children) {
        if (budget <= 0) break;
        if (auto tree = findRowTree((AXUIElementRef)child, depth + 1, budget)) return tree;
    }
    return nullptr;
}

std::optional<QString> terminalRows(AXUIElementRef pane) {
    int budget = 128;
    auto tree = pane ? findRowTree(pane, 0, budget) : nullptr;
    if (!tree) return std::nullopt;
    NSArray* rows = axAttribute(tree, kAXChildrenAttribute);
    if (![rows isKindOfClass:NSArray.class] || !rows.count || rows.count > 512) return std::nullopt;
    // xterm's visible rows can be old scrollback even while the helper has focus.
    // Only the viewport ending at the final buffer row can describe the draft.
    NSNumber* last = axAttribute((AXUIElementRef)rows.lastObject, CFSTR("AXARIAPosInSet"));
    NSNumber* total = axAttribute(tree, CFSTR("AXARIASetSize"));
    if (![total isKindOfClass:NSNumber.class])
        total = axAttribute((AXUIElementRef)rows.lastObject, CFSTR("AXARIASetSize"));
    if (![last isKindOfClass:NSNumber.class] || ![total isKindOfClass:NSNumber.class] ||
        last.integerValue <= 0 || last.integerValue != total.integerValue) return std::nullopt;
    QStringList lines;
    qsizetype length = 0;
    for (id row in rows) {
        // Chromium exposes xterm list items as groups of static text.
        auto rowElement = (AXUIElementRef)row;
        NSArray* fragments = axAttribute(rowElement, kAXChildrenAttribute);
        std::optional<QString> text;
        if (fragments.count) {
            QString joined;
            for (id fragment in fragments) {
                auto child = (AXUIElementRef)fragment;
                if (copyStringAttribute(child, kAXRoleAttribute) != QLatin1String("AXStaticText")) return std::nullopt;
                auto value = readStringAttribute(child, kAXValueAttribute);
                if (!value || joined.size() + value->size() > kMaxReadChars) return std::nullopt;
                joined += *value;
            }
            text = joined;
        } else {
            text = readStringAttribute(rowElement, kAXValueAttribute);
            if (!text || text->isEmpty()) text = readStringAttribute(rowElement, kAXDescriptionAttribute);
            if (!text) return std::nullopt;
        }
        length += text->size() + 1;
        if (length > kMaxReadChars) return std::nullopt;
        lines.append(*text);
    }
    // xterm pads its viewport with blank rows below the live cursor.
    while (!lines.isEmpty() && lines.last().trimmed().isEmpty()) lines.removeLast();
    if (lines.isEmpty()) return std::nullopt;
    return lines.join(QLatin1Char('\n'));
}

using Word = std::pair<int, int>;

std::optional<Word> recentTypo(const QString& text) {
    int start = std::max(0, static_cast<int>(text.size()) - 1600);
    while (start > 0 && start < text.size() && !text[start - 1].isSpace()) ++start;
    NSString* suffix = text.mid(start).toNSString();
    NSSpellChecker* checker = [NSSpellChecker sharedSpellChecker];
    std::optional<Word> result;
    NSUInteger cursor = 0;
    for (int errors = 0; cursor < suffix.length && errors < 64; ++errors) {
        NSRange r = [checker checkSpellingOfString:suffix startingAt:cursor language:nil wrap:NO inSpellDocumentWithTag:0 wordCount:nullptr];
        if (r.location == NSNotFound || !r.length || r.location < cursor || NSMaxRange(r) > suffix.length) break;
        cursor = NSMaxRange(r);
        int first = start + static_cast<int>(r.location), end = start + static_cast<int>(cursor);
        const auto span = wordSpanAtCaret(text, first);
        const auto word = QStringView(text).mid(first, end - first);
        if (r.length >= 4 && span == Word(first, end) && std::all_of(word.begin(), word.end(), [](QChar c) { return c.isLower(); }))
            result = span;
    }
    return result;
}

QRect elementRect(AXUIElementRef element) {
    id pos = axAttribute(element, kAXPositionAttribute);
    id size = axAttribute(element, kAXSizeAttribute);
    CGPoint origin{};
    CGSize extent{};
    if (!pos || !size || CFGetTypeID((CFTypeRef)pos) != AXValueGetTypeID() ||
        CFGetTypeID((CFTypeRef)size) != AXValueGetTypeID() ||
        !AXValueGetValue((AXValueRef)pos, static_cast<AXValueType>(kAXValueCGPointType), &origin) ||
        !AXValueGetValue((AXValueRef)size, static_cast<AXValueType>(kAXValueCGSizeType), &extent)) return {};
    return QRectF(origin.x, origin.y, extent.width, extent.height).toAlignedRect();
}

std::optional<QRect> textBounds(AXUIElementRef element, CFRange range) {
    id value = axParameter(element, kAXBoundsForRangeParameterizedAttribute, axRange(range));
    CGRect bounds{};
    if (!value || CFGetTypeID((CFTypeRef)value) != AXValueGetTypeID() ||
        !AXValueGetValue((AXValueRef)value, static_cast<AXValueType>(kAXValueCGRectType), &bounds) ||
        CGRectIsInfinite(bounds) || CGRectIsNull(bounds)) return std::nullopt;
    return QRectF(bounds.origin.x, bounds.origin.y, bounds.size.width, bounds.size.height).toAlignedRect();
}

void readWordBounds(AXUIElementRef el, ObservedField& f, Word span) {
    const auto [start, end] = span;
    if (f.fieldRect.isEmpty()) return;
    NSString* word = axParameter(el, kAXStringForRangeParameterizedAttribute, axRange({start, end - start}));
    if (![word isKindOfClass:NSString.class] || QString::fromNSString(word) != f.text.mid(start, end - start)) return;
    QRect viewport = f.fieldRect;
    auto ancestor = el;
    for (int depth = 0; ancestor && depth < 16; ++depth) {
        const auto role = copyStringAttribute(ancestor, kAXRoleAttribute);
        if (role == QLatin1String("AXScrollArea") || role == QLatin1String("AXWindow")) {
            const auto bounds = elementRect(ancestor);
            if (!bounds.isEmpty()) viewport = viewport.intersected(bounds);
            if (role == QLatin1String("AXWindow")) break;
        }
        ancestor = (AXUIElementRef)axAttribute(ancestor, kAXParentAttribute);
    }
    const auto bounds = textBounds(el, {start, end - start});
    const auto first = textBounds(el, {start, 1});
    if (!bounds || !first) return;
    QList<QRect> rects;
    if (bounds->top() == first->top() && bounds->bottom() == first->bottom()) {
        const auto visible = bounds->intersected(viewport);
        if (!visible.isEmpty()) rects.append(visible);
    } else {
        // Chromium reports a union across soft wraps even when AXRangeForLine
        // identifies a single logical line. Join only adjacent glyph bounds.
        if (end - start > 128) return;
        for (int pos = start; pos < end; ++pos) {
            const auto glyph = pos == start ? first : textBounds(el, {pos, 1});
            if (!glyph) return;
            const auto visible = glyph->intersected(viewport);
            if (visible.isEmpty()) continue;
            if (!rects.isEmpty() && rects.last().top() == visible.top() &&
                rects.last().bottom() == visible.bottom() && rects.last().adjusted(-1, 0, 1, 0).intersects(visible))
                rects.last() = rects.last().united(visible);
            else
                rects.append(visible);
        }
    }
    f.wordStartOffset = start;
    f.wordEndOffset = end;
    f.wordRects = rects;
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

    void setCardVisible(bool visible) override {
        m_cardVisible.store(visible);
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
            } else {
                detachFocused();
                m_lastField.reset();
                emit fieldLost();
                return;
            }
            readAndEmit(requestId, true);
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
    struct KnownField { AXUIElementRef element; pid_t pid; QString bundle; quint64 token; AXUIElementRef pane; FieldKind kind; QString documentUrl; };

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
        CFRunLoopPerformBlock(m_loop, kCFRunLoopDefaultMode, ^{ @autoreleasepool { block(); } });
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
                @autoreleasepool { this->onHeartbeat(); }
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

    static QString documentUrlFor(AXUIElementRef element) {
        auto window = (AXUIElementRef)axAttribute(element, kAXWindowAttribute);
        return window ? copyStringAttribute(window, kAXDocumentAttribute) : QString{};
    }

    quint64 tokenFor(AXUIElementRef element) {
        const bool embedded = isElectronBundle(m_bundleId) && hasClass(element, @"xterm-helper-textarea");
        const auto pane = embedded ? xtermPane(element) : nullptr;
        const auto kind = embedded || isTerminalBundle(m_bundleId) ? FieldKind::Terminal : FieldKind::GuiEditor;
        const auto documentUrl = kind == FieldKind::GuiEditor ? documentUrlFor(element) : QString{};
        for (const auto& field : m_knownFields) {
            const bool samePane = pane == field.pane || (pane && field.pane && CFEqual(pane, field.pane));
            if (field.kind == kind && field.documentUrl == documentUrl && samePane && field.pid == m_pid && field.bundle == m_bundleId &&
                CFEqual(field.element, element)) return field.token;
        }
        if (m_knownFields.size() == 32) {
            releaseField(m_knownFields.front());
            m_knownFields.erase(m_knownFields.begin());
        }
        const auto token = ++m_nextFieldToken;
        CFRetain(element);
        if (pane) CFRetain(pane);
        m_knownFields.push_back({element, m_pid, m_bundleId, token, pane, kind, documentUrl});
        return token;
    }
    static void releaseField(const KnownField& field) {
        CFRelease(field.element);
        if (field.pane) CFRelease(field.pane);
    }

    void clearKnownFields() {
        for (const auto& field : m_knownFields) releaseField(field);
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
        m_spellingText.clear();
        m_typo.reset();
        m_lastField.reset();
        m_newFocus = true;
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
        if (!HostContextDetector::isAmbientEligibleApp(m_bundleId)) {
            emit fieldLost();
            return;
        }
        m_app = AXUIElementCreateApplication(pid);
        if (m_app == nullptr) {
            return;
        }
        if (isElectronBundle(m_bundleId)) {
            // Chromium builds its accessibility tree only when asked.
            AXUIElementSetAttributeValue(m_app, CFSTR("AXManualAccessibility"), kCFBooleanTrue);
            AXUIElementSetAttributeValue(m_app, CFSTR("AXEnhancedUserInterface"), kCFBooleanTrue);
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
                ^(CFRunLoopTimerRef) { @autoreleasepool { this->onFocusChanged(); } });
            CFRunLoopAddTimer(m_loop, retry, kCFRunLoopDefaultMode);
            CFRelease(retry);
        }
    }

    void onFocusChanged() {
        if (m_app == nullptr) {
            return;
        }
        // A floating panel can retain a key window while an editor owns input.
        if (frontmostPid() != m_pid) {
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
            const auto previous = m_token;
            m_token = tokenFor(m_focused);
            if (previous != m_token) readAndEmit();
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
        // A rich editor can reparent a retained input into a different pane.
        if (const auto* previous = capturedField(m_token, FieldKind::Terminal);
            previous && !isTerminalBundle(m_bundleId)) m_token = tokenFor(m_focused);
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
        const auto target = std::find_if(m_knownFields.begin(), m_knownFields.end(), [this](const KnownField& known) { return known.token == m_token; });
        if (target == m_knownFields.end()) return std::nullopt;
        f.kind = target->kind;
        f.documentUrl = target->documentUrl;
        f.isPassword = copyStringAttribute(m_focused, kAXSubroleAttribute) == QLatin1String("AXSecureTextField");
        if (f.isPassword) {
            return f;
        }
        auto text = captureText(*target);
        if (!text) return std::nullopt;
        f.text = *text;

        if (!target->pane) {
            if (auto selection = readSelection(m_focused)) {
                f.caretOffset = static_cast<int>(std::min<CFIndex>(selection->location, f.text.size()));
                id value = axParameter(m_focused, kAXBoundsForRangeParameterizedAttribute, axRange({selection->location, 0}));
                CGRect rect{};
                if (value && CFGetTypeID((CFTypeRef)value) == AXValueGetTypeID() &&
                    AXValueGetValue((AXValueRef)value, static_cast<AXValueType>(kAXValueCGRectType), &rect) && rect.size.height > 0) {
                    f.caretRect = QRect(qRound(rect.origin.x), qRound(rect.origin.y),
                                        std::max(1, qRound(rect.size.width)), std::max(1, qRound(rect.size.height)));
                }
            }
        }
        const auto container = target->pane ? target->pane : m_focused;
        if (container) f.fieldRect = elementRect(container);

        if (f.kind == FieldKind::Terminal) {
            const auto rawCaret = target->pane ? static_cast<int>(f.text.size()) : f.caretOffset;
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
        // Inline word marks are hidden while the HUD follows text. Avoid spelling
        // and ancestor/glyph AX round trips on the critical path of each edit.
        if (!m_cardVisible.load() && f.kind == FieldKind::GuiEditor && f.caretOffset >= 0) {
            if (QStringView(f.text).mid(f.caretOffset).trimmed().isEmpty()) {
                if (m_spellingText != f.text) {
                    m_spellingText = f.text;
                    m_typo = recentTypo(f.text);
                }
                if (m_typo) readWordBounds(m_focused, f, *m_typo);
            }
            if (f.wordStartOffset < 0) {
                if (auto span = wordSpanAtCaret(f.text, f.caretOffset)) readWordBounds(m_focused, f, *span);
            }
        }
        return f;
    }

    void readAndEmit(quint64 requestId = 0, bool force = false) {
        auto field = readField();
        if (!field) {
            if (m_lastField || force || m_newFocus) emit fieldLost();
            m_lastField.reset();
            m_newFocus = false;
            return;
        }
        if (!force && m_lastField && *field == *m_lastField) return;
        m_newFocus = false;
        m_lastField = field;
        field->refreshRequestId = requestId;
        if (qEnvironmentVariableIsSet("ROCLINT_CAPTURE_DIAGNOSTICS")) {
            qInfo() << "[MacAX] readAndEmit: characters=" << field->text.size() << "fieldRect=" << field->fieldRect;
        }
        emit fieldObserved(*field);
    }

    const KnownField* capturedField(quint64 token, FieldKind kind) const {
        for (const auto& field : m_knownFields) {
            if (field.token == token &&
                field.kind == kind) return &field;
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
        if (!matches || !target.pane) return matches;
        const auto pane = xtermPane(target.element);
        return pane && CFEqual(pane, target.pane);
    }

    static bool belongsToWebArea(const KnownField& target, AXUIElementRef area) {
        if (!area || target.kind != FieldKind::GuiEditor || !isElectronBundle(target.bundle) ||
            copyStringAttribute(area, kAXRoleAttribute) != QLatin1String("AXWebArea")) return false;
        auto parent = target.element;
        for (int depth = 0; parent && depth < 32; ++depth) {
            if (CFEqual(parent, area)) return true;
            parent = (AXUIElementRef)axAttribute(parent, kAXParentAttribute);
        }
        return false;
    }

    // Chromium can return focus to the containing web document when the
    // user clicks Roclint. Restore only the last observed editor inside that
    // exact focused document and window. A sibling input or pane is a veto.
    static bool focusReturnedWebEditor(AXUIElementRef app, const KnownField& target) {
        if (target.kind != FieldKind::GuiEditor || !isElectronBundle(target.bundle) ||
            frontmostPid() != target.pid) return false;
        auto focused = (AXUIElementRef)axAttribute(app, kAXFocusedUIElementAttribute);
        if (!belongsToWebArea(target, focused)) return false;
        id window = axAttribute(app, kAXFocusedWindowAttribute);
        id capturedWindow = axAttribute(target.element, kAXWindowAttribute);
        if (!window || !capturedWindow || !CFEqual((CFTypeRef)window, (CFTypeRef)capturedWindow)) return false;
        const auto rect = elementRect(target.element);
        if (rect.isEmpty()) return false;
        AXUIElementRef hit = nullptr;
        const auto center = rect.center();
        if (AXUIElementCopyElementAtPosition(app, center.x(), center.y(), &hit) != kAXErrorSuccess || !hit) return false;
        bool visible = CFEqual(hit, target.element);
        // Rich composers can expose the rendered text under the pointer as a child.
        if (!visible && copyStringAttribute(hit, kAXRoleAttribute) == QLatin1String("AXStaticText")) {
            auto parent = (AXUIElementRef)axAttribute(hit, kAXParentAttribute);
            for (int depth = 0; parent && depth < 32; ++depth) {
                if (CFEqual(parent, target.element)) { visible = true; break; }
                if (copyStringAttribute(parent, kAXRoleAttribute) != QLatin1String("AXGroup")) break;
                parent = (AXUIElementRef)axAttribute(parent, kAXParentAttribute);
            }
        }
        CFRelease(hit);
        if (!visible) return false;
        return AXUIElementSetAttributeValue(target.element, kAXFocusedAttribute, kCFBooleanTrue) == kAXErrorSuccess;
    }

    bool activateCapturedApp(AXUIElementRef app, const KnownField& target) {
        const auto front = frontmostPid();
        if (front == target.pid) {
            if (targetFocused(app, target)) return true;
            if (!focusReturnedWebEditor(app, target)) return false;
            for (int attempt = 0; attempt < 18 && frontmostPid() == target.pid; ++attempt) {
                if (targetFocused(app, target)) return true;
                usleep(kPollMicros);
            }
            return false;
        }
        if (front != getpid() || target.pid != m_pid || !m_focused) return false;
        const bool sameField = target.token == m_token && CFEqual(target.element, m_focused);
        if (!sameField && !belongsToWebArea(target, m_focused)) return false;

        @autoreleasepool {
            NSRunningApplication* application = [NSRunningApplication runningApplicationWithProcessIdentifier:target.pid];
            if (!application || !application.bundleIdentifier ||
                QString::fromNSString(application.bundleIdentifier) != target.bundle) return false;
            // AX can return the already verified editor application to the
            // foreground without selecting a window or an input on its behalf.
            if (AXUIElementSetAttributeValue(app, kAXFrontmostAttribute, kCFBooleanTrue) != kAXErrorSuccess) {
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
        }
        bool restoredWebFocus = false;
        for (int i = 0; i < 18; ++i) {
            const auto active = frontmostPid();
            if (active != getpid() && active != target.pid) return false;
            if (targetFocused(app, target)) return true;
            if (!restoredWebFocus && active == target.pid)
                restoredWebFocus = focusReturnedWebEditor(app, target);
            usleep(kPollMicros);
        }
        return false;
    }

    static std::optional<QString> captureText(const KnownField& target) {
        if (target.kind == FieldKind::GuiEditor) {
            auto text = guiText(target.element, isElectronBundle(target.bundle));
            // Codex can expose its empty-composer hint as AX text with a trailing newline.
            // Only query the selection for a candidate hint, off the ordinary typing path.
            if (text && isRichComposerPlaceholder(*text, {}, {}, target.bundle, -1)) {
                const auto selection = readSelection(target.element);
                if (!selection || (selection->location == 0 && selection->length == 0)) return QString{};
            }
            return text;
        }
        if (!isTerminalBundle(target.bundle)) {
            const auto pane = xtermPane(target.element);
            if (!pane || !target.pane || !CFEqual(pane, target.pane)) return std::nullopt;
            return terminalRows(pane);
        }
        return readStringAttribute(target.element, kAXValueAttribute);
    }

    static std::optional<QString> deliveryText(const KnownField& target, FieldKind kind) {
        auto text = captureText(target);
        if (kind == FieldKind::GuiEditor || !text) return text;

        const auto selection = target.pane ? std::nullopt : readSelection(target.element);
        const auto caret = selection ? std::min(selection->location, static_cast<CFIndex>(text->size())) : text->size();
        auto prefix = text->left(caret);
        prefix.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        prefix.replace(QLatin1Char('\r'), QLatin1Char('\n'));
        const auto draft = TerminalAdapter::extractDraft(prefix, prefix.size());
        return draft ? std::optional{draft->text} : std::nullopt;
    }

    static bool exactText(AXUIElementRef app, const KnownField& target, FieldKind kind, const QString& expected) {
        const bool focused = targetFocused(app, target);
        const auto current = focused ? deliveryText(target, kind) : std::nullopt;
        const bool matches = current && RangeInjectionPolicy::readbackMatches(expected, *current);
        if (!matches && qEnvironmentVariableIsSet("ROCLINT_CAPTURE_DIAGNOSTICS"))
            qInfo() << "[MacAX] replacement precondition failed: focus=" << focused << "readable=" << current.has_value();
        return matches;
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
    InjectionOutcome withCapturedField(quint64 token, FieldKind kind, const QString& before, Write write, QString* detail = nullptr) {
        if (!m_observing || !AXIsProcessTrusted()) return InjectionOutcome::NotSupported;
        const auto* target = capturedField(token, kind);
        if (!target) {
            if (detail) *detail = QStringLiteral("The captured editor field is no longer available.");
            return InjectionOutcome::FieldChanged;
        }
        const auto subrole = copyStringAttribute(target->element, kAXSubroleAttribute);
        if (subrole == QLatin1String("AXSecureTextField")) return InjectionOutcome::NotSupported;
        const auto current = deliveryText(*target, kind);
        if (!current) return InjectionOutcome::NotSupported;
        if (!RangeInjectionPolicy::preconditionsHold(token, target->token, before, *current)) {
            if (detail) *detail = QStringLiteral("The draft text changed after review.");
            if (qEnvironmentVariableIsSet("ROCLINT_CAPTURE_DIAGNOSTICS"))
                qInfo() << "[MacAX] captured draft changed before delivery";
            return InjectionOutcome::FieldChanged;
        }

        const std::unique_ptr<std::remove_pointer_t<AXUIElementRef>, decltype(&CFRelease)>
            app(AXUIElementCreateApplication(target->pid), CFRelease);
        if (!app || !activateCapturedApp(app.get(), *target)) {
            if (detail) *detail = QStringLiteral("The original composer could not regain focus.");
            if (qEnvironmentVariableIsSet("ROCLINT_CAPTURE_DIAGNOSTICS"))
                qInfo() << "[MacAX] delivery refused: original field did not regain focus";
            return InjectionOutcome::FieldChanged;
        }
        if (!target->documentUrl.isEmpty() && documentUrlFor(target->element) != target->documentUrl)
            return InjectionOutcome::FieldChanged;
        if (!exactText(app.get(), *target, kind, before)) return InjectionOutcome::FieldChanged;
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
            QString detail;
            const auto outcome = withCapturedField(r.fieldToken, r.kind, r.expectedCurrentText,
                [&](AXUIElementRef app, const KnownField& target) {
                    if ((r.processId > 0 && r.processId != target.pid) ||
                        (!r.appIdentifier.isEmpty() && r.appIdentifier != target.bundle))
                        return InjectionOutcome::FieldChanged;
                    if (r.kind == FieldKind::Terminal) return replaceTerminalDraft(app, target, r);
                    if (r.text.size() > kMaxReadChars) return InjectionOutcome::NotSupported;
                    return replaceGuiValue(app, target, r.expectedCurrentText, r.text);
                }, &detail);
            emit injectionFinished(r.requestId, outcome, detail.isEmpty() ? deliveryDetail(outcome) : detail);
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
            if (expected.size() > kMaxReadChars) {
                emit rangeInjectionFinished(r.requestId, InjectionOutcome::NotSupported,
                    QStringLiteral("The replacement would exceed the verified capture limit."));
                return;
            }
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
    std::atomic<bool> m_cardVisible{false};
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
    bool m_newFocus{true};
    std::optional<ObservedField> m_lastField;
    QString m_spellingText;
    std::optional<Word> m_typo;
    CFRunLoopTimerRef m_pollTimer{nullptr};
    int m_consecutiveFocusMisses{0};
};

void axCallback(AXObserverRef, AXUIElementRef, CFStringRef notification, void* refcon) {
    @autoreleasepool { static_cast<MacAccessibilityBridge*>(refcon)->onNotification(notification); }
}

} // namespace

std::unique_ptr<IAccessibilityBridge> createPlatformAccessibilityBridge() {
    return std::make_unique<MacAccessibilityBridge>();
}

} // namespace roclint::host

#endif
