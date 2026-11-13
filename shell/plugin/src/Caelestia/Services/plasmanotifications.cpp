#include "plasmanotifications.hpp"

#include <QDBusArgument>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QDir>
#include <QImage>
#include <QLoggingCategory>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QUuid>

namespace caelestia::services {
namespace {
const QString serverName = QStringLiteral("org.freedesktop.Notifications");
const QString serverPath = QStringLiteral("/org/freedesktop/Notifications");
const QString managerInterface = QStringLiteral("org.kde.NotificationManager");
const QString presenterName = QStringLiteral("org.caelestia.PlasmaNotifications");
const QString presenterPath = QStringLiteral("/org/caelestia/PlasmaNotifications");
const QString watcherPath = QStringLiteral("/NotificationWatcher");
Q_LOGGING_CATEGORY(lcPlasmaNotifications, "caelestia.services.plasmanotifications")

QVariant unwrap(const QVariant& value) {
    return value.metaType() == QMetaType::fromType<QDBusVariant>() ? value.value<QDBusVariant>().variant() : value;
}
}

PlasmaNotifications::PlasmaNotifications(QObject* parent)
    : QObject(parent)
    , m_bus(QStringLiteral("caelestia-notifications-disconnected")) {}

PlasmaNotifications::~PlasmaNotifications() {
    stop();
}

void PlasmaNotifications::setEnabled(bool enabled) {
    if (m_enabled == enabled)
        return;
    m_enabled = enabled;
    if (enabled)
        QTimer::singleShot(0, this, &PlasmaNotifications::start);
    else
        stop();
    emit stateChanged();
}

void PlasmaNotifications::start() {
    if (!m_enabled || m_registered)
        return;
    m_error.clear();
    m_connectionName = QStringLiteral("caelestia-notifications-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    m_bus = QDBusConnection::connectToBus(QDBusConnection::SessionBus, m_connectionName);
    if (!m_bus.isConnected()) {
        fail(QStringLiteral("Cannot connect Plasma notification watcher to session bus"));
        return;
    }
    const QDBusReply<QString> serverOwner = m_bus.interface()->serviceOwner(serverName);
    const QDBusReply<QString> presenterOwner = m_bus.interface()->serviceOwner(presenterName);
    if (!serverOwner.isValid() || !presenterOwner.isValid() || serverOwner.value() != presenterOwner.value()) {
        fail(QStringLiteral("The scoped notification presenter must be loaded inside the authoritative Plasma notification process"));
        return;
    }
    m_serverOwner = serverOwner.value();
    m_presenterOwner = presenterOwner.value();
    m_watcher = new QDBusServiceWatcher(this);
    m_watcher->setConnection(m_bus);
    m_watcher->setWatchMode(QDBusServiceWatcher::WatchForOwnerChange);
    m_watcher->setWatchedServices({serverName, presenterName});
    connect(m_watcher, &QDBusServiceWatcher::serviceOwnerChanged, this, &PlasmaNotifications::onOwnerChanged);
    if (!m_bus.registerObject(watcherPath, QStringLiteral("org.kde.NotificationWatcher"), this, QDBusConnection::ExportScriptableSlots)
        || !m_bus.connect(m_serverOwner, serverPath, serverName, QStringLiteral("NotificationClosed"), this, SLOT(onNotificationClosed(uint,uint)))
        || !m_bus.connect(m_presenterOwner, presenterPath, presenterName, QStringLiteral("PolicyChanged"), this, SLOT(onPresenterPolicyChanged()))) {
        fail(QStringLiteral("Cannot register the Plasma notification watcher and policy subscriptions"));
        return;
    }
    QDBusMessage request = QDBusMessage::createMethodCall(m_serverOwner, serverPath, managerInterface, QStringLiteral("RegisterWatcher"));
    const QDBusMessage reply = m_bus.call(request, QDBus::Block, 3000);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        fail(QStringLiteral("Plasma RegisterWatcher failed: %1").arg(reply.errorMessage()));
        return;
    }
    m_registered = true;
    if (!refreshPolicy())
        return;
    m_ready = true;
    emit stateChanged();
}

void PlasmaNotifications::stop() {
    m_ready = false;
    if (m_active)
        presenterCall(QStringLiteral("Release"));
    m_active = false;
    if (m_registered) {
        const auto request = QDBusMessage::createMethodCall(m_serverOwner, serverPath, managerInterface, QStringLiteral("UnRegisterWatcher"));
        m_bus.call(request, QDBus::NoBlock);
    }
    m_registered = false;
    m_bus.unregisterObject(watcherPath);
    delete m_watcher;
    m_watcher = nullptr;
    if (!m_connectionName.isEmpty())
        QDBusConnection::disconnectFromBus(m_connectionName);
    m_connectionName.clear();
    m_bus = QDBusConnection(QStringLiteral("caelestia-notifications-disconnected"));
    m_serverOwner.clear();
    m_presenterOwner.clear();
    m_entries.clear();
    emit invalidated();
    emit stateChanged();
}

void PlasmaNotifications::fail(const QString& error) {
    m_error = error;
    qCWarning(lcPlasmaNotifications) << error;
    stop();
}

QDBusMessage PlasmaNotifications::presenterCall(const QString& method, const QList<QVariant>& arguments) {
    auto request = QDBusMessage::createMethodCall(m_presenterOwner, presenterPath, presenterName, method);
    request.setArguments(arguments);
    return m_bus.call(request, QDBus::Block, 3000);
}

bool PlasmaNotifications::refreshPolicy() {
    const QDBusMessage reply = presenterCall(QStringLiteral("Snapshot"));
    if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().size() != 1) {
        fail(QStringLiteral("Cannot query scoped Plasma notification presenter: %1").arg(reply.errorMessage()));
        return false;
    }
    m_policy = qdbus_cast<QVariantMap>(reply.arguments().constFirst());
    if (!m_policy.value(QStringLiteral("supported")).toBool()) {
        fail(QStringLiteral("Unsupported Plasma popup backend: %1").arg(m_policy.value(QStringLiteral("error")).toString()));
        return false;
    }
    emit stateChanged();
    return true;
}

