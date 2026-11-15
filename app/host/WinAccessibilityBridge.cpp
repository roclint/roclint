#include "IAccessibilityBridge.hpp"
#include "HostContext.hpp"
#include "TerminalAdapter.hpp"
#include "GuiCapturePolicy.hpp"
#include "RangeInjectionPolicy.hpp"

#if defined(Q_OS_WIN)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <oleauto.h>
#include <uiautomation.h>
#include <wrl/client.h>

#include <QStringList>
#include <QDebug>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <map>
#include <optional>
#include <set>
#include <thread>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace roclint::host {

namespace {

constexpr int kMaxReadChars = 16000;
constexpr auto kPollInterval = std::chrono::milliseconds(500);
constexpr auto kElectronWakeDelay = std::chrono::milliseconds(60);
constexpr DWORD kTerminalEchoMs = 300;

QString fromBstr(BSTR value) {
    if (value == nullptr) {
        return {};
    }
    return QString::fromWCharArray(value, static_cast<qsizetype>(SysStringLen(value)));
}

bool hasWritablePattern(IUIAutomationElement* element) {
    ComPtr<IUIAutomationValuePattern> value;
    if (SUCCEEDED(element->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(value.GetAddressOf()))) && value) {
        BOOL readOnly = TRUE;
        if (SUCCEEDED(value->get_CurrentIsReadOnly(&readOnly)) && !readOnly) return true;
    }
    ComPtr<IUIAutomationTextPattern> text;
    ComPtr<IUIAutomationTextRange> range;
    if (FAILED(element->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(text.GetAddressOf()))) || !text ||
        FAILED(text->get_DocumentRange(range.GetAddressOf())) || !range) return false;
    VARIANT readOnly;
    VariantInit(&readOnly);
    const auto hr = range->GetAttributeValue(UIA_IsReadOnlyAttributeId, &readOnly);
    const bool writable = SUCCEEDED(hr) && readOnly.vt == VT_BOOL && readOnly.boolVal == VARIANT_FALSE;
    VariantClear(&readOnly);
    return writable;
}

bool isGuiEditor(IUIAutomationElement* element) {
    CONTROLTYPEID type = 0;
    if (!element || FAILED(element->get_CurrentControlType(&type))) return false;
    GuiCaptureRole role = GuiCaptureRole::Other;
    switch (type) {
        case UIA_EditControlTypeId: role = GuiCaptureRole::Edit; break;
        case UIA_DocumentControlTypeId: role = GuiCaptureRole::Document; break;
        case UIA_ComboBoxControlTypeId: role = GuiCaptureRole::ComboBox; break;
        case UIA_GroupControlTypeId: role = GuiCaptureRole::Group; break;
        case UIA_CustomControlTypeId: role = GuiCaptureRole::Custom; break;
        default: break;
    }
    BSTR id = nullptr;
    element->get_CurrentAutomationId(&id);
    const bool webRoot = role == GuiCaptureRole::Document && fromBstr(id) == QLatin1String("RootWebArea");
    SysFreeString(id);
    return isGuiCaptureEligible(role, hasWritablePattern(element), webRoot);
}

// Canonical LF text; caret offsets are shifted for every CR removed before them.
void normalizeText(QString& text, int& caret) {
    QString out;
    out.reserve(text.size());
    int removedBeforeCaret = 0;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text[i];
        if (c == QLatin1Char('\r')) {
            const bool crlf = i + 1 < text.size() && text[i + 1] == QLatin1Char('\n');
            if (caret >= 0 && i < caret) {
                if (crlf) {
                    ++removedBeforeCaret;
                }
            }
            if (!crlf) {
                out.append(QLatin1Char('\n'));
            }
            continue;
        }
        out.append(c);
    }
    text = out;
    if (caret >= 0) {
        caret = std::max(0, caret - removedBeforeCaret);
    }
}

QString executableName(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        return {};
    }
    wchar_t path[MAX_PATH];
    DWORD size = MAX_PATH;
    QString name;
    if (QueryFullProcessImageNameW(process, 0, path, &size)) {
        const QString full = QString::fromWCharArray(path, static_cast<qsizetype>(size));
        name = full.mid(full.lastIndexOf(QLatin1Char('\\')) + 1);
    }
    CloseHandle(process);
    return name;
}

std::optional<bool> isProcessElevated(HANDLE process) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) {
        return std::nullopt;
    }
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    if (!ok) {
        return std::nullopt;
    }
    return elevation.TokenIsElevated != 0;
}

// UIPI blocks a non-elevated process from driving an elevated one. Unknown counts as a mismatch.
bool elevationMismatch(DWORD pid) {
    const auto self = isProcessElevated(GetCurrentProcess());
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        return true;
    }
    const auto target = isProcessElevated(process);
    CloseHandle(process);
    if (!self.has_value() || !target.has_value()) {
        return true;
    }
    return *target && !*self;
}

bool isElectronHost(const QString& exe) {
    const QString e = exe.toLower();
    return e == QLatin1String("cursor.exe") || e == QLatin1String("code.exe") ||
           e == QLatin1String("antigravity.exe") || e == QLatin1String("antigravity ide.exe") ||
           e == QLatin1String("code - insiders.exe") ||
           e == QLatin1String("chatgpt.exe") || e == QLatin1String("codex.exe") ||
           e == QLatin1String("windsurf.exe");
}

struct FindRenderWidget {
    HWND found{nullptr};
};

BOOL CALLBACK findRenderWidgetProc(HWND hwnd, LPARAM param) {
    wchar_t cls[64];
    if (GetClassNameW(hwnd, cls, 64) > 0 && wcscmp(cls, L"Chrome_RenderWidgetHostHWND") == 0) {
        reinterpret_cast<FindRenderWidget*>(param)->found = hwnd;
        return FALSE;
    }
    return TRUE;
}

class ClipboardText {
public:
    static std::optional<std::wstring> read() {
        if (!OpenClipboard(nullptr)) {
            return std::nullopt;
        }
        std::optional<std::wstring> out;
        if (HANDLE data = GetClipboardData(CF_UNICODETEXT); data != nullptr) {
            if (const auto* text = static_cast<const wchar_t*>(GlobalLock(data)); text != nullptr) {
                out = std::wstring(text);
                GlobalUnlock(data);
            }
        }
        CloseClipboard();
        return out;
    }

    static bool write(const std::wstring& text, HWND owner) {
        // EmptyClipboard with a null owner makes SetClipboardData fail on Windows.
        if (owner == nullptr || !OpenClipboard(owner)) {
            return false;
        }
        EmptyClipboard();
        const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        bool ok = false;
        if (mem != nullptr) {
            if (void* dst = GlobalLock(mem); dst != nullptr) {
                memcpy(dst, text.c_str(), bytes);
                GlobalUnlock(mem);
                ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
            }
            if (!ok) {
                GlobalFree(mem);
            }
        }
        CloseClipboard();
        return ok;
    }
};

