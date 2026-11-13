#include "globalshortcut.hpp"

#include <KGlobalAccel>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QProcess>
#include <QStringList>
#include <QTextStream>
#include <cstdlib>

#include "../Config/common.hpp"
#include "../Config/generalconfig.hpp"
#include "../Config/rootnodes.hpp"
#include "../../../../../src/lib/shortcutrecovery.hpp"

Q_GLOBAL_STATIC(GlobalShortcutDispatcher, s_dispatcher)

namespace {

bool managedRuntime() {
    static const bool managed = qEnvironmentVariable("CAELESTIA_MANAGED") == QLatin1String("1");
    return managed;
}

QString escapeGVariantString(const QString& value) {
    QString escaped = value;
    escaped.replace(u'\\', QStringLiteral("\\\\"));
    escaped.replace(u'\'', QStringLiteral("\\'"));
    return escaped;
}

QString stolenShortcutsPath() {
    return caelestia::config::configFile(QStringLiteral("stolen-shortcuts.json"));
}

QStringList buildRestoreArgs(const QString& component, const QString& action, const QList<QKeySequence>& keys) {
    QStringList seqStrings;
    for (const QKeySequence& seq : keys) {
        int k1 = seq.count() > 0 ? seq[0].toCombined() : 0;
        int k2 = seq.count() > 1 ? seq[1].toCombined() : 0;
        int k3 = seq.count() > 2 ? seq[2].toCombined() : 0;
        int k4 = seq.count() > 3 ? seq[3].toCombined() : 0;
        seqStrings.append(QStringLiteral("([%1, %2, %3, %4],)").arg(k1).arg(k2).arg(k3).arg(k4));
    }
    QString arrayStr = seqStrings.isEmpty()
                           ? QStringLiteral("[([0, 0, 0, 0],)]")
                           : QStringLiteral("[") + seqStrings.join(QStringLiteral(", ")) + QStringLiteral("]");
    return { QStringLiteral("call"), QStringLiteral("--session"), QStringLiteral("--dest"),
        QStringLiteral("org.kde.kglobalaccel"), QStringLiteral("--object-path"), QStringLiteral("/kglobalaccel"),
        QStringLiteral("--method"), QStringLiteral("org.kde.KGlobalAccel.setShortcutKeys"),
        QStringLiteral("['%1', '%2', '', '']").arg(escapeGVariantString(component), escapeGVariantString(action)),
        arrayStr, QStringLiteral("4") };
}

} // namespace

GlobalShortcutDispatcher::GlobalShortcutDispatcher(QObject* parent)
    : QObject(parent) {
    if (!managedRuntime())
        return;
    m_recovery = std::make_unique<caelestia::shortcuts::Recovery>();
    if (!m_recovery->open(m_error) || !m_recovery->restoreAll(m_error))
        qWarning() << "[Caelestia] Shortcut recovery failed:" << m_error;
    auto bus = QDBusConnection::sessionBus();
    m_serviceRegistered = bus.registerService(QStringLiteral("org.caelestia.Shortcuts"));
    if (!m_serviceRegistered || !bus.registerObject(QStringLiteral("/org/caelestia/Shortcuts"), this,
            QDBusConnection::ExportAllSlots)) {
        m_error = QStringLiteral("Cannot register managed shortcut DBus service");
        qWarning() << "[Caelestia]" << m_error;
    }
    if (QCoreApplication::instance())
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this] { release(); });
}

GlobalShortcutDispatcher::~GlobalShortcutDispatcher() = default;

QVariantMap GlobalShortcutDispatcher::status() const {
    int registered = 0;
    for (const auto* shortcut : GlobalShortcut::allShortcuts()) {
        if (!shortcut->m_activeKeys.isEmpty())
            ++registered;
    }
    return { { QStringLiteral("ok"), m_error.isEmpty() }, { QStringLiteral("pending"), 0 },
        { QStringLiteral("registered"), registered }, { QStringLiteral("metaAction"), QString() },
        { QStringLiteral("metaBackend"), QStringLiteral("kglobalaccel") },
        { QStringLiteral("error"), m_error } };
}