bool PlasmaNotifications::setActive(bool active) {
    if (m_active == active)
        return !active || m_ready;
    if (!m_ready)
        return false;
    const QDBusMessage reply = presenterCall(active ? QStringLiteral("Acquire") : QStringLiteral("Release"));
    if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().size() != 1 || !reply.arguments().constFirst().toBool()) {
        fail(QStringLiteral("Plasma popup lease %1 was not acknowledged: %2")
                 .arg(active ? QStringLiteral("acquisition") : QStringLiteral("release"), reply.errorMessage()));
        return false;
    }
    if (!refreshPolicy())
        return false;
    if (m_policy.value(QStringLiteral("active")).toBool() != active
        || (active && m_policy.value(QStringLiteral("owner")).toString() != m_bus.baseService())) {
        fail(QStringLiteral("Scoped Plasma popup suppression does not match its acknowledged lease"));
        return false;
    }
    m_active = active;
    emit stateChanged();
    if (active) {
        const auto existing = presenterCall(QStringLiteral("Existing"));
        if (existing.type() == QDBusMessage::ErrorMessage || existing.arguments().size() != 1) {
            fail(QStringLiteral("Cannot replay authoritative Plasma notifications: %1").arg(existing.errorMessage()));
            return false;
        }
        const auto notifications = qdbus_cast<QVariantList>(existing.arguments().constFirst());
        for (const auto& notification : notifications) {
            QVariantMap data = qdbus_cast<QVariantMap>(notification);
            data.insert(QStringLiteral("replayed"), true);
            if (!deliver(data))
                return false;
        }
    }
    return true;
}

bool PlasmaNotifications::present(uint id) {
    if (!m_ready || !m_active || !m_entries.contains(id))
        return false;
    const QDBusMessage reply = presenterCall(QStringLiteral("Present"), {id, !m_entries.value(id).replayed});
    if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().size() != 1) {
        fail(QStringLiteral("Cannot evaluate native notification presentation policy: %1").arg(reply.errorMessage()));
        return false;
    }
    return reply.arguments().constFirst().toBool();
}

bool PlasmaNotifications::eligible(uint id) {
    if (!m_ready || !m_active || !m_entries.contains(id))
        return false;
    const auto reply = presenterCall(QStringLiteral("Eligible"), {id});
    if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().size() != 1) {
        fail(QStringLiteral("Cannot refresh native popup eligibility: %1").arg(reply.errorMessage()));
        return false;
    }
    return reply.arguments().constFirst().toBool();
}