bool sendChord(std::initializer_list<WORD> modifiers, WORD key) {
    std::vector<INPUT> inputs;
    for (const WORD m : modifiers) {
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = m;
        inputs.push_back(in);
    }
    INPUT down{};
    down.type = INPUT_KEYBOARD;
    down.ki.wVk = key;
    inputs.push_back(down);
    INPUT up = down;
    up.ki.dwFlags = KEYEVENTF_KEYUP;
    inputs.push_back(up);
    for (auto it = modifiers.end(); it != modifiers.begin();) {
        --it;
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = *it;
        in.ki.dwFlags = KEYEVENTF_KEYUP;
        inputs.push_back(in);
    }
    const auto count = static_cast<UINT>(inputs.size());
    return SendInput(count, inputs.data(), sizeof(INPUT)) == count;
}

// Event sinks: UIA calls these on its own threads, so they only forward to the worker queue.
template <typename Interface>
class Sink : public Interface {
public:
    explicit Sink(std::function<void(IUIAutomationElement*)> onEvent) : m_onEvent(std::move(onEvent)) {}
    virtual ~Sink() = default;

    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(++m_refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = static_cast<ULONG>(--m_refs);
        if (left == 0) {
            delete this;
        }
        return left;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (out == nullptr) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(Interface)) {
            *out = static_cast<Interface*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }

protected:
    void forward(IUIAutomationElement* sender) {
        if (m_onEvent) {
            m_onEvent(sender);
        }
    }

private:
    std::atomic<long> m_refs{1};
    std::function<void(IUIAutomationElement*)> m_onEvent;
};

class FocusSink final : public Sink<IUIAutomationFocusChangedEventHandler> {
public:
    using Sink::Sink;
    HRESULT STDMETHODCALLTYPE HandleFocusChangedEvent(IUIAutomationElement* sender) override {
        forward(sender);
        return S_OK;
    }
};

class TextSink final : public Sink<IUIAutomationEventHandler> {
public:
    using Sink::Sink;
    HRESULT STDMETHODCALLTYPE HandleAutomationEvent(IUIAutomationElement* sender, EVENTID) override {
        forward(sender);
        return S_OK;
    }
};

class ValueSink final : public Sink<IUIAutomationPropertyChangedEventHandler> {
public:
    using Sink::Sink;
    HRESULT STDMETHODCALLTYPE HandlePropertyChangedEvent(IUIAutomationElement* sender, PROPERTYID, VARIANT) override {
        forward(sender);
        return S_OK;
    }
};

class WinAccessibilityBridge final : public IAccessibilityBridge {
public:
    WinAccessibilityBridge() { m_thread = std::thread([this]() { run(); }); }

    ~WinAccessibilityBridge() override {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
        }
        m_wake.notify_all();
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    [[nodiscard]] AccessibilityAccess access() const override {
        return m_available.load() ? AccessibilityAccess::Granted : AccessibilityAccess::Unavailable;
    }

    // Windows UI Automation needs no user permission.
    bool requestAccess() override { return false; }

    void setObserving(bool observing) override {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_wantObserving = observing;
            m_observingChanged = true;
        }
        m_wake.notify_all();
    }

    void refreshFocusedField(quint64 requestId = 0) override {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_requestedRefreshId = requestId;
            m_focusDirty = m_textDirty = true;
        }
        m_wake.notify_all();
    }

    void inject(const InjectionRequest& request) override {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_injections.push_back(request);
        }
        m_wake.notify_all();
    }

    void injectRange(const RangeInjectionRequest& request) override {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_rangeInjections.push_back(request);
        }
        m_wake.notify_all();
    }

