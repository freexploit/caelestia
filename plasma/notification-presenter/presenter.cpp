#include "presenter.hpp"

// Notifications may be degraded per field (inline reply omitted, sender actions
// omitted, image omitted). Only attach/ownership/lease problems are capability
// failures; a single notification must never latch the bridge off.

#include <QBuffer>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusReply>
#include <QImage>
#include <QJSValue>
#include <QLoggingCategory>
#include <QObject>
#include <KPluginFactory>
#include <QScopedValueRollback>
#include <QUrl>

namespace {
Q_LOGGING_CATEGORY(lcPresenter, "caelestia.notification-presenter")
const QString service = QStringLiteral("org.caelestia.PlasmaNotifications");
const QString path = QStringLiteral("/org/caelestia/PlasmaNotifications");
QObject* objectProperty(QObject* object, const char* property) {
    const QVariant value = object->property(property);
    if (value.metaType() == QMetaType::fromType<QJSValue>())
        return value.value<QJSValue>().toQObject();
    return value.value<QObject*>();
}
}

NotificationPresenter::NotificationPresenter(QObject* parent, const KPluginMetaData& data, const QVariantList& args)
    : Plasma::Applet(parent, data, args)
    , m_watcher(this) {
    m_watcher.setConnection(QDBusConnection::sessionBus());
    m_watcher.setWatchMode(QDBusServiceWatcher::WatchForUnregistration);
    connect(&m_watcher, &QDBusServiceWatcher::serviceUnregistered, this, [this](const QString& owner) {
        if (owner == m_owner)
            releaseLease();
    });
    m_watchdog.setInterval(1000);
    connect(&m_watchdog, &QTimer::timeout, this, &NotificationPresenter::watchdog);
}

NotificationPresenter::~NotificationPresenter() {
    detach();
}

void NotificationPresenter::attach(QObject* globals, QObject* history) {
    if (m_registered || !globals)
        return;
    m_globals = globals;
    m_instantiator = objectProperty(globals, "popupInstantiator");
    m_popupModel = qobject_cast<NotificationManager::Notifications*>(objectProperty(globals, "popupNotificationsModel"));
    m_historyModel = qobject_cast<NotificationManager::Notifications*>(history);
    if (!supported()) {
        m_error = QStringLiteral("Plasma notification singleton does not expose the supported popup instantiator/model");
        return;
    }
    auto bus = QDBusConnection::sessionBus();
    const QDBusReply<QString> owner = bus.interface()->serviceOwner(QStringLiteral("org.freedesktop.Notifications"));
    if (!owner.isValid() || owner.value() != bus.baseService()) {
        m_error = QStringLiteral("Presenter is not inside the authoritative Plasma notification process");
        return;
    }
    if (!bus.registerObject(path, this, QDBusConnection::ExportScriptableSlots | QDBusConnection::ExportScriptableSignals)) {
        m_error = QStringLiteral("Another notification presenter object is already registered");
        return;
    }
    if (!bus.registerService(service)) {
        bus.unregisterObject(path);
        m_error = QStringLiteral("Another notification presenter owns the session service");
        return;
    }
    m_registered = true;
    m_watchdog.start();
}

void NotificationPresenter::detach() {
    if (!m_registered)
        return;
    releaseLease();
    m_watchdog.stop();
    auto bus = QDBusConnection::sessionBus();
    bus.unregisterObject(path);
    bus.unregisterService(service);
    m_registered = false;
}

bool NotificationPresenter::supported() const {
    return m_globals && m_instantiator && m_popupModel && m_historyModel
        && m_instantiator->metaObject()->indexOfProperty("active") >= 0
        && m_instantiator->property("active").isValid();
}

bool NotificationPresenter::isOwner() const {
    return calledFromDBus() && !m_owner.isEmpty() && message().service() == m_owner;
}

QVariantMap NotificationPresenter::Snapshot() {
    return {{QStringLiteral("supported"), supported() && m_error.isEmpty()},
            {QStringLiteral("active"), leased() && m_instantiator && !m_instantiator->property("active").toBool()},
            {QStringLiteral("owner"), m_owner}, {QStringLiteral("error"), m_error},
            {QStringLiteral("inhibited"), m_globals ? m_globals->property("inhibited").toBool() : false},
            {QStringLiteral("backend"), QStringLiteral("plasma-6.6-popup-instantiator")},
            {QStringLiteral("sound"), QStringLiteral("native-policy-sound-hint")},
            {QStringLiteral("configurationChanged"), false}};
}