void PlasmaNotifications::expire(uint id) {
    if (!m_active || !m_entries.contains(id))
        return;
    const auto reply = presenterCall(QStringLiteral("Expire"), {id});
    if (reply.type() == QDBusMessage::ErrorMessage)
        fail(QStringLiteral("Cannot expire native notification presentation: %1").arg(reply.errorMessage()));
}

bool PlasmaNotifications::invokeAction(uint id, const QString& action) {
    auto it = m_entries.constFind(id);
    if (!m_active || !m_ready || it == m_entries.cend() || !it->actions.contains(action))
        return false;
    const bool resident = it->resident;
    auto request = QDBusMessage::createMethodCall(m_serverOwner, serverPath, managerInterface, QStringLiteral("InvokeAction"));
    request.setArguments({id, action});
    const auto reply = m_bus.call(request, QDBus::Block, 3000);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        fail(QStringLiteral("Plasma action invocation failed: %1").arg(reply.errorMessage()));
        return false;
    }
    if (!resident)
        return dismiss(id);
    return true;
}

bool PlasmaNotifications::dismiss(uint id) {
    if (!m_active || !m_ready || !m_entries.contains(id))
        return false;
    const auto reply = presenterCall(QStringLiteral("Dismiss"), {id});
    if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().size() != 1 || !reply.arguments().constFirst().toBool()) {
        fail(QStringLiteral("Plasma notification dismissal was not acknowledged: %1").arg(reply.errorMessage()));
        return false;
    }
    return true;
}

QString PlasmaNotifications::notificationImage(const QVariantMap& hints, uint id) {
    QVariant pixels = hints.value(QStringLiteral("image-data"), hints.value(QStringLiteral("image_data"), hints.value(QStringLiteral("icon_data"))));
    pixels = unwrap(pixels);
    if (pixels.isValid()) {
        if (pixels.metaType() != QMetaType::fromType<QDBusArgument>()) {
            fail(QStringLiteral("Notification %1 contains unsupported image-data encoding").arg(id));
            return {};
        }
        const auto argument = pixels.value<QDBusArgument>();
        int width = 0, height = 0, stride = 0, bits = 0, channels = 0;
        bool alpha = false;
        QByteArray data;
        argument.beginStructure();
        argument >> width >> height >> stride >> alpha >> bits >> channels >> data;
        argument.endStructure();
        const qint64 minimumStride = qint64(width) * channels;
        const qint64 required = qint64(stride) * (qint64(height) - 1) + minimumStride;
        if (width <= 0 || height <= 0 || width > 16384 || height > 16384 || bits != 8
            || channels != (alpha ? 4 : 3) || minimumStride > stride || required <= 0 || required > data.size()) {
            fail(QStringLiteral("Notification %1 contains invalid or unsupported image-data dimensions").arg(id));
            return {};
        }
        if (qint64(stride) * height > data.size()) {
            fail(QStringLiteral("Notification %1 image-data has an incomplete final row").arg(id));
            return {};
        }
        const QImage image(reinterpret_cast<const uchar*>(data.constData()), width, height, stride,
                           alpha ? QImage::Format_RGBA8888 : QImage::Format_RGB888);
        const QString directory = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/plasma-notification-images");
        const QString file = directory + QStringLiteral("/%1-%2-%3.png").arg(m_bus.baseService().mid(1).replace(QLatin1Char('.'), QLatin1Char('-'))).arg(id).arg(++m_imageSerial);
        if (!QDir().mkpath(directory) || !image.save(file)) {
            fail(QStringLiteral("Cannot cache image for Plasma notification %1").arg(id));
            return {};
        }
        return QUrl::fromLocalFile(file).toString();
    }
    const QString path = unwrap(hints.value(QStringLiteral("image-path"), hints.value(QStringLiteral("image_path")))).toString();
    if (path.startsWith(QLatin1Char('/')))
        return QUrl::fromLocalFile(path).toString();
    if (!path.isEmpty() && QUrl(path).scheme().isEmpty())
        return QStringLiteral("image://icon/") + path;
    return path;
}