private:
    struct Focused {
        ComPtr<IUIAutomationElement> element;
        quint64 token{0};
        DWORD pid{0};
        HWND window{nullptr};
        QString exe;
        FieldKind kind{FieldKind::GuiEditor};
        bool xterm{false};
        bool eligible{false};
    };

    void run() {
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
            emit accessChanged();
            return;
        }
        HRESULT hr = CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(m_uia.GetAddressOf()));
        if (FAILED(hr)) {
            hr = CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(m_uia.GetAddressOf()));
        }
        m_available = SUCCEEDED(hr) && m_uia;
        m_clipboardOwner = CreateWindowExW(0, L"STATIC", L"Roclint clipboard", 0,
                                           0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
        emit accessChanged();

        auto nextPoll = std::chrono::steady_clock::now() + kPollInterval;
        while (true) {
            std::deque<InjectionRequest> injections;
            std::deque<RangeInjectionRequest> rangeInjections;
            bool observingChanged = false;
            bool wantObserving = false;
            bool focusDirty = false;
            bool textDirty = false;
            quint64 refreshId = 0;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_wake.wait_until(lock, nextPoll, [this]() {
                    return m_stop || m_observingChanged || m_focusDirty || m_textDirty || !m_injections.empty() ||
                           !m_rangeInjections.empty() ||
                           (m_wakeRecheckAt.has_value() && std::chrono::steady_clock::now() >= *m_wakeRecheckAt);
                });
                if (m_stop) {
                    break;
                }
                injections.swap(m_injections);
                rangeInjections.swap(m_rangeInjections);
                observingChanged = std::exchange(m_observingChanged, false);
                wantObserving = m_wantObserving;
                focusDirty = std::exchange(m_focusDirty, false);
                textDirty = std::exchange(m_textDirty, false);
                refreshId = std::exchange(m_requestedRefreshId, 0);
                if (m_wakeRecheckAt.has_value() && std::chrono::steady_clock::now() >= *m_wakeRecheckAt) {
                    m_wakeRecheckAt.reset();
                    focusDirty = true;
                }
            }
            if (!m_available) {
                for (const auto& r : injections) {
                    emit injectionFinished(r.requestId, InjectionOutcome::NotSupported,
                                           QStringLiteral("Windows UI Automation isn't available."));
                }
                for (const auto& r : rangeInjections) {
                    emit rangeInjectionFinished(r.requestId, InjectionOutcome::NotSupported,
                                                QStringLiteral("Windows UI Automation isn't available."));
                }
                continue;
            }
            if (refreshId) {
                m_emitRefreshId = refreshId;
                m_refreshEchoUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
                m_lastEmittedText.clear();
            }
            if (m_emitRefreshId && std::chrono::steady_clock::now() > m_refreshEchoUntil) m_emitRefreshId = 0;
            if (observingChanged) {
                applyObserving(wantObserving);
            }
            if (m_observing && focusDirty) {
                onFocusChanged();
            }
            const auto now = std::chrono::steady_clock::now();
            if (m_observing && (textDirty || now >= nextPoll)) {
                onFocusChanged(); // Recover focus changes missed by an Electron accessibility rebuild.
                readAndEmit();
            }
            if (now >= nextPoll) {
                nextPoll = now + kPollInterval;
            }
            for (const auto& r : injections) {
                performInjection(r);
            }
            for (const auto& r : rangeInjections) {
                performRangeInjection(r);
            }
        }
        applyObserving(false);
        m_uia.Reset();
        if (m_clipboardOwner != nullptr) DestroyWindow(m_clipboardOwner);
        CoUninitialize();
    }

    void markFocusDirty() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_focusDirty = true;
        }
        m_wake.notify_all();
    }

    void markTextDirty() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_textDirty = true;
        }
        m_wake.notify_all();
    }

    void applyObserving(bool observing) {
        if (observing == m_observing || !m_uia) {
            return;
        }
        if (observing) {
            m_focusSink.Attach(new FocusSink([this](IUIAutomationElement*) { markFocusDirty(); }));
            if (SUCCEEDED(m_uia->AddFocusChangedEventHandler(nullptr, m_focusSink.Get()))) {
                m_observing = true;
                markFocusDirty();
            } else {
                m_focusSink.Reset();
            }
            return;
        }
        detachFieldHandlers();
        if (m_focusSink) {
            m_uia->RemoveFocusChangedEventHandler(m_focusSink.Get());
            m_focusSink.Reset();
        }
        m_observing = false;
        m_focused = Focused{};
        m_capturedFields.clear();
        m_lastEmittedText.clear();
        emit fieldLost();
    }

    void detachFieldHandlers() {
        if (m_focused.element && m_textSink) {
            m_uia->RemoveAutomationEventHandler(UIA_Text_TextChangedEventId, m_focused.element.Get(), m_textSink.Get());
        }
        if (m_focused.element && m_valueSink) {
            m_uia->RemovePropertyChangedEventHandler(m_focused.element.Get(), m_valueSink.Get());
        }
        m_textSink.Reset();
        m_valueSink.Reset();
    }

    void wakeElectron(DWORD pid, HWND root) {
        if (m_wokenPids.count(pid) > 0 || root == nullptr) {
            return;
        }
        FindRenderWidget find;
        EnumChildWindows(root, findRenderWidgetProc, reinterpret_cast<LPARAM>(&find));
        if (find.found == nullptr) find.found = root;
        m_wokenPids.insert(pid);
        // Pattern queries alone don't reliably activate Chromium's renderer accessibility.
        // Request its MSAA and UIA providers explicitly, with a bounded cross-process wait.
        DWORD_PTR result = 0;
        SendMessageTimeoutW(find.found, WM_GETOBJECT, 0,
                            static_cast<LPARAM>(static_cast<LONG>(OBJID_CLIENT)),
                            SMTO_ABORTIFHUNG, 200, &result);
        SendMessageTimeoutW(find.found, WM_GETOBJECT, 0, UiaRootObjectId,
                            SMTO_ABORTIFHUNG, 200, &result);
        // Asking the render widget for a text pattern sends WM_GETOBJECT, which makes Chromium build
        // its accessibility tree. The tree appears a few milliseconds later, so focus is re-read.
        ComPtr<IUIAutomationElement> widget;
        if (SUCCEEDED(m_uia->ElementFromHandle(find.found, widget.GetAddressOf())) && widget) {
            ComPtr<IUnknown> pattern;
            widget->GetCurrentPattern(UIA_TextPatternId, pattern.GetAddressOf());
            widget->GetCurrentPattern(UIA_ValuePatternId, pattern.ReleaseAndGetAddressOf());
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_wakeRecheckAt = std::chrono::steady_clock::now() + kElectronWakeDelay;
    }

    void onFocusChanged() {
        const HWND foreground = GetForegroundWindow();
        DWORD foregroundPid = 0;
        GetWindowThreadProcessId(foreground, &foregroundPid);
        const QString foregroundExe = executableName(foregroundPid);
        if (isElectronHost(foregroundExe)) wakeElectron(foregroundPid, foreground);
        ComPtr<IUIAutomationElement> element;
        if (FAILED(m_uia->GetFocusedElement(element.GetAddressOf())) || !element) {
            return;
        }
        int pidValue = 0;
        if (FAILED(element->get_CurrentProcessId(&pidValue)) || pidValue <= 0) {
            return;
        }
        const DWORD pid = static_cast<DWORD>(pidValue);
        if (pid == GetCurrentProcessId()) {
            return; // Roclint's own windows never change the observed target
        }
        const QString exe = executableName(pid);
        if (isElectronHost(exe)) {
            // A Chromium text leaf can own accessibility focus while its contenteditable ancestor
            // owns the actual edit pattern. Capture that ancestor, never the whole chat document.
            ComPtr<IUIAutomationTreeWalker> walker;
            m_uia->get_ControlViewWalker(walker.GetAddressOf());
            ComPtr<IUIAutomationElement> candidate = element;
            // Some Chromium providers return the web document for UIA focus.
            // Only an active caret can identify a more specific editing surface;
            // never choose an arbitrary descendant by its label or location.
            if (!isGuiEditor(candidate.Get())) {
                ComPtr<IUIAutomationTextPattern2> text;
                ComPtr<IUIAutomationTextRange> caret;
                BOOL active = FALSE;
                if (SUCCEEDED(element->GetCurrentPatternAs(UIA_TextPattern2Id, IID_PPV_ARGS(text.GetAddressOf()))) && text &&
                    SUCCEEDED(text->GetCaretRange(&active, caret.GetAddressOf())) && active && caret) {
                    ComPtr<IUIAutomationElement> enclosing;
                    int enclosingPid = 0;
                    if (SUCCEEDED(caret->GetEnclosingElement(enclosing.GetAddressOf())) && enclosing &&
                        SUCCEEDED(enclosing->get_CurrentProcessId(&enclosingPid)) && enclosingPid == pidValue)
                        candidate = enclosing;
                }
            }
            for (int depth = 0; walker && candidate && depth < 6; ++depth) {
                if (isGuiEditor(candidate.Get())) {
                    element = candidate;
                    break;
                }
                ComPtr<IUIAutomationElement> parent;
                if (FAILED(walker->GetParentElement(candidate.Get(), parent.GetAddressOf()))) break;
                candidate = parent;
            }
        }
        if (m_focused.element) {
            BOOL same = FALSE;
            if (SUCCEEDED(m_uia->CompareElements(m_focused.element.Get(), element.Get(), &same)) && same) {
                if (m_focused.eligible) return;
                if (!isGuiEditor(element.Get())) return;
            }
        }
        detachFieldHandlers();

        Focused next;
        next.element = element;
        next.token = ++m_tokenCounter;
        next.pid = pid;
        next.exe = exe;
        if (!HostContextDetector::isAmbientEligibleApp(next.exe)) {
            m_focused = Focused{};
            m_lastEmittedText.clear();
            emit fieldLost();
            return;
        }
        UIA_HWND hwnd = nullptr;
        element->get_CurrentNativeWindowHandle(&hwnd);
        HWND native = static_cast<HWND>(hwnd);
        HWND fg = GetForegroundWindow();
        DWORD fgPid = 0;
        GetWindowThreadProcessId(fg, &fgPid);
        next.window = native != nullptr ? GetAncestor(native, GA_ROOT) : (fgPid == pid ? fg : nullptr);
        if (next.window == nullptr && isElectronHost(next.exe) &&
            executableName(fgPid).compare(next.exe, Qt::CaseInsensitive) == 0) {
            next.window = fg; // Chromium may expose the renderer's process ID.
        }

        if (isElectronHost(next.exe)) {
            wakeElectron(pid, next.window);
        }

        BSTR cls = nullptr;
        element->get_CurrentClassName(&cls);
        const QString className = fromBstr(cls);
        SysFreeString(cls);
        CONTROLTYPEID type = 0;
        element->get_CurrentControlType(&type);

        next.xterm = className.contains(QLatin1String("xterm-helper-textarea"));
        const bool terminalHost = next.exe.compare(QLatin1String("WindowsTerminal.exe"), Qt::CaseInsensitive) == 0 ||
                                  className == QLatin1String("TermControl");
        next.kind = (terminalHost || next.xterm) ? FieldKind::Terminal : FieldKind::GuiEditor;
        next.eligible = next.xterm || terminalHost || isGuiEditor(element.Get());
        if (qEnvironmentVariableIsSet("ROCLINT_CAPTURE_DIAGNOSTICS")) {
            qInfo().noquote() << QStringLiteral("[Roclint capture] app=%1 token=%2 control=%3 eligible=%4")
                                    .arg(next.exe).arg(next.token).arg(type).arg(next.eligible);
        }

        m_focused = next;
        m_lastEmittedText.clear();
        if (!m_focused.eligible) {
            emit fieldLost();
            return;
        }
        m_textSink.Attach(new TextSink([this](IUIAutomationElement*) { markTextDirty(); }));
        m_uia->AddAutomationEventHandler(UIA_Text_TextChangedEventId, element.Get(), TreeScope_Element, nullptr,
                                         m_textSink.Get());
        m_valueSink.Attach(new ValueSink([this](IUIAutomationElement*) { markTextDirty(); }));
        PROPERTYID props[] = {UIA_ValueValuePropertyId};
        SAFEARRAY* list = SafeArrayCreateVector(VT_I4, 0, 1);
        if (list != nullptr) {
            LONG index = 0;
            SafeArrayPutElement(list, &index, &props[0]);
            m_uia->AddPropertyChangedEventHandler(element.Get(), TreeScope_Element, nullptr, m_valueSink.Get(), list);
            SafeArrayDestroy(list);
        }
        readAndEmit();
    }

    static QRect rectFromArray(SAFEARRAY* array) {
        QRect rect;
        if (array == nullptr) {
            return rect;
        }
        double* values = nullptr;
        if (SUCCEEDED(SafeArrayAccessData(array, reinterpret_cast<void**>(&values)))) {
            LONG upper = 0;
            SafeArrayGetUBound(array, 1, &upper);
            if (upper >= 3) {
                rect = QRect(static_cast<int>(values[0]), static_cast<int>(values[1]),
                             std::max(1, static_cast<int>(values[2])), std::max(1, static_cast<int>(values[3])));
            }
            SafeArrayUnaccessData(array);
        }
        SafeArrayDestroy(array);
        return rect;
    }

    // Reads the xterm.js accessibility rows next to the hidden input textarea.
    QString readXtermBuffer(IUIAutomationElement* textarea) {
        ComPtr<IUIAutomationTreeWalker> walker;
        if (FAILED(m_uia->get_ControlViewWalker(walker.GetAddressOf()))) {
            return {};
        }
        ComPtr<IUIAutomationElement> container;
        walker->GetParentElement(textarea, container.GetAddressOf());
        if (!container) {
            return {};
        }
        VARIANT cls;
        VariantInit(&cls);
        cls.vt = VT_BSTR;
        cls.bstrVal = SysAllocString(L"xterm-accessibility-tree");
        ComPtr<IUIAutomationCondition> condition;
        m_uia->CreatePropertyCondition(UIA_ClassNamePropertyId, cls, condition.GetAddressOf());
        VariantClear(&cls);
        ComPtr<IUIAutomationElement> tree;
        if (condition) {
            container->FindFirst(TreeScope_Descendants, condition.Get(), tree.GetAddressOf());
        }
        if (!tree) {
            return {};
        }
        ComPtr<IUIAutomationElementArray> rows;
        ComPtr<IUIAutomationCondition> all;
        m_uia->CreateTrueCondition(all.GetAddressOf());
        tree->FindAll(TreeScope_Children, all.Get(), rows.GetAddressOf());
        if (!rows) {
            return {};
        }
        int count = 0;
        rows->get_Length(&count);
        QStringList lines;
        for (int i = 0; i < count; ++i) {
            ComPtr<IUIAutomationElement> row;
            if (SUCCEEDED(rows->GetElement(i, row.GetAddressOf())) && row) {
                BSTR name = nullptr;
                row->get_CurrentName(&name);
                lines.append(fromBstr(name));
                SysFreeString(name);
            }
        }
        return lines.join(QLatin1Char('\n'));
    }

    std::optional<ObservedField> readField(const Focused& target) {
        if (!target.element || !target.eligible) {
            return std::nullopt;
        }
        IUIAutomationElement* el = target.element.Get();
        // Recheck before every read and insertion readback: provider capabilities
        // can change while a captured element remains alive.
        if (target.kind == FieldKind::GuiEditor && !isGuiEditor(el)) return std::nullopt;
        int currentPid = 0;
        if (FAILED(el->get_CurrentProcessId(&currentPid)) || currentPid != static_cast<int>(target.pid) ||
            executableName(target.pid).compare(target.exe, Qt::CaseInsensitive) != 0 ||
            !HostContextDetector::isAmbientEligibleApp(target.exe)) return std::nullopt;
        ObservedField f;
        f.fieldToken = target.token;
        f.appIdentifier = target.exe;
        f.appDisplayName = target.exe.endsWith(QLatin1String(".exe"), Qt::CaseInsensitive)
                               ? target.exe.left(target.exe.size() - 4) : target.exe;
        f.processId = static_cast<qint64>(target.pid);
        f.kind = target.kind;

        if (elevationMismatch(target.pid)) {
            f.elevationMismatch = true;
            return f;
        }
        BOOL password = FALSE;
        el->get_CurrentIsPassword(&password);
        f.isPassword = password != FALSE;
        if (f.isPassword) {
            return f;
        }
        RECT bounds{};
        if (SUCCEEDED(el->get_CurrentBoundingRectangle(&bounds)) && bounds.right > bounds.left) {
            f.fieldRect = QRect(bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top);
        }

        int caret = -1;
        if (target.xterm) {
            f.text = readXtermBuffer(el);
        } else {
            bool valueRead = false;
            bool textRangeRead = false;
            ComPtr<IUIAutomationValuePattern> valuePattern;
            if (SUCCEEDED(el->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(valuePattern.GetAddressOf()))) && valuePattern) {
                BOOL readOnly = TRUE;
                if (SUCCEEDED(valuePattern->get_CurrentIsReadOnly(&readOnly)) && !readOnly) {
                    BSTR value = nullptr;
                    if (SUCCEEDED(valuePattern->get_CurrentValue(&value))) {
                        f.text = fromBstr(value);
                        valueRead = true;
                    }
                    SysFreeString(value);
                }
            }
            ComPtr<IUIAutomationTextPattern> text;
            if (SUCCEEDED(el->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(text.GetAddressOf()))) && text) {
                ComPtr<IUIAutomationTextRange> document;
                if (SUCCEEDED(text->get_DocumentRange(document.GetAddressOf())) && document) {
                    VARIANT readOnly;
                    VariantInit(&readOnly);
                    const HRESULT attr = document->GetAttributeValue(UIA_IsReadOnlyAttributeId, &readOnly);
                    const bool immutable = SUCCEEDED(attr) && readOnly.vt == VT_BOOL && readOnly.boolVal;
                    VariantClear(&readOnly);
                    if (immutable && f.kind != FieldKind::Terminal) return std::nullopt;
                    // ValuePattern exposes editor text without TextPattern's synthetic paragraph
                    // terminator. Still use TextPattern for caret position and bounds.
                    // Chromium combobox composers can expose an empty ValuePattern
                    // while their writable text range contains the current draft.
                    // This range belongs to the validated editor, never a page root.
                    if (!valueRead || f.text.isEmpty()) {
                        BSTR value = nullptr;
                        if (SUCCEEDED(document->GetText(kMaxReadChars + 1, &value))) {
                            f.text = fromBstr(value);
                            valueRead = true;
                            textRangeRead = true;
                        }
                        SysFreeString(value);
                    }
                    if (f.text.size() > kMaxReadChars) return std::nullopt;
                    readCaret(el, document.Get(), f, caret);
                }
            }
            CONTROLTYPEID type = 0;
            el->get_CurrentControlType(&type);
            if (type == UIA_ComboBoxControlTypeId && f.text.isEmpty()) {
                ComPtr<IUIAutomationLegacyIAccessiblePattern> legacy;
                if (SUCCEEDED(el->GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId,
                                                     IID_PPV_ARGS(legacy.GetAddressOf()))) && legacy) {
                    BSTR value = nullptr;
                    if (SUCCEEDED(legacy->get_CurrentValue(&value))) {
                        const auto legacyText = fromBstr(value);
                        if (!legacyText.isEmpty()) { f.text = legacyText; valueRead = true; }
                    }
                    SysFreeString(value);
                }
                // Some composite providers expose Value="" even for a nonempty
                // visible draft. Without a readable text range this cannot prove
                // that the destination is empty. Refuse capture/insertion rather
                // than authorizing replacement against a falsely empty value.
            }
            const auto role = type == UIA_ComboBoxControlTypeId ? GuiCaptureRole::ComboBox : GuiCaptureRole::Edit;
            if (!isGuiCaptureTextReliable(role, valueRead, f.text.isEmpty(), textRangeRead,
                                          isElectronHost(target.exe)) ||
                f.text.size() > kMaxReadChars) return std::nullopt;
        }
        normalizeText(f.text, caret);
        f.caretOffset = caret >= 0 ? std::min(caret, static_cast<int>(f.text.size())) : -1;
        return f;
    }

    void readCaret(IUIAutomationElement* el, IUIAutomationTextRange* document, ObservedField& f, int& caret) {
        ComPtr<IUIAutomationTextPattern2> text2;
        if (FAILED(el->GetCurrentPatternAs(UIA_TextPattern2Id, IID_PPV_ARGS(text2.GetAddressOf()))) || !text2) {
            return;
        }
        BOOL active = FALSE;
        ComPtr<IUIAutomationTextRange> caretRange;
        if (FAILED(text2->GetCaretRange(&active, caretRange.GetAddressOf())) || !caretRange) {
            return;
        }
        SAFEARRAY* rects = nullptr;
        if (SUCCEEDED(caretRange->GetBoundingRectangles(&rects))) {
            f.caretRect = rectFromArray(rects);
        }
        if (f.caretRect.isNull()) {
            ComPtr<IUIAutomationTextRange> expanded;
            if (SUCCEEDED(caretRange->Clone(expanded.GetAddressOf())) && expanded &&
                SUCCEEDED(expanded->ExpandToEnclosingUnit(TextUnit_Character)) &&
                SUCCEEDED(expanded->GetBoundingRectangles(&rects))) {
                f.caretRect = rectFromArray(rects);
            }
        }
        ComPtr<IUIAutomationTextRange> before;
        if (SUCCEEDED(document->Clone(before.GetAddressOf())) && before &&
            SUCCEEDED(before->MoveEndpointByRange(TextPatternRangeEndpoint_End, caretRange.Get(),
                                                  TextPatternRangeEndpoint_Start))) {
            BSTR prefix = nullptr;
            if (SUCCEEDED(before->GetText(kMaxReadChars, &prefix))) {
                caret = static_cast<int>(SysStringLen(prefix));
            }
            SysFreeString(prefix);
        }
    }

    void readAndEmit() {
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        if (pid == GetCurrentProcessId()) return; // Keep the target, without observing behind the HUD.
        auto field = readField(m_focused);
        if (!field.has_value()) {
            if (qEnvironmentVariableIsSet("ROCLINT_CAPTURE_DIAGNOSTICS") && m_failedReadToken != m_focused.token) {
                qInfo() << "[Roclint capture] No writable text provider for token" << m_focused.token;
                m_failedReadToken = m_focused.token;
            }
            emit fieldLost();
            return;
        }
        // A focus change must not discard the COM identity that an open HUD captured.
        // Keep a bounded set of readable targets; never retain captured text here.
        if (!field->isPassword && !field->elevationMismatch) {
            m_capturedFields.insert_or_assign(m_focused.token, m_focused);
            while (m_capturedFields.size() > 32) m_capturedFields.erase(m_capturedFields.begin());
        }
        const QString signature = field->text + QChar(0x1F) + QString::number(field->caretOffset) + QChar(0x1F) +
                                  QString::number(field->fieldToken);
        if (signature == m_lastEmittedText && !m_emitRefreshId) {
            return;
        }
        m_lastEmittedText = signature;
        field->refreshRequestId = m_emitRefreshId;
        if (qEnvironmentVariableIsSet("ROCLINT_CAPTURE_DIAGNOSTICS")) {
            // Metadata only: never log captured prompt content or selections.
            qInfo().noquote() << QStringLiteral("[Roclint capture] observed app=%1 token=%2 chars=%3 password=%4 elevation_mismatch=%5")
                                    .arg(field->appIdentifier).arg(field->fieldToken).arg(field->text.size())
                                    .arg(field->isPassword).arg(field->elevationMismatch);
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

    bool focusTarget(const Focused& target) {
        if (target.window == nullptr || !IsWindow(target.window)) return false;
        if (GetForegroundWindow() != target.window) {
            SetForegroundWindow(target.window);
            Sleep(50);
        }
        if (target.element) {
            // Some rich edit providers reject SetFocus even though restoring their window
            // already restored the correct child. Verify the actual focus below either way.
            target.element->SetFocus();
            Sleep(30);
        }
        ComPtr<IUIAutomationElement> focused;
        if (GetForegroundWindow() != target.window ||
            FAILED(m_uia->GetFocusedElement(focused.GetAddressOf())) || !focused) return false;
        ComPtr<IUIAutomationTreeWalker> walker;
        m_uia->get_ControlViewWalker(walker.GetAddressOf());
        for (int depth = 0; focused && depth < 6; ++depth) {
            BOOL same = FALSE;
            if (SUCCEEDED(m_uia->CompareElements(target.element.Get(), focused.Get(), &same)) && same) return true;
            if (!walker) break;
            ComPtr<IUIAutomationElement> parent;
            if (FAILED(walker->GetParentElement(focused.Get(), parent.GetAddressOf()))) break;
            focused = parent;
        }
        return false;
    }

    bool pasteText(const QString& textToPaste, const QString& expectedFinalText,
                   std::initializer_list<WORD> pasteModifiers, const Focused& target) {
        const auto saved = ClipboardText::read();
        if (!ClipboardText::write(textToPaste.toStdWString(), m_clipboardOwner)) {
            return false;
        }
        const DWORD clipboardSequence = GetClipboardSequenceNumber();
        const bool sent = GetForegroundWindow() == target.window && sendChord(pasteModifiers, 'V');
        bool verified = false;
        qsizetype observedChars = -1;
        bool observedTrailingLineBreak = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1200);
        // Chromium can deliver the paste and expose its updated value on later frames.
        // Keep our clipboard payload available until readback, then restore the user's text.
        while (sent && std::chrono::steady_clock::now() < deadline) {
            const auto after = readField(target);
            if (after) {
                const auto actual = currentDraft(*after);
                observedChars = actual.size();
                observedTrailingLineBreak = actual.endsWith(QLatin1Char('\n'));
            }
            if (after && !after->isPassword && !after->elevationMismatch && currentDraft(*after) == expectedFinalText) {
                verified = true; break;
            }
            Sleep(25);
        }
        if (saved.has_value() && GetClipboardSequenceNumber() == clipboardSequence) {
            ClipboardText::write(*saved, m_clipboardOwner);
        }
        if (!verified && qEnvironmentVariableIsSet("ROCLINT_CAPTURE_DIAGNOSTICS"))
            qInfo() << "[Roclint readback] token=" << target.token << "sent=" << sent
                    << "expected_chars=" << expectedFinalText.size() << "observed_chars=" << observedChars
                    << "expected_final_lf=" << expectedFinalText.endsWith(QLatin1Char('\n')) << "observed_final_lf=" << observedTrailingLineBreak;
        return verified;
    }

    bool pasteText(const QString& text, std::initializer_list<WORD> pasteModifiers, const Focused& target) {
        return pasteText(text, text, pasteModifiers, target);
    }

    void finish(const InjectionRequest& r, InjectionOutcome outcome, const QString& detail) {
        if (qEnvironmentVariableIsSet("ROCLINT_CAPTURE_DIAGNOSTICS"))
            qInfo() << "[Roclint insertion] token=" << r.fieldToken << "outcome=" << toString(outcome) << "detail=" << detail;
        emit injectionFinished(r.requestId, outcome, detail);
    }

    void finishRange(const RangeInjectionRequest& r, InjectionOutcome outcome, const QString& detail) {
        if (qEnvironmentVariableIsSet("ROCLINT_CAPTURE_DIAGNOSTICS"))
            qInfo() << "[Roclint range insertion] token=" << r.fieldToken << "outcome=" << toString(outcome) << "detail=" << detail;
        emit rangeInjectionFinished(r.requestId, outcome, detail);
    }

    void performInjection(const InjectionRequest& r) {
        const auto saved = m_capturedFields.find(r.fieldToken);
        if (saved == m_capturedFields.end() || !saved->second.element || saved->second.kind != r.kind) {
            finish(r, InjectionOutcome::FieldChanged, QStringLiteral("The captured input field expired. Focus your prompt and open Roclint again."));
            return;
        }
        const Focused target = saved->second;
        auto before = readField(target);
        if (!before.has_value() || before->elevationMismatch || before->isPassword) {
            finish(r, InjectionOutcome::NotSupported, QStringLiteral("That field can't be written to."));
            return;
        }
        if (currentDraft(*before) != r.expectedCurrentText) {
            finish(r, InjectionOutcome::FieldChanged, QStringLiteral("The text changed after Roclint captured it."));
            return;
        }

        if (r.kind == FieldKind::Terminal) {
            if (!focusTarget(target)) {
                finish(r, InjectionOutcome::Failed, QStringLiteral("Couldn't bring the terminal to the front."));
                return;
            }
            bool pasted = true;
            for (const auto key : TerminalAdapter::replacementKeys(r.terminalVimMode)) {
                switch (key) {
                    case TerminalAdapter::Key::Escape: sendChord({}, VK_ESCAPE); break;
                    case TerminalAdapter::Key::AppendAtLineEnd: sendChord({VK_SHIFT}, 'A'); break;
                    case TerminalAdapter::Key::LineEnd: sendChord({VK_CONTROL}, 'E'); break;
                    case TerminalAdapter::Key::ClearLine: sendChord({VK_CONTROL}, 'U'); break;
                    case TerminalAdapter::Key::Paste: pasted = pasteText(r.text, {VK_CONTROL}, target); break;
                }
            }
            Sleep(kTerminalEchoMs);
            const auto after = readField(target);
            if (pasted && after.has_value() && currentDraft(*after) == r.text) {
                finish(r, InjectionOutcome::Injected, QString());
            } else {
                finish(r, InjectionOutcome::Failed, QStringLiteral("The terminal didn't show the new text."));
            }
            return;
        }

        if (!focusTarget(target)) {
            finish(r, InjectionOutcome::Failed, QStringLiteral("Couldn't focus the captured input field."));
            return;
        }
        const auto focusedBefore = readField(target);
        if (!focusedBefore || focusedBefore->text != r.expectedCurrentText) {
            finish(r, InjectionOutcome::FieldChanged, QStringLiteral("The text changed while restoring focus."));
            return;
        }
        ComPtr<IUIAutomationValuePattern> value;
        if (!isElectronHost(target.exe) &&
            SUCCEEDED(target.element->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(value.GetAddressOf()))) &&
            value) {
            BOOL readOnly = TRUE;
            value->get_CurrentIsReadOnly(&readOnly);
            if (!readOnly) {
                BSTR text = SysAllocStringLen(reinterpret_cast<const OLECHAR*>(r.text.utf16()),
                                              static_cast<UINT>(r.text.size()));
                const HRESULT hr = value->SetValue(text);
                SysFreeString(text);
                const auto after = readField(target);
                if (SUCCEEDED(hr) && after.has_value() && after->text == r.text) {
                    finish(r, InjectionOutcome::Injected, QString());
                    return;
                }
                if (!after || after->text != r.expectedCurrentText) {
                    finish(r, InjectionOutcome::Failed, QStringLiteral("The editor changed the replacement text."));
                    return;
                }
            }
        }
        // Rich editors (Chromium contenteditable, Monaco) usually reject SetValue: select all and paste.
        if (!focusTarget(target)) {
            finish(r, InjectionOutcome::Failed, QStringLiteral("Couldn't bring the app to the front."));
            return;
        }
        if (!sendChord({VK_CONTROL}, 'A')) {
            finish(r, InjectionOutcome::Failed, QStringLiteral("Windows refused the selection shortcut."));
            return;
        }
        const bool pasted = pasteText(r.text, {VK_CONTROL}, target);
        if (pasted) {
            finish(r, InjectionOutcome::Injected, QString());
        } else {
            finish(r, InjectionOutcome::Failed, QStringLiteral("The editor did not confirm the exact replacement. Check whether it is already present before pasting the recovery copy."));
        }
    }

    // Bounded read of an automation text range. Returns nullopt when the provider cannot
    // produce text, so callers can abort instead of treating an empty selection as a match.
    std::optional<QString> readRangeText(IUIAutomationTextRange* range) {
        if (range == nullptr) return std::nullopt;
        BSTR text = nullptr;
        const HRESULT hr = range->GetText(16384, &text);
        if (FAILED(hr) || text == nullptr) {
            if (text) SysFreeString(text);
            return std::nullopt;
        }
        const QString value = QString::fromWCharArray(text, static_cast<int>(SysStringLen(text)));
        SysFreeString(text);
        return value;
    }

    void performRangeInjection(const RangeInjectionRequest& r) {
        if (r.kind == FieldKind::Terminal) {
            finishRange(r, InjectionOutcome::NotSupported,
                        QStringLiteral("Inline word replacement isn't available in terminal buffers. Explore in the companion instead."));
            return;
        }
        const auto saved = m_capturedFields.find(r.fieldToken);
        if (saved == m_capturedFields.end() || !saved->second.element || saved->second.kind != r.kind) {
            finishRange(r, InjectionOutcome::FieldChanged, QStringLiteral("The captured input field expired. Focus your prompt and open Roclint again."));
            return;
        }
        const Focused target = saved->second;
        auto before = readField(target);
        if (!before.has_value() || before->elevationMismatch || before->isPassword) {
            finishRange(r, InjectionOutcome::NotSupported, QStringLiteral("That field can't be written to."));
            return;
        }
        if (!RangeInjectionPolicy::preconditionsHold(r.fieldToken, before->fieldToken,
                                                     r.expectedCurrentText, currentDraft(*before))) {
            finishRange(r, InjectionOutcome::FieldChanged, QStringLiteral("The text or field changed after Roclint captured it."));
            return;
        }
        if (!RangeInjectionPolicy::offsetsValid(r.expectedCurrentText, r.startOffset, r.endOffset)) {
            finishRange(r, InjectionOutcome::FieldChanged, QStringLiteral("Invalid text range offsets."));
            return;
        }
        if (!RangeInjectionPolicy::rangeMatches(r.expectedCurrentText, r.startOffset, r.endOffset,
                                                r.originalRangeText)) {
            finishRange(r, InjectionOutcome::FieldChanged, QStringLiteral("The target text range changed."));
            return;
        }

        const QString newFullText = r.expectedCurrentText.left(r.startOffset) + r.replacementText + r.expectedCurrentText.mid(r.endOffset);

        if (!focusTarget(target)) {
            finishRange(r, InjectionOutcome::Failed, QStringLiteral("Couldn't focus the captured input field."));
            return;
        }
        const auto focusedBefore = readField(target);
        if (!focusedBefore || !RangeInjectionPolicy::preconditionsHold(r.fieldToken, focusedBefore->fieldToken,
                                                                      r.expectedCurrentText, currentDraft(*focusedBefore))) {
            finishRange(r, InjectionOutcome::FieldChanged, QStringLiteral("The text changed while restoring focus."));
            return;
        }

        auto abortIfExternallyChanged = [&]() -> bool {
            const auto now = readField(target);
            if (!now || now->elevationMismatch || now->isPassword) {
                finishRange(r, InjectionOutcome::FieldChanged, QStringLiteral("The field disappeared or became unreadable."));
                return true;
            }
            if (currentDraft(*now) != r.expectedCurrentText) {
                finishRange(r, InjectionOutcome::FieldChanged,
                            QStringLiteral("The editor changed while the replacement was being applied. Nothing else was written."));
                return true;
            }
            return false;
        };

        // ValuePattern approach (Notepad, WordPad, standard edit control)
        ComPtr<IUIAutomationValuePattern> value;
        if (!isElectronHost(target.exe) &&
            SUCCEEDED(target.element->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(value.GetAddressOf()))) &&
            value) {
            BOOL readOnly = TRUE;
            value->get_CurrentIsReadOnly(&readOnly);
            if (!readOnly) {
                BSTR text = SysAllocStringLen(reinterpret_cast<const OLECHAR*>(newFullText.utf16()),
                                              static_cast<UINT>(newFullText.size()));
                const HRESULT hr = value->SetValue(text);
                SysFreeString(text);
                const auto after = readField(target);
                if (SUCCEEDED(hr) && after.has_value() && currentDraft(*after) == newFullText) {
                    finishRange(r, InjectionOutcome::Injected, QString());
                    return;
                }
                if (!after || currentDraft(*after) != r.expectedCurrentText) {
                    // A failed SetValue may have partially written text. Never paste a snapshot
                    // over it as a recovery mechanism.
                    finishRange(r, InjectionOutcome::FieldChanged,
                                QStringLiteral("The editor changed the text during a write. Review it before retrying."));
                    return;
                }
            }
        }

        // TextPattern / selection approach (Electron, Monaco, rich text editors)
        bool selectedRange = false;
        ComPtr<IUIAutomationTextPattern> textPattern;
        if (SUCCEEDED(target.element->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(textPattern.GetAddressOf()))) && textPattern) {
            ComPtr<IUIAutomationTextRange> document;
            if (SUCCEEDED(textPattern->get_DocumentRange(document.GetAddressOf())) && document) {
                ComPtr<IUIAutomationTextRange> range;
                if (SUCCEEDED(document->Clone(range.GetAddressOf())) && range) {
                    bool movementOk = true;
                    HRESULT hr = range->MoveEndpointByRange(TextPatternRangeEndpoint_End, range.Get(), TextPatternRangeEndpoint_Start);
                    movementOk = movementOk && SUCCEEDED(hr);
                    int moved = 0;
                    if (movementOk && r.startOffset > 0) {
                        hr = range->MoveEndpointByUnit(TextPatternRangeEndpoint_Start, TextUnit_Character, r.startOffset, &moved);
                        movementOk = movementOk && SUCCEEDED(hr) && moved == r.startOffset;
                    }
                    if (movementOk) {
                        hr = range->MoveEndpointByRange(TextPatternRangeEndpoint_End, range.Get(), TextPatternRangeEndpoint_Start);
                        movementOk = movementOk && SUCCEEDED(hr);
                    }
                    if (movementOk && r.endOffset > r.startOffset) {
                        moved = 0;
                        hr = range->MoveEndpointByUnit(TextPatternRangeEndpoint_End, TextUnit_Character,
                                                       r.endOffset - r.startOffset, &moved);
                        movementOk = movementOk && SUCCEEDED(hr) && moved == (r.endOffset - r.startOffset);
                    }
                    if (movementOk) {
                        // The range text must match the text the host validated before any write.
                        const auto actualRangeText = readRangeText(range.Get());
                        if (!actualRangeText.has_value() || *actualRangeText != r.originalRangeText) {
                            finishRange(r, InjectionOutcome::FieldChanged,
                                        QStringLiteral("The editor reports different text at that range. Nothing was written."));
                            return;
                        }
                        if (SUCCEEDED(range->Select())) {
                            selectedRange = true;
                        }
                    }
                }
            }
        }

        if (selectedRange && textPattern) {
            // Verify the provider actually selected the expected text before sending a paste.
            ComPtr<IUIAutomationTextRangeArray> selection;
            bool exactSelection = false;
            if (SUCCEEDED(textPattern->GetSelection(selection.GetAddressOf())) && selection) {
                int count = 0;
                selection->get_Length(&count);
                if (count == 1) {
                    ComPtr<IUIAutomationTextRange> selected;
                    if (SUCCEEDED(selection->GetElement(0, selected.GetAddressOf())) && selected) {
                        const auto selectedText = readRangeText(selected.Get());
                        exactSelection = selectedText.has_value() && *selectedText == r.originalRangeText;
                    }
                }
            }
            if (!exactSelection) {
                finishRange(r, InjectionOutcome::FieldChanged,
                            QStringLiteral("The editor didn't select the expected text. Nothing was written."));
                return;
            }
            const bool pasted = pasteText(r.replacementText, newFullText, {VK_CONTROL}, target);
            if (pasted) {
                finishRange(r, InjectionOutcome::Injected, QString());
                return;
            }
        }

        // The full-field fallback is permitted only when the exact preconditions still hold:
        // the same field is focused, it still contains the frozen pre-write text, and the
        // range path performed no write at all.
        if (abortIfExternallyChanged()) return;
        if (!focusTarget(target)) {
            finishRange(r, InjectionOutcome::Failed, QStringLiteral("Couldn't bring the app to the front."));
            return;
        }
        if (abortIfExternallyChanged()) return;
        if (!sendChord({VK_CONTROL}, 'A')) {
            finishRange(r, InjectionOutcome::Failed, QStringLiteral("Windows refused the selection shortcut."));
            return;
        }
        // Confirm the selection shortcut selected the whole current field before pasting.
        if (textPattern) {
            ComPtr<IUIAutomationTextRangeArray> selection;
            int count = 0;
            bool selectedWholeField = false;
            if (SUCCEEDED(textPattern->GetSelection(selection.GetAddressOf())) && selection) {
                selection->get_Length(&count);
                if (count == 1) {
                    ComPtr<IUIAutomationTextRange> selected;
                    if (SUCCEEDED(selection->GetElement(0, selected.GetAddressOf())) && selected) {
                        const auto selectedText = readRangeText(selected.Get());
                        selectedWholeField = selectedText.has_value() && *selectedText == r.expectedCurrentText;
                    }
                }
            }
            if (!selectedWholeField) {
                finishRange(r, InjectionOutcome::FieldChanged,
                            QStringLiteral("The editor didn't select the whole field. Nothing was pasted."));
                return;
            }
        }
        const bool pasted = pasteText(newFullText, newFullText, {VK_CONTROL}, target);
        if (pasted) {
            finishRange(r, InjectionOutcome::Injected, QString());
        } else {
            finishRange(r, InjectionOutcome::Failed, QStringLiteral("The editor did not confirm the exact replacement."));
        }
    }

    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    bool m_stop{false};
    bool m_wantObserving{false};
    bool m_observingChanged{false};
    bool m_focusDirty{false};
    bool m_textDirty{false};
    quint64 m_requestedRefreshId{0};
    std::optional<std::chrono::steady_clock::time_point> m_wakeRecheckAt;
    std::deque<InjectionRequest> m_injections;
    std::deque<RangeInjectionRequest> m_rangeInjections;
    std::atomic<bool> m_available{false};

    // Worker-thread state.
    ComPtr<IUIAutomation> m_uia;
    HWND m_clipboardOwner{nullptr};
    ComPtr<IUIAutomationFocusChangedEventHandler> m_focusSink;
    ComPtr<IUIAutomationEventHandler> m_textSink;
    ComPtr<IUIAutomationPropertyChangedEventHandler> m_valueSink;
    bool m_observing{false};
    Focused m_focused;
    std::map<quint64, Focused> m_capturedFields;
    quint64 m_tokenCounter{0};
    quint64 m_failedReadToken{0};
    quint64 m_emitRefreshId{0};
    std::chrono::steady_clock::time_point m_refreshEchoUntil{};
    std::set<DWORD> m_wokenPids;
    QString m_lastEmittedText;
};

} // namespace

std::unique_ptr<IAccessibilityBridge> createPlatformAccessibilityBridge() {
    return std::make_unique<WinAccessibilityBridge>();
}

} // namespace roclint::host

#endif
