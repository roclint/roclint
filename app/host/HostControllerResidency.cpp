#include "HostController.hpp"
#include "GraniteResidencyPolicy.hpp"

#include <QCoreApplication>
#include <QSettings>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace roclint::host {

// Poll the OS notification without blocking Qt or allocating pressure-test memory.
class MemoryPressureMonitor {
public:
#ifdef Q_OS_WIN
    MemoryPressureMonitor() : handle_(CreateMemoryResourceNotification(LowMemoryResourceNotification)) {}
    ~MemoryPressureMonitor() { if (handle_) CloseHandle(handle_); }
    std::optional<bool> low() const {
        BOOL pressure = FALSE;
        if (!handle_ || !QueryMemoryResourceNotification(handle_, &pressure)) return std::nullopt;
        return pressure != FALSE;
    }
private:
    HANDLE handle_{nullptr};
#else
    std::optional<bool> low() const { return std::nullopt; }
#endif
};

void HostController::initializeResidencyPolicy() {
    if (!QCoreApplication::organizationName().isEmpty()) {
        QSettings settings;
        const auto preset = settings.value(QStringLiteral("models/residencyPreset"), m_graniteResidencyPreset).toString();
        if (GraniteResidencyPolicyV1::validPreset(preset.toStdString())) m_graniteResidencyPreset = preset;
    }
    m_memoryPressure = std::make_shared<MemoryPressureMonitor>();
    m_nativeLastUse.start();
    m_residencyTimer.setInterval(GraniteResidencyPolicyV1::kPollMs);
    connect(&m_residencyTimer, &QTimer::timeout, this, &HostController::applyResidencyPolicy);
    if (QCoreApplication::instance()) m_residencyTimer.start();
    applyResidencyPolicy();
}

bool HostController::setGraniteResidencyPreset(const QString& preset) {
    if (!GraniteResidencyPolicyV1::validPreset(preset.toStdString())) {
        setLastError(core::makeError(core::ErrorCode::InvalidInput, "Choose Keep warm, Balanced or Low memory."));
        return false;
    }
    if (preset == m_graniteResidencyPreset) return true;
    m_graniteResidencyPreset = preset;
    if (!QCoreApplication::organizationName().isEmpty()) {
        QSettings settings; settings.setValue(QStringLiteral("models/residencyPreset"), preset);
    }
    emit modelEngineStatusChanged();
    applyResidencyPolicy();
    return true;
}

void HostController::applyResidencyPolicy() {
    if (m_hostState == HostState::Quitting) { m_residencyTimer.stop(); return; }
    auto pressure = m_memoryPressure ? m_memoryPressure->low() : std::nullopt;
#ifdef ROCLINT_ENABLE_TEST_MOCKS
    if (m_residencyPressureOverride) pressure = m_residencyPressureOverride;
#endif
    const auto detail = !pressure ? QStringLiteral("Idle policy active; OS memory-pressure monitoring is unavailable.") :
        *pressure ? QStringLiteral("Windows reports low physical memory. Unused Granite resources will be released after active work finishes.") :
                    QStringLiteral("Idle policy active; Windows memory-pressure monitoring is available.");
    if (detail != m_graniteResidencyStatus) { m_graniteResidencyStatus = detail; emit modelEngineStatusChanged(); }
    if (!nativeGraniteSelected() || usesTransportOverride() || !m_native->isRunning() || !m_nativeLastUse.isValid()) return;
    const bool busy = m_nativeWarming || m_nativeSemanticInFlight > 0 || m_inFlight > 0 ||
        m_checkingClarification || m_reviewState == ReviewState::Running ||
        m_discoveryQueued || m_pendingNativeRewrite.has_value() || m_isDownloadingModel ||
        m_native->state() == QLatin1String("LOADING") || m_native->state() == QLatin1String("BUSY") ||
        m_native->state() == QLatin1String("STOPPING") || m_native->state() == QLatin1String("FAILED") ||
        m_native->state() == QLatin1String("RECOVERING");
    auto idleMs = m_nativeLastUse.elapsed();
#ifdef ROCLINT_ENABLE_TEST_MOCKS
    if (m_residencyIdleOverride) idleMs = *m_residencyIdleOverride;
#endif
    const auto tier = GraniteResidencyPolicyV1::desiredTier(m_graniteResidencyPreset.toStdString(),
        m_native->residency().toStdString(), idleMs, busy, pressure.value_or(false));
    if (tier) m_native->setResidency(QString::fromLatin1(tier->data(), static_cast<qsizetype>(tier->size())));
}

} // namespace roclint::host
