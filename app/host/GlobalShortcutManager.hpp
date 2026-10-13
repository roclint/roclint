#pragma once

#include <QObject>
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

signals:
    void activated();

private:
    HostController* m_controller{nullptr};
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace roclint::host