QVariantMap GlobalShortcutDispatcher::release() {
    if (!managedRuntime())
        return status();
    m_released = true;
    for (auto* shortcut : GlobalShortcut::allShortcuts())
        shortcut->releaseManagedShortcut();
    QString error;
    if (m_recovery && m_recovery->restoreAll(error))
        m_error.clear();
    else
        m_error = error.isEmpty() ? QStringLiteral("Shortcut recovery unavailable") : error;
    return status();
}

GlobalShortcutDispatcher* GlobalShortcutDispatcher::instance() {
    GlobalShortcutDispatcher* inst = s_dispatcher();
    if (managedRuntime())
        return inst;

    // On very first access, run crash recovery: if the stolen-shortcuts file
    // exists, a previous session crashed before cleaning up. Restore everything
    // and pre-populate the collision index so blinkers light up immediately.
    static bool recovered = false;
    if (!recovered) {
        recovered = true;
        QString path = stolenShortcutsPath();
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) {
            QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
            file.close();
            if (doc.isArray()) {
                const QJsonArray entries = doc.array();
                for (const QJsonValue& val : entries) {
                    QJsonObject obj = val.toObject();
                    QString component = obj.value(QStringLiteral("component")).toString();
                    QString action = obj.value(QStringLiteral("action")).toString();
                    QString componentFriendly = obj.value(QStringLiteral("componentFriendlyName")).toString();
                    QString actionFriendly = obj.value(QStringLiteral("actionFriendlyName")).toString();
                    QList<QKeySequence> keys;
                    for (const QJsonValue& kv : obj.value(QStringLiteral("keys")).toArray()) {
                        QKeySequence seq = QKeySequence::fromString(kv.toString());
                        if (!seq.isEmpty())
                            keys.append(seq);
                    }
                    if (!component.isEmpty() && !action.isEmpty()) {
                        qDebug() << "[Caelestia] Crash recovery: restoring shortcut" << action << "for" << component;
                        QProcess::startDetached(QStringLiteral("gdbus"), buildRestoreArgs(component, action, keys));

                        // Populate the collision index so the blinker shows collisions
                        // even though no GlobalShortcut instances have been created yet.
                        QString label = componentFriendly.isEmpty() ? component : componentFriendly;
                        QString actionLabel = actionFriendly.isEmpty() ? action : actionFriendly;
                        QString friendlyLabel = label + QStringLiteral(" - ") + actionLabel;
                        for (const QKeySequence& seq : keys) {
                            inst->m_collisionIndex.insert(seq.toString(QKeySequence::PortableText), friendlyLabel);
                        }
                    }
                }
                if (!inst->m_collisionIndex.isEmpty()) {
                    emit inst->collisionIndexChanged();
                }
            }
            // Remove recovery file — crash recovery is done
            QFile::remove(path);
        }

        if (QCoreApplication::instance()) {
            QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, [] {
                QFile::remove(stolenShortcutsPath());
                qDebug() << "[Caelestia] Removed stolen-shortcuts recovery file on clean exit";
            });
        }
    }

    return inst;
}

QString GlobalShortcutDispatcher::collisionForKey(const QString& portableKeyString) const {
    return m_collisionIndex.value(portableKeyString);
}

void GlobalShortcutDispatcher::rebuildCollisionIndex() {
    GlobalShortcut::rebuildCollisionIndex();
}

