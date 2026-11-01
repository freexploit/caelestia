// SPDX-License-Identifier: GPL-3.0-only
#include "../lib/shortcutrecovery.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("caelestia-shortcut-recovery"));
    auto arguments = app.arguments();
    arguments.removeFirst();
    if (arguments.isEmpty() || (arguments.first() != QStringLiteral("recover")
        && arguments.first() != QStringLiteral("status"))) {
        QTextStream(stderr) << "Usage: caelestia-shortcut-recovery <recover|status> [--journal PATH]\n";
        return 2;
    }
    const auto command = arguments.takeFirst();
    QString path = caelestia::shortcuts::journalPath();
    if (!arguments.isEmpty()) {
        if (arguments.size() != 2 || arguments[0] != QStringLiteral("--journal") || arguments[1].isEmpty()) {
            QTextStream(stderr) << "Expected --journal PATH\n";
            return 2;
        }
        path = arguments[1];
    }
    // An unused integration should not need a running desktop or session bus.
    if (!QFile::exists(path) && !QFile::exists(path + QStringLiteral(".lock"))) {
        QTextStream(stdout) << "{\"ok\":true,\"pending\":0}\n";
        return 0;
    }
    caelestia::shortcuts::Recovery recovery(path);
    QString error;
    bool ok = recovery.open(error);
    if (ok && command == QStringLiteral("recover"))
        ok = recovery.restoreAll(error);
    QJsonObject result { { QStringLiteral("ok"), ok }, { QStringLiteral("pending"), recovery.pending() },
        { QStringLiteral("error"), error }, { QStringLiteral("journal"), path } };
    if (command == QStringLiteral("status"))
        result.insert(QStringLiteral("records"), recovery.records());
    QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Compact) << '\n';
    return ok ? 0 : 1;
}
