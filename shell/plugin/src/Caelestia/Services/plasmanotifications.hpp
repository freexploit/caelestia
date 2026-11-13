#pragma once

#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusMessage>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

class QDBusServiceWatcher;

namespace caelestia::services {

// Plasma retains org.freedesktop.Notifications. This object only exports the
// watcher interface on a separate, stable connection, also used for the scoped
// presenter lease. Disconnecting that connection releases the lease on crashes.
class PlasmaNotifications : public QObject, protected QDBusContext {
    Q_OBJECT
    QML_ELEMENT
    Q_CLASSINFO("D-Bus Interface", "org.kde.NotificationWatcher")
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY stateChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged)
    Q_PROPERTY(bool active READ active NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap policySnapshot READ policySnapshot NOTIFY stateChanged)

public:
    explicit PlasmaNotifications(QObject* parent = nullptr);
    ~PlasmaNotifications() override;

    bool enabled() const { return m_enabled; }
    bool ready() const { return m_ready; }
    bool active() const { return m_active; }
    QString error() const { return m_error; }
    QVariantMap policySnapshot() const { return m_policy; }
    void setEnabled(bool enabled);

    Q_INVOKABLE bool setActive(bool active);
    Q_INVOKABLE bool present(uint id);
    Q_INVOKABLE void expire(uint id);
    Q_INVOKABLE bool invokeAction(uint id, const QString& action);
    Q_INVOKABLE bool dismiss(uint id);
    Q_INVOKABLE bool eligible(uint id);

public slots:
    Q_SCRIPTABLE void Notify(uint id, const QString& appName, uint replacesId,
                             const QString& appIcon, const QString& summary,
                             const QString& body, const QStringList& actions,
                             const QVariantMap& hints, int timeout);
    Q_SCRIPTABLE void CloseNotification(uint id);

signals:
    void stateChanged();
    void notificationReceived(const QVariantMap& notification);
    void notificationClosed(uint id, uint reason);
    void notificationPolicyChanged();
    void invalidated();

private slots:
    void onNotificationClosed(uint id, uint reason);
    void onOwnerChanged(const QString& name, const QString& oldOwner, const QString& newOwner);
    void onPresenterPolicyChanged();

private:
    struct Entry {
        QSet<QString> actions;
        bool replayed = false;
        bool resident = false;
    };

    void start();
    void stop();
    void fail(const QString& error);
    bool refreshPolicy();
    QDBusMessage presenterCall(const QString& method, const QList<QVariant>& arguments = {});
    QString notificationImage(const QVariantMap& hints, uint id);
    bool deliver(QVariantMap notification);

    QDBusConnection m_bus;
    QDBusServiceWatcher* m_watcher = nullptr;
    QString m_connectionName;
    QString m_serverOwner;
    QString m_presenterOwner;
    QString m_error;
    QVariantMap m_policy;
    QHash<uint, Entry> m_entries;
    bool m_enabled = false;
    bool m_ready = false;
    bool m_active = false;
    bool m_registered = false;
    quint64 m_imageSerial = 0;
};

} // namespace caelestia::services