void GlobalShortcut::rebuildCollisionIndex() {
    auto* dispatcher = GlobalShortcutDispatcher::instance();
    dispatcher->m_collisionIndex.clear();
    for (const GlobalShortcut* sc : s_registry) {
        for (const auto& stolen : sc->m_stolenShortcuts) {
            const QString label =
                stolen.componentFriendlyName.isEmpty() ? stolen.component : stolen.componentFriendlyName;
            const QString actionLabel = stolen.actionFriendlyName.isEmpty() ? stolen.action : stolen.actionFriendlyName;
            const QString friendlyLabel = label + QStringLiteral(" - ") + actionLabel;
            for (const QKeySequence& seq : stolen.keys) {
                dispatcher->m_collisionIndex.insert(seq.toString(QKeySequence::PortableText), friendlyLabel);
            }
        }
    }

    QHash<QString, QList<const GlobalShortcut*>> owners;
    for (const GlobalShortcut* sc : s_registry) {
        for (const QKeySequence& seq : sc->m_activeKeys) {
            owners[seq.toString(QKeySequence::PortableText)].append(sc);
        }
    }

    for (auto it = owners.constBegin(); it != owners.constEnd(); ++it) {
        if (it.value().size() < 2)
            continue;

        if (dispatcher->m_collisionIndex.contains(it.key()))
            continue;

        QStringList names;
        QList<const GlobalShortcut*> counted;
        for (const GlobalShortcut* sc : it.value()) {
            if (counted.contains(sc))
                continue;
            counted.append(sc);
            names.append(sc->displayLabel());
        }
        names.sort();

        dispatcher->m_collisionIndex.insert(
            it.key(), QStringLiteral("Caelestia - ") + names.join(QStringLiteral(", ")));
    }

    emit dispatcher->collisionIndexChanged();
}

QString GlobalShortcut::displayLabel() const {
    if (!m_description.isEmpty())
        return m_description;
    if (!m_name.isEmpty())
        return m_name;
    return QStringLiteral("unnamed shortcut");
}

QHash<QString, GlobalShortcut*> GlobalShortcut::s_registry;

GlobalShortcut::GlobalShortcut(QObject* parent)
    : QObject(parent)
    , m_action(new QAction(this)) {
    connect(m_action, &QAction::triggered, this, &GlobalShortcut::activated);
}

GlobalShortcut::~GlobalShortcut() {
    if (managedRuntime()) {
        releaseManagedShortcut();
        if (!m_name.isEmpty()) {
            s_registry.remove(m_name);
            emit GlobalShortcutDispatcher::instance()->shortcutUnregistered(this);
        }
        return;
    }
    if (!m_name.isEmpty()) {
        s_registry.remove(m_name);
        emit GlobalShortcutDispatcher::instance() -> shortcutUnregistered(this);
    }

    for (const auto& stolen : m_stolenShortcuts) {
        QProcess::startDetached(
            QStringLiteral("gdbus"), buildRestoreArgs(stolen.component, stolen.action, stolen.keys));
    }

    if (!m_stolenShortcuts.isEmpty())
        persistStolenShortcuts();
}

void GlobalShortcut::persistStolenShortcuts() const {
    QJsonArray entries;
    for (const GlobalShortcut* sc : s_registry) {
        for (const auto& stolen : sc->m_stolenShortcuts) {
            QJsonArray keyArr;
            for (const QKeySequence& seq : stolen.keys) {
                keyArr.append(seq.toString(QKeySequence::PortableText));
            }
            QJsonObject obj;
            obj.insert(QStringLiteral("component"), stolen.component);
            obj.insert(QStringLiteral("action"), stolen.action);
            obj.insert(QStringLiteral("componentFriendlyName"), stolen.componentFriendlyName);
            obj.insert(QStringLiteral("actionFriendlyName"), stolen.actionFriendlyName);
            obj.insert(QStringLiteral("keys"), keyArr);
            entries.append(obj);
        }
    }

    const QString path = stolenShortcutsPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(QJsonDocument(entries).toJson());
    }

    GlobalShortcutDispatcher::instance()->rebuildCollisionIndex();
}

QString GlobalShortcut::name() const {
    return m_name;
}

