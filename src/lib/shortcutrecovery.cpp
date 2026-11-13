// SPDX-License-Identifier: GPL-3.0-only
#include "shortcutrecovery.hpp"

#include <KGlobalAccel>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QStandardPaths>
#include <fcntl.h>
#include <unistd.h>
#include <utility>

namespace caelestia::shortcuts {
namespace {
QJsonArray encodeKeys(const QList<QKeySequence>& keys) {
    QJsonArray result;
    for (const auto& key : normalized(keys))
        result.append(key.toString(QKeySequence::PortableText));
    return result;
}

QList<QKeySequence> decodeKeys(const QJsonArray& keys) {
    QList<QKeySequence> result;
    for (const auto& key : keys)
        result.append(QKeySequence::fromString(key.toString(), QKeySequence::PortableText));
    return normalized(result);
}

QStringList actionId(const QJsonObject& record) {
    QStringList result;
    for (const auto& value : record.value(QStringLiteral("actionId")).toArray())
        result.append(value.toString());
    return result;
}

bool syncDirectory(const QString& path, QString& error) {
    const QByteArray directory = QFile::encodeName(QFileInfo(path).absolutePath());
    const int fd = ::open(directory.constData(), O_DIRECTORY | O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        error = QStringLiteral("Cannot open shortcut journal directory for durability");
        return false;
    }
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
    if (!ok)
        error = QStringLiteral("Cannot sync shortcut journal directory");
    return ok;
}

QDBusMessage call(const QString& method, const QVariantList& arguments) {
    // KGlobalAccel registers its QKeySequence DBus metatypes. This does not
    // register an action or require a graphical application in the helper.
    KGlobalAccel::self();
    auto message = QDBusMessage::createMethodCall(QStringLiteral("org.kde.kglobalaccel"),
        QStringLiteral("/kglobalaccel"), QStringLiteral("org.kde.KGlobalAccel"), method);
    message.setArguments(arguments);
    return QDBusConnection::sessionBus().call(message, QDBus::Block, 5000);
}
} // namespace

QString journalPath() {
    const QString override = qEnvironmentVariable("CAELESTIA_SHORTCUT_JOURNAL");
    if (!override.isEmpty())
        return override;
    return QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation)
        + QStringLiteral("/caelestia/shortcuts.json");
}

QList<QKeySequence> normalized(const QList<QKeySequence>& keys) {
    QList<QKeySequence> result;
    result.reserve(keys.size());
    for (const auto& key : keys) {
        if (!key.isEmpty() && !result.contains(key))
            result.append(key);
    }
    return result;
}

bool conflictsForKey(const QKeySequence& key, QList<KGlobalShortcutInfo>& conflicts, QString& error) {
    QDBusReply<QList<KGlobalShortcutInfo>> reply = call(QStringLiteral("globalShortcutsByKey"),
        { QVariant::fromValue(key), QVariant::fromValue(KGlobalAccel::Equal) });
    if (!reply.isValid()) {
        error = QStringLiteral("Querying shortcut conflicts: %1").arg(reply.error().message());
        return false;
    }
    conflicts = reply.value();
    return true;
}

bool readKeys(const QStringList& id, QList<QKeySequence>& keys, QString& error) {
    QDBusReply<QList<QKeySequence>> reply = call(QStringLiteral("shortcutKeys"), { id });
    if (!reply.isValid()) {
        error = QStringLiteral("Reading %1/%2: %3").arg(id.value(0), id.value(1), reply.error().message());
        return false;
    }
    keys = normalized(reply.value());
    return true;
}

bool writeKeys(const QStringList& id, const QList<QKeySequence>& keys, QString& error) {
    // Foreign setters notify the live owning client, unlike setShortcutKeys
    // alone. Never steal systemwide or clear an unrelated alternative binding.
    const auto expected = normalized(keys);
    const auto reply = call(QStringLiteral("setForeignShortcutKeys"), { id, QVariant::fromValue(expected) });
    if (reply.type() == QDBusMessage::ErrorMessage) {
        error = QStringLiteral("Writing %1/%2: %3").arg(id.value(0), id.value(1), reply.errorMessage());
        return false;
    }
    QList<QKeySequence> actual;
    if (!readKeys(id, actual, error))
        return false;
    if (actual != expected) {
        error = QStringLiteral("Shortcut readback mismatch for %1/%2").arg(id.value(0), id.value(1));
        return false;
    }
    return true;
}

Recovery::Recovery(QString path)
    : m_path(std::move(path)) {}

