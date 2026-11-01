#pragma once

#include <Plasma/Applet>
#include <QDBusContext>
#include <QDBusServiceWatcher>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QVariantMap>
#include <notifications.h>

// Intentionally runs in plasmashell, with the same QML engine and original
// popup policy model. It owns presentation only, never the notification name.
class NotificationPresenter : public Plasma::Applet, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.caelestia.PlasmaNotifications")
    Q_PROPERTY(bool leased READ leased NOTIFY leasedChanged)

public:
    NotificationPresenter(QObject* parent, const KPluginMetaData& data, const QVariantList& args);
    ~NotificationPresenter() override;
    bool leased() const { return !m_owner.isEmpty(); }
    Q_INVOKABLE void attach(QObject* globals, QObject* history);
    Q_INVOKABLE void detach();
    Q_INVOKABLE void checkPresentation();
    Q_INVOKABLE void policyUpdated();

public Q_SLOTS:
    Q_SCRIPTABLE QVariantMap Snapshot();
    Q_SCRIPTABLE bool Acquire();
    Q_SCRIPTABLE bool Release();
    Q_SCRIPTABLE bool Present(uint id, bool playSound);
    Q_SCRIPTABLE bool Expire(uint id);
    Q_SCRIPTABLE bool Dismiss(uint id);
    Q_SCRIPTABLE QVariantMap Describe(uint id);
    Q_SCRIPTABLE QVariantList Existing();
    Q_SCRIPTABLE bool Eligible(uint id);

Q_SIGNALS:
    void leasedChanged();
    Q_SCRIPTABLE void PolicyChanged();

private:
    QModelIndex find(NotificationManager::Notifications* model, uint id) const;
    NotificationManager::Notifications* historyModel() const;
    bool supported() const;
    bool isOwner() const;
    bool releaseLease();
    void watchdog();
    QVariantMap describe(const QModelIndex& index);

    QPointer<QObject> m_globals;
    QPointer<QObject> m_instantiator;
    QPointer<NotificationManager::Notifications> m_popupModel;
    QPointer<NotificationManager::Notifications> m_historyModel;
    QDBusServiceWatcher m_watcher;
    QTimer m_watchdog;
    QSet<uint> m_presented;
    QString m_owner;
    QString m_error;
    bool m_registered = false;
    bool m_previousActive = true;
    bool m_changing = false;
};