void GlobalShortcut::setName(const QString& name) {
    if (managedRuntime() && m_name != name && !m_name.isEmpty() && !releaseManagedShortcut())
        return;
    if (m_name == name)
        return;

    if (!m_name.isEmpty()) {
        s_registry.remove(m_name);
    }

    m_name = name;
    if (!m_action) {
        m_action = new QAction(this);
        connect(m_action, &QAction::triggered, this, &GlobalShortcut::activated);
    }
    m_action->setObjectName(QStringLiteral("caelestia-shortcut-") + m_name);
    if (managedRuntime()) {
        m_action->setProperty("componentName", QStringLiteral("caelestia-activity"));
        m_action->setProperty("componentDisplayName", QStringLiteral("Caelestia Activity"));
    }

    if (!m_name.isEmpty()) {
        s_registry.insert(m_name, this);
    }

    emit nameChanged();
    emit GlobalShortcutDispatcher::instance() -> shortcutRegistered(this);

    updateShortcut();
}

QString GlobalShortcut::key() const {
    return m_key;
}

void GlobalShortcut::setKey(const QString& key) {
    if (m_key == key)
        return;

    m_key = key;
    emit keyChanged();
    updateShortcut();
}

QString GlobalShortcut::getCollisionName() const {
    if (m_stolenShortcuts.isEmpty()) {
        return QString();
    }
    const auto& s = m_stolenShortcuts.first();
    QString actionName = s.actionFriendlyName;
    if (actionName.isEmpty())
        actionName = s.action;
    return s.componentFriendlyName + QStringLiteral(" - ") + actionName;
}

QString GlobalShortcut::getCollisionNameForKey(const QString& keyPart) const {
    QKeySequence targetSeq(keyPart.trimmed());
    if (targetSeq.isEmpty())
        return QString();

    for (const auto& s : m_stolenShortcuts) {
        if (s.keys.contains(targetSeq)) {
            QString actionName = s.actionFriendlyName;
            if (actionName.isEmpty())
                actionName = s.action;
            return s.componentFriendlyName + QStringLiteral(" - ") + actionName;
        }
    }
    return QString();
}

QString GlobalShortcut::description() const {
    return m_description;
}

void GlobalShortcut::setDescription(const QString& description) {
    if (m_description == description)
        return;

    m_description = description;
    emit descriptionChanged();
    updateShortcut();
}

GlobalShortcut* GlobalShortcut::findByName(const QString& name) {
    return s_registry.value(name, nullptr);
}

QList<GlobalShortcut*> GlobalShortcut::allShortcuts() {
    return s_registry.values();
}