bool NotificationPresenter::Acquire() {
    if (!calledFromDBus() || !supported() || !m_error.isEmpty())
        return false;
    if (leased())
        return isOwner() && !m_instantiator->property("active").toBool();
    if (!m_instantiator->property("active").toBool()) {
        m_error = QStringLiteral("Native notification instantiator was already disabled by another owner");
        Q_EMIT PolicyChanged();
        return false;
    }
    QScopedValueRollback<bool> changing(m_changing, true);
    m_previousActive = m_instantiator->property("active").toBool();
    m_owner = message().service();
    m_watcher.addWatchedService(m_owner);
    Q_EMIT leasedChanged(); // Synchronous, non-delayed QML Binding applies here.
    if (m_instantiator->property("active").toBool()) {
        releaseLease();
        m_error = QStringLiteral("The scoped QML popup binding did not become effective");
        Q_EMIT PolicyChanged();
        return false;
    }
    Q_EMIT PolicyChanged();
    return true;
}

bool NotificationPresenter::Release() {
    if (!isOwner())
        return !leased();
    return releaseLease();
}

bool NotificationPresenter::releaseLease() {
    if (!leased())
        return true;
    QScopedValueRollback<bool> changing(m_changing, true);
    m_watcher.removeWatchedService(m_owner);
    m_owner.clear();
    using N = NotificationManager::Notifications;
    for (uint id : std::as_const(m_presented)) {
        const auto index = find(m_historyModel, id);
        if (!index.isValid())
            continue;
        // Do not replay handled notifications/sounds on the restored renderer.
        // This changes notification lifecycle, never the user's policy.
        m_historyModel->setData(index, true, N::ExpiredRole);
        m_historyModel->startTimeout(id);
    }
    m_presented.clear();
    Q_EMIT leasedChanged(); // Binding restores the original value/binding.
    const bool restored = m_instantiator && m_instantiator->property("active").toBool() == m_previousActive;
    if (!restored)
        m_error = QStringLiteral("Original Plasma popup binding was not restored");
    Q_EMIT PolicyChanged();
    return restored;
}

void NotificationPresenter::checkPresentation() {
    if (m_changing || !leased())
        return;
    if (!supported() || m_instantiator->property("active").toBool()) {
        m_error = QStringLiteral("Scoped popup binding was lost or overridden");
        releaseLease();
    }
}

void NotificationPresenter::policyUpdated() {
    if (m_registered)
        Q_EMIT PolicyChanged();
}

void NotificationPresenter::watchdog() {
    checkPresentation();
    if (!leased())
        return;
    const QDBusReply<bool> alive = QDBusConnection::sessionBus().interface()->isServiceRegistered(m_owner);
    if (!alive.isValid() || !alive.value())
        releaseLease();
}

QModelIndex NotificationPresenter::find(NotificationManager::Notifications* model, uint id) const {
    if (model)
        for (int row = 0; row < model->rowCount(); ++row) {
            const auto index = model->index(row, 0);
            if (index.data(NotificationManager::Notifications::TypeRole).toInt() == NotificationManager::Notifications::NotificationType
                && index.data(NotificationManager::Notifications::IdRole).toUInt() == id)
                return index;
        }
    return {};
}

NotificationManager::Notifications* NotificationPresenter::historyModel() const {
    return m_historyModel;
}

bool NotificationPresenter::Present(uint id, bool playSound) {
    if (!isOwner() || !supported() || m_instantiator->property("active").toBool())
        return false;
    const auto index = find(m_popupModel, id);
    if (!index.isValid())
        return false; // Native filtering, including DND, is authoritative.
    m_popupModel->stopTimeout(index);
    if (playSound)
        m_popupModel->playSoundHint(index);
    m_presented.insert(id);
    return true;
}

bool NotificationPresenter::Eligible(uint id) {
    return isOwner() && supported() && find(m_popupModel, id).isValid();
}

bool NotificationPresenter::Expire(uint id) {
    if (!isOwner() || !supported())
        return false;
    const auto index = find(historyModel(), id);
    if (!index.isValid())
        return false;
    using N = NotificationManager::Notifications;
    const bool hasActions = index.data(N::HasDefaultActionRole).toBool() || index.data(N::HasReplyActionRole).toBool()
        || !index.data(N::ActionNamesRole).toStringList().isEmpty();
    if (index.data(N::ResidentRole).toBool() || (hasActions && !index.data(N::TransientRole).toBool()))
        m_historyModel->setData(index, true, N::ExpiredRole);
    else
        m_historyModel->expire(index);
    m_presented.remove(id);
    return true;
}

bool NotificationPresenter::Dismiss(uint id) {
    if (!isOwner() || !supported())
        return false;
    const auto index = find(historyModel(), id);
    if (!index.isValid())
        return false;
    m_historyModel->close(index); // DismissedByUser, not FDO's Revoked reason.
    m_presented.remove(id);
    return true;
}