bool PlasmaNotifications::deliver(QVariantMap data) {
    const uint id = data.value(QStringLiteral("id")).toUInt();
    if (!id) {
        fail(QStringLiteral("Plasma returned a notification without its authoritative ID"));
        return false;
    }
    const auto hints = qdbus_cast<QVariantMap>(data.value(QStringLiteral("hints")));
    QString image;
    const QByteArray png = data.take(QStringLiteral("imagePng")).toByteArray();
    if (!png.isEmpty())
        image = QStringLiteral("data:image/png;base64,") + QString::fromLatin1(png.toBase64());
    else
        image = notificationImage(hints, id);
    if (!m_ready)
        return false;
    const QString icon = data.take(QStringLiteral("iconName")).toString();
    if (image.isEmpty() && !icon.isEmpty()) {
        if (icon.startsWith(QLatin1Char('/')))
            image = QUrl::fromLocalFile(icon).toString();
        else if (QUrl(icon).scheme().isEmpty())
            image = QStringLiteral("image://icon/") + icon;
        else
            image = icon;
    }
    data.insert(QStringLiteral("image"), image);
    data.insert(QStringLiteral("hints"), hints);
    QVariantList actions = qdbus_cast<QVariantList>(data.value(QStringLiteral("actions")));
    Entry entry;
    entry.resident = data.value(QStringLiteral("resident")).toBool();
    entry.replayed = data.value(QStringLiteral("replayed")).toBool();
    for (auto& value : actions) {
        const QVariantMap action = qdbus_cast<QVariantMap>(value);
        entry.actions.insert(action.value(QStringLiteral("identifier")).toString());
        value = action;
    }
    data.insert(QStringLiteral("actions"), actions);
    const uint replaced = data.value(QStringLiteral("replacesId")).toUInt();
    if (replaced && replaced != id)
        m_entries.remove(replaced);
    m_entries.insert(id, entry);
    emit notificationReceived(data);
    return m_ready;
}

void PlasmaNotifications::Notify(uint id, const QString& appName, uint replacesId, const QString& appIcon,
                                 const QString& summary, const QString& body, const QStringList& actions,
                                 const QVariantMap& hints, int timeout) {
    if (!calledFromDBus() || message().service() != m_serverOwner || !m_registered)
        return;
    if (actions.size() % 2) {
        fail(QStringLiteral("Notification %1 has malformed action pairs").arg(id));
        return;
    }
    if (!m_active)
        return;
    const auto reply = presenterCall(QStringLiteral("Describe"), {id});
    if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().size() != 1) {
        fail(QStringLiteral("Cannot describe authoritative Plasma notification %1: %2").arg(id).arg(reply.errorMessage()));
        return;
    }
    QVariantMap data = qdbus_cast<QVariantMap>(reply.arguments().constFirst());
    data.insert(QStringLiteral("replacesId"), replacesId);
    // The watcher preserves original hints/actions omitted by normalized history.
    data.insert(QStringLiteral("hints"), hints);
    QVariantList actionList;
    for (qsizetype i = 0; i < actions.size(); i += 2)
        actionList.append(QVariantMap{{QStringLiteral("identifier"), actions.at(i)}, {QStringLiteral("text"), actions.at(i + 1)}});
    data.insert(QStringLiteral("actions"), actionList);
    if (!data.contains(QStringLiteral("id"))) {
        fail(QStringLiteral("Plasma watcher notification %1 is absent from its authoritative model").arg(id));
        return;
    }
    deliver(data);
}

void PlasmaNotifications::CloseNotification(uint id) {
    Q_UNUSED(id);
    // NotificationClosed carries the authoritative reason. This watcher method
    // does not, so it must not consume the ID before that real signal arrives.
}

void PlasmaNotifications::onNotificationClosed(uint id, uint reason) {
    if (m_entries.remove(id))
        emit notificationClosed(id, reason);
}

void PlasmaNotifications::onOwnerChanged(const QString&, const QString&, const QString&) {
    fail(QStringLiteral("Plasma notification service or scoped presenter ownership changed; re-prepare the managed shell"));
}

void PlasmaNotifications::onPresenterPolicyChanged() {
    if (!m_ready || !refreshPolicy())
        return;
    if (m_active && (!m_policy.value(QStringLiteral("active")).toBool()
        || m_policy.value(QStringLiteral("owner")).toString() != m_bus.baseService()))
        fail(QStringLiteral("The scoped Plasma popup lease was lost"));
    else
        emit notificationPolicyChanged();
}

} // namespace caelestia::services