void GlobalShortcut::updateShortcut() {
    if (m_name.isEmpty()) {
        return;
    }

    const int myGeneration = ++m_registerGeneration;

    if (m_action)
        m_action->setText(m_description.isEmpty() ? QStringLiteral("Caelestia Action") : m_description);

    QList<QKeySequence> newSeqs;
    if (!m_key.isEmpty()) {
        const QStringList parts = m_key.split(QStringLiteral(";"));
        for (const QString& part : parts) {
            const QString trimmed = part.trimmed();
            if (managedRuntime() && trimmed.compare(QLatin1String("Meta"), Qt::CaseInsensitive) == 0) {
                newSeqs.append(QKeySequence(Qt::MetaModifier));
                continue;
            }
            if (!trimmed.isEmpty()) {
                newSeqs.append(QKeySequence(trimmed));
            }
        }
    }
    if (managedRuntime()) {
        updateManagedShortcut(caelestia::shortcuts::normalized(newSeqs));
        return;
    }

    QList<QKeySequence> removedSeqs;
    for (const QKeySequence& old : m_activeKeys) {
        if (!newSeqs.contains(old)) {
            removedSeqs.append(old);
        }
    }
    QList<QKeySequence> addedSeqs;
    for (const QKeySequence& seq : newSeqs) {
        if (!m_activeKeys.contains(seq)) {
            addedSeqs.append(seq);
        }
    }

    if (!removedSeqs.isEmpty()) {
        QList<StolenShortcut> toKeep;
        for (const auto& stolen : m_stolenShortcuts) {
            if (removedSeqs.contains(stolen.triggerKey)) {
                qDebug() << "[Caelestia] Restoring shortcut" << stolen.action << "for" << stolen.component
                         << "— trigger key removed";
                QProcess::startDetached(
                    QStringLiteral("gdbus"), buildRestoreArgs(stolen.component, stolen.action, stolen.keys));
            } else {
                toKeep.append(stolen);
            }
        }
        m_stolenShortcuts = toKeep;
    }

    m_activeKeys = newSeqs;

    if (newSeqs.isEmpty()) {
        persistStolenShortcuts();
        KGlobalAccel::self()->removeAllShortcuts(m_action);
        return;
    }

    if (addedSeqs.isEmpty()) {
        persistStolenShortcuts();
        KGlobalAccel::self()->setShortcut(m_action, newSeqs, KGlobalAccel::NoAutoloading);
        return;
    }

    QList<QStringList> stealCmds;

    for (const QKeySequence& seq : addedSeqs) {
        const QList<KGlobalShortcutInfo> conflicts = KGlobalAccel::globalShortcutsByKey(seq);
        for (const auto& info : conflicts) {
            if (info.componentUniqueName() == QStringLiteral("caelestia") ||
                info.componentUniqueName() == QCoreApplication::applicationName() ||
                info.componentUniqueName() == QStringLiteral("quickshell")) {
                continue;
            }

            bool alreadyStolen = false;
            for (const auto& existing : m_stolenShortcuts) {
                if (existing.component == info.componentUniqueName() && existing.action == info.uniqueName()) {
                    alreadyStolen = true;
                    break;
                }
            }
            if (alreadyStolen)
                continue;

            m_stolenShortcuts.append({ info.componentUniqueName(), info.uniqueName(), info.keys(),
                info.componentFriendlyName(), info.friendlyName(), seq });

            stealCmds.append({ QStringLiteral("call"), QStringLiteral("--session"), QStringLiteral("--dest"),
                QStringLiteral("org.kde.kglobalaccel"), QStringLiteral("--object-path"),
                QStringLiteral("/kglobalaccel"), QStringLiteral("--method"),
                QStringLiteral("org.kde.KGlobalAccel.setShortcutKeys"),
                QStringLiteral("['%1', '%2', '', '']")
                    .arg(escapeGVariantString(info.componentUniqueName()), escapeGVariantString(info.uniqueName())),
                QStringLiteral("[([0, 0, 0, 0],)]"), QStringLiteral("4") });
        }
    }

    persistStolenShortcuts();

    if (stealCmds.isEmpty()) {
        KGlobalAccel::self()->setShortcut(m_action, newSeqs, KGlobalAccel::NoAutoloading);
        return;
    }

    auto pending = std::make_shared<QAtomicInt>(stealCmds.size());
    const auto settle = [this, pending, newSeqs, myGeneration]() {
        if (pending->fetchAndSubRelaxed(1) == 1 && m_registerGeneration == myGeneration) {
            KGlobalAccel::self()->setShortcut(m_action, newSeqs, KGlobalAccel::NoAutoloading);
        }
    };

    for (const QStringList& args : stealCmds) {
        auto* proc = new QProcess(this);
        connect(proc, &QProcess::finished, proc, [proc, settle](int, QProcess::ExitStatus) {
            proc->deleteLater();
            settle();
        });
        connect(proc, &QProcess::errorOccurred, proc, [proc, settle, args](QProcess::ProcessError err) {
            if (err != QProcess::FailedToStart) {
                return;
            }
            qWarning() << "[Caelestia] could not run gdbus to release a conflicting shortcut:" << args;
            proc->deleteLater();
            settle();
        });
        proc->start(QStringLiteral("gdbus"), args);
    }
}

bool GlobalShortcut::releaseManagedShortcut() {
    auto* dispatcher = GlobalShortcutDispatcher::instance();
    // Disconnect the callback before returning keys. QAction destruction marks
    // the action inactive but does not overwrite a later editor's saved keys.
    delete m_action;
    m_action = nullptr;
    m_activeKeys.clear();
    QString error;
    if (dispatcher->m_recovery && !dispatcher->m_recovery->restore(m_name, error)) {
        dispatcher->m_error = error;
        qWarning() << "[Caelestia] Shortcut release failed:" << error;
        return false;
    }
    m_stolenShortcuts.clear();
    rebuildCollisionIndex();
    return true;
}