bool Recovery::open(QString& error) {
    if (m_open)
        return true;
    const QFileInfo info(m_path);
    if (!QDir().mkpath(info.absolutePath())) {
        error = QStringLiteral("Cannot create shortcut journal directory");
        return false;
    }
    m_lock = std::make_unique<QLockFile>(m_path + QStringLiteral(".lock"));
    // Do not mistake a slow live shell for a stale lock; QLockFile still checks
    // whether the recorded PID exists and reclaims dead-process locks.
    m_lock->setStaleLockTime(0);
    if (!m_lock->tryLock()) {
        error = QStringLiteral("Shortcut journal is locked by a running shell or recovery");
        return false;
    }
    QFile file(m_path);
    if (file.exists()) {
        if (!file.open(QIODevice::ReadOnly)) {
            error = file.errorString();
            return false;
        }
        QJsonParseError parse;
        const auto document = QJsonDocument::fromJson(file.readAll(), &parse);
        if (parse.error != QJsonParseError::NoError || !document.isObject()
            || document.object().value(QStringLiteral("version")).toInt() != 1
            || !document.object().value(QStringLiteral("records")).isArray()) {
            error = QStringLiteral("Invalid shortcut recovery journal; retained without mutation");
            return false;
        }
        m_records = document.object().value(QStringLiteral("records")).toArray();
        for (const auto& value : m_records) {
            const auto record = value.toObject();
            const auto id = actionId(record);
            if (id.size() != 4 || id[0].isEmpty() || id[1].isEmpty()
                || !record.value(QStringLiteral("before")).isArray()
                || !record.value(QStringLiteral("owned")).isArray()
                || !record.value(QStringLiteral("owners")).isObject()) {
                error = QStringLiteral("Invalid shortcut recovery record; retained without mutation");
                return false;
            }
            const auto validKeys = [](const QJsonArray& values) {
                for (const auto& key : values) {
                    if (!key.isString() || QKeySequence::fromString(key.toString(), QKeySequence::PortableText).isEmpty())
                        return false;
                }
                return true;
            };
            if (!validKeys(record.value(QStringLiteral("before")).toArray())
                || !validKeys(record.value(QStringLiteral("owned")).toArray())) {
                error = QStringLiteral("Invalid shortcut key in recovery journal; retained without mutation");
                return false;
            }
            const auto owners = record.value(QStringLiteral("owners")).toObject();
            for (auto it = owners.constBegin(); it != owners.constEnd(); ++it) {
                if (!it.value().isArray() || !validKeys(it.value().toArray())) {
                    error = QStringLiteral("Invalid shortcut owner in recovery journal; retained without mutation");
                    return false;
                }
            }
        }
    }
    m_open = true;
    return true;
}

bool Recovery::save(QString& error) {
    if (m_records.isEmpty()) {
        if (QFile::exists(m_path) && !QFile::remove(m_path)) {
            error = QStringLiteral("Cannot remove completed shortcut journal");
            return false;
        }
        return syncDirectory(m_path, error);
    }
    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly)) {
        error = file.errorString();
        return false;
    }
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        error = QStringLiteral("Cannot restrict shortcut journal permissions");
        return false;
    }
    const auto bytes = QJsonDocument(QJsonObject { { QStringLiteral("version"), 1 },
        { QStringLiteral("records"), m_records } }).toJson(QJsonDocument::Compact);
    if (file.write(bytes) != bytes.size() || !file.flush() || ::fsync(file.handle()) != 0) {
        error = QStringLiteral("Cannot durably write shortcut journal");
        return false;
    }
    if (!file.commit()) {
        error = file.errorString();
        return false;
    }
    return syncDirectory(m_path, error);
}

bool Recovery::prepare(const QString& owner, const QStringList& id, const QList<QKeySequence>& before,
    const QList<QKeySequence>& owned, bool shellAction, QString& error) {
    if (!m_open || owner.isEmpty() || id.size() != 4) {
        error = QStringLiteral("Shortcut journal is not ready");
        return false;
    }
    const auto previous = m_records;
    int index = -1;
    for (int i = 0; i < m_records.size(); ++i) {
        if (actionId(m_records[i].toObject()).mid(0, 2) == id.mid(0, 2)) {
            index = i;
            break;
        }
    }
    QJsonObject record = index < 0 ? QJsonObject {} : m_records[index].toObject();
    if (index < 0) {
        record.insert(QStringLiteral("actionId"), QJsonArray::fromStringList(id));
        record.insert(QStringLiteral("before"), encodeKeys(before));
        record.insert(QStringLiteral("shell"), shellAction);
    } else if (!shellAction && normalized(before) != decodeKeys(record.value(QStringLiteral("owned")).toArray())) {
        error = QStringLiteral("Shortcut changed outside Caelestia during acquisition: %1/%2").arg(id[0], id[1]);
        return false;
    }
    auto owners = record.value(QStringLiteral("owners")).toObject();
    auto removed = decodeKeys(owners.value(owner).toArray());
    for (const auto& key : normalized(before)) {
        if (!owned.contains(key) && !removed.contains(key))
            removed.append(key);
    }
    owners.insert(owner, encodeKeys(removed));
    record.insert(QStringLiteral("owners"), owners);
    if (index >= 0)
        record.insert(QStringLiteral("accepted"), QJsonArray { record.value(QStringLiteral("owned")) });
    record.insert(QStringLiteral("owned"), encodeKeys(owned));
    if (index < 0)
        m_records.append(record);
    else
        m_records[index] = record;
    if (save(error))
        return true;
    m_records = previous;
    return false;
}