QVariantMap NotificationPresenter::describe(const QModelIndex& index) {
    // Per-notification data problems degrade this notification only. They must
    // never change presenter-wide capability state: a single chat notification
    // requesting inline reply used to latch the whole managed bridge off.
    if (!index.isValid())
        return {{QStringLiteral("declined"), true}, {QStringLiteral("reason"), QStringLiteral("absent")}};
    using N = NotificationManager::Notifications;
    const uint id = index.data(N::IdRole).toUInt();
    QStringList degraded;
    QVariantList actions;
    if (index.data(N::HasDefaultActionRole).toBool())
        actions.append(QVariantMap{{QStringLiteral("identifier"), QStringLiteral("default")},
                                   {QStringLiteral("text"), index.data(N::DefaultActionLabelRole)}});
    const auto names = index.data(N::ActionNamesRole).toStringList();
    const auto labels = index.data(N::ActionLabelsRole).toStringList();
    if (names.size() != labels.size()) {
        // Keep the default action; sender actions cannot be paired safely.
        degraded.append(QStringLiteral("action names/labels do not match; sender actions omitted"));
    } else {
        for (qsizetype i = 0; i < names.size(); ++i) {
            if (names.at(i) == QLatin1String("inline-reply")) {
                // Plasma's reply affordance needs text input the shell does not
                // render. Exposing it as a button would invoke an action that
                // expects a reply body.
                degraded.append(QStringLiteral("inline-reply omitted"));
                continue;
            }
            actions.append(QVariantMap{{QStringLiteral("identifier"), names.at(i)}, {QStringLiteral("text"), labels.at(i)}});
        }
    }
    if (index.data(N::HasReplyActionRole).toBool() && !degraded.contains(QStringLiteral("inline-reply omitted")))
        degraded.append(QStringLiteral("inline-reply omitted"));
    QVariantMap hints = index.data(N::HintsRole).toMap();
    const int urgency = index.data(N::UrgencyRole).toInt();
    QByteArray imagePng;
    const QImage image = qvariant_cast<QImage>(index.data(N::ImageRole));
    if (!image.isNull()) {
        QBuffer buffer(&imagePng);
        buffer.open(QIODevice::WriteOnly);
        if (!image.save(&buffer, "PNG")) {
            imagePng.clear();
            degraded.append(QStringLiteral("image omitted"));
        }
    }
    if (!degraded.isEmpty())
        qCWarning(lcPresenter).nospace() << "notification " << id << " degraded: " << degraded.join(QStringLiteral("; "));
    return {{QStringLiteral("id"), id}, {QStringLiteral("replacesId"), id},
            {QStringLiteral("summary"), index.data(N::SummaryRole)}, {QStringLiteral("body"), index.data(N::BodyRole)},
            {QStringLiteral("appName"), index.data(N::ApplicationNameRole)},
            {QStringLiteral("appIcon"), index.data(N::ApplicationIconNameRole)},
            {QStringLiteral("iconName"), index.data(N::IconNameRole)},
            {QStringLiteral("imagePng"), imagePng},
            {QStringLiteral("hints"), hints}, {QStringLiteral("actions"), actions},
            {QStringLiteral("expireTimeout"), index.data(N::TimeoutRole)},
            {QStringLiteral("urgency"), urgency == N::CriticalUrgency ? 2 : urgency == N::LowUrgency ? 0 : 1},
            {QStringLiteral("resident"), index.data(N::ResidentRole)},
            {QStringLiteral("expired"), index.data(N::ExpiredRole)},
            {QStringLiteral("hasActionIcons"), hints.value(QStringLiteral("action-icons"), false)}};
}

QVariantMap NotificationPresenter::Describe(uint id) {
    if (!isOwner())
        return {};
    return describe(find(historyModel(), id));
}

QVariantList NotificationPresenter::Existing() {
    QVariantList result;
    if (!isOwner() || !supported())
        return result;
    result.reserve(m_historyModel->rowCount());
    using N = NotificationManager::Notifications;
    for (int row = 0; row < m_historyModel->rowCount(); ++row) {
        const auto index = m_historyModel->index(row, 0);
        if (index.data(N::TypeRole).toInt() != N::NotificationType)
            continue;
        if (!index.data(N::ExpiredRole).toBool() || index.data(N::HasDefaultActionRole).toBool()
            || !index.data(N::ActionNamesRole).toStringList().isEmpty()) {
            const QVariantMap entry = describe(index);
            if (!entry.value(QStringLiteral("declined")).toBool())
                result.append(entry);
        }
    }
    return result;
}

K_PLUGIN_CLASS_WITH_JSON(NotificationPresenter, "metadata.json")
#include "presenter.moc"
