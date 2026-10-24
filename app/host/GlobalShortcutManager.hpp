#pragma once

#include <QObject>
#include <QString>
#include <QByteArray>
#include <memory>

namespace roclint::host {

class HostController;

class GlobalShortcutManager : public QObject {
    Q_OBJECT

public:
    explicit GlobalShortcutManager(HostController* controller, QObject* parent = nullptr);
    ~GlobalShortcutManager() override;

    bool registerShortcut();
    void unregisterShortcut();
    // One modified letter/digit/function key/Space. Empty means invalid or unsupported.
    [[nodiscard]] static QString normalizeShortcut(const QString& sequence);
#ifdef ROCLINT_ENABLE_TEST_MOCKS
    bool dispatchNativeHotkeyForTesting(const QByteArray& eventType, quint32 messageId, quintptr id);
#endif

signals:
    void activated();

private:
    bool registerSequence(const QString& sequence);
    void changeShortcut(const QString& sequence);
    QString m_registeredSequence;
    HostController* m_controller{nullptr};
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace roclint::host