void GlobalShortcut::updateManagedShortcut(const QList<QKeySequence>& keys) {
    using namespace caelestia::shortcuts;
    auto* dispatcher = GlobalShortcutDispatcher::instance();
    if (dispatcher->m_released || !dispatcher->m_error.isEmpty())
        return;
    if (keys == m_activeKeys)
        return;
    if (!m_activeKeys.isEmpty()) {
        if (!releaseManagedShortcut())
            return;
    }
    if (!m_action) {
        m_action = new QAction(this);
        connect(m_action, &QAction::triggered, this, &GlobalShortcut::activated);
    }
    m_action->setObjectName(QStringLiteral("caelestia-shortcut-") + m_name);
    m_action->setProperty("componentName", QStringLiteral("caelestia-activity"));
    m_action->setProperty("componentDisplayName", QStringLiteral("Caelestia Activity"));
    m_action->setText(displayLabel());
    const QStringList id { QStringLiteral("caelestia-activity"), m_action->objectName(),
        QStringLiteral("Caelestia Activity"), displayLabel() };
    QString error;
    const auto fail = [&] {
        dispatcher->m_error = error;
        qWarning() << "[Caelestia] Shortcut acquisition failed:" << error;
        releaseManagedShortcut();
    };
    if (!keys.isEmpty()) {
        QList<QKeySequence> before;
        if (!readKeys(id, before, error)
            || !dispatcher->m_recovery->prepare(m_name, id, before, keys, true, error)) {
            fail();
            return;
        }
    }
    for (const auto& key : keys) {
        QList<KGlobalShortcutInfo> conflicts;
        if (!conflictsForKey(key, conflicts, error)) {
            fail();
            return;
        }
        for (const auto& info : conflicts) {
            if (info.componentUniqueName() == QStringLiteral("caelestia-activity")) {
                if (info.uniqueName() == m_action->objectName())
                    continue;
                error = QStringLiteral("Another Caelestia action already owns %1").arg(key.toString());
                fail();
                return;
            }
            QString component = info.componentUniqueName();
            if (!info.contextUniqueName().isEmpty())
                component += QLatin1Char('|') + info.contextUniqueName();
            const QStringList displaced { component, info.uniqueName(), info.componentFriendlyName(), info.friendlyName() };
            QList<QKeySequence> before;
            if (!readKeys(displaced, before, error)) {
                fail();
                return;
            }
            auto owned = before;
            owned.removeAll(key);
            if (owned == before)
                continue;
            if (!dispatcher->m_recovery->prepare(m_name, displaced, before, owned, false, error)) {
                fail();
                return;
            }
            // Journal precedes mutation. Check again so a change observed during
            // the write-ahead transaction cannot be silently overwritten.
            QList<QKeySequence> current;
            if (!readKeys(displaced, current, error) || current != before) {
                if (error.isEmpty())
                    error = QStringLiteral("Shortcut changed during acquisition");
                fail();
                return;
            }
            if (!writeKeys(displaced, owned, error) || !dispatcher->m_recovery->confirm(displaced, error)) {
                fail();
                return;
            }
            m_stolenShortcuts.append({ component, info.uniqueName(), before,
                info.componentFriendlyName(), info.friendlyName(), key });
        }
    }
    if (!keys.isEmpty()) {
        const bool registered = KGlobalAccel::self()->setShortcut(m_action, keys, KGlobalAccel::NoAutoloading);
        if (!dispatcher->m_recovery->confirm(id, error)) {
            fail();
            return;
        }
        QList<QKeySequence> actual;
        if (!registered || !readKeys(id, actual, error) || actual != keys) {
            if (error.isEmpty())
                error = QStringLiteral("Caelestia shortcut acquisition readback mismatch: %1").arg(m_name);
            fail();
            return;
        }
    }
    m_activeKeys = keys;
    rebuildCollisionIndex();
}
