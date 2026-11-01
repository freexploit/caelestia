// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <KGlobalShortcutInfo>
#include <QJsonArray>
#include <QKeySequence>
#include <QLockFile>
#include <QStringList>
#include <memory>

namespace caelestia::shortcuts {

QString journalPath();
QList<QKeySequence> normalized(const QList<QKeySequence>& keys);
bool readKeys(const QStringList& actionId, QList<QKeySequence>& keys, QString& error);
bool writeKeys(const QStringList& actionId, const QList<QKeySequence>& keys, QString& error);
bool conflictsForKey(const QKeySequence& key, QList<KGlobalShortcutInfo>& conflicts, QString& error);

// The shell holds the lock until it exits. Recovery therefore cannot race a live
// shell, but can acquire the stale lock after SIGKILL without starting its UI.
class Recovery {
public:
    explicit Recovery(QString path = journalPath());
    bool open(QString& error);
    bool prepare(const QString& owner, const QStringList& actionId, const QList<QKeySequence>& before,
        const QList<QKeySequence>& owned, bool shellAction, QString& error);
    bool restore(const QString& owner, QString& error);
    bool confirm(const QStringList& actionId, QString& error);
    bool restoreAll(QString& error);
    int pending() const;
    const QJsonArray& records() const { return m_records; }

private:
    bool save(QString& error);
    bool restoreMatching(const QString& owner, bool all, QString& error);
    QString m_path;
    std::unique_ptr<QLockFile> m_lock;
    QJsonArray m_records;
    bool m_open = false;
};

} // namespace caelestia::shortcuts