bool Recovery::restore(const QString& owner, QString& error) {
    return restoreMatching(owner, false, error);
}

bool Recovery::confirm(const QStringList& id, QString& error) {
    QList<QKeySequence> actual;
    if (!readKeys(id, actual, error))
        return false;
    for (int i = 0; i < m_records.size(); ++i) {
        auto record = m_records[i].toObject();
        if (actionId(record).mid(0, 2) != id.mid(0, 2))
            continue;
        const auto intended = decodeKeys(record.value(QStringLiteral("owned")).toArray());
        if (actual != intended) {
            error = QStringLiteral("Shortcut changed or registration was partial before acknowledgment");
            return false;
        }
        record.insert(QStringLiteral("owned"), encodeKeys(actual));
        record.remove(QStringLiteral("accepted"));
        m_records[i] = record;
        return save(error);
    }
    error = QStringLiteral("Shortcut registration has no before-change journal");
    return false;
}

bool Recovery::restoreAll(QString& error) {
    return restoreMatching({}, true, error);
}

int Recovery::pending() const {
    return m_records.size();
}

bool Recovery::restoreMatching(const QString& owner, bool all, QString& error) {
    if (!m_open) {
        error = QStringLiteral("Shortcut journal is not ready");
        return false;
    }
    // First release shell actions, then return baseline chords. A helper after
    // SIGKILL follows the same ordering as a clean shell exit.

    bool ok = true;
    for (const bool shell : { true, false }) {
        for (int i = m_records.size() - 1; i >= 0; --i) {
            auto record = m_records[i].toObject();
            auto owners = record.value(QStringLiteral("owners")).toObject();
            if (record.value(QStringLiteral("shell")).toBool() != shell || (!all && !owners.contains(owner)))
                continue;
            if (all)
                owners = {};
            else
                owners.remove(owner);
            auto target = decodeKeys(record.value(QStringLiteral("before")).toArray());
            if (!shell) {
                for (auto it = owners.constBegin(); it != owners.constEnd(); ++it) {
                    for (const auto& key : decodeKeys(it.value().toArray()))
                        target.removeAll(key);
                }
            }
            const auto id = actionId(record);
            QList<QKeySequence> current;
            QString failure;
            if (!readKeys(id, current, failure)) {
                ok = false;
                error = failure;
                continue;
            }
            const auto expected = decodeKeys(record.value(QStringLiteral("owned")).toArray());
            bool owned = current == expected;
            for (const auto& accepted : record.value(QStringLiteral("accepted")).toArray())
                owned = owned || current == decodeKeys(accepted.toArray());
            if (owned && current != target) {
                // Persist the restore value too: SIGKILL between its write and
                // journal commit must still leave an owned value for recovery.
                auto accepted = record.value(QStringLiteral("accepted")).toArray();
                accepted.append(encodeKeys(target));
                record.insert(QStringLiteral("accepted"), accepted);
                m_records[i] = record;
                if (!save(failure)) {
                    ok = false;
                    error = failure;
                    continue;
                }
                QList<QKeySequence> latest;
                if (!readKeys(id, latest, failure)) {
                    ok = false;
                    error = failure;
                    continue;
                }
                if (latest != current) {
                    // An editor changed it while the durable transaction was
                    // being recorded. Reconcile that value on the next pass.
                    ok = false;
                    error = QStringLiteral("Shortcut changed during restoration; retry recovery");
                    continue;
                }
                if (!writeKeys(id, target, failure)) {
                    ok = false;
                    error = failure;
                    continue;
                }
            }
            // A different value belongs to a later editor, not this integration.
            // Retire the claim without writing it, including on crash recovery.
            const auto previous = m_records;
            if (owners.isEmpty() || !owned) {
                m_records.removeAt(i);
            } else {
                record.insert(QStringLiteral("owners"), owners);
                record.insert(QStringLiteral("owned"), encodeKeys(target));
                record.remove(QStringLiteral("accepted"));
                m_records[i] = record;
            }
            if (!save(failure)) {
                m_records = previous;
                ok = false;
                error = failure;
            }
        }
    }
    return ok;
}
} // namespace caelestia::shortcuts
