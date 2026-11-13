#pragma once

#include "common.hpp"
#include "objectnode.hpp"
#include "settingsfile.hpp"

namespace caelestia::settings {

class RootNode : public ObjectNode {
    Q_OBJECT

    Q_PROPERTY(QList<caelestia::settings::Diagnostic> diagnostics READ diagnostics NOTIFY diagnosticsChanged)

public:
    // A file root owns its batcher and path even when its QObject owner is another
    // root. Global fallback layers are defaults; only Monitor rejects global keys.
    explicit RootNode(const QString& path, RootNode* fallback, QObject* parent = nullptr,
        LayerScope scope = LayerScope::Inherit, SettingsFile::Access access = SettingsFile::Access::ReadWrite);

    [[nodiscard]] QList<Diagnostic> diagnostics() const;

    void load();
    Q_INVOKABLE void save();

signals:
    void diagnosticsChanged();
    void treeLoaded(RootNode* self);
    void treeLoadFailed(RootNode* self, const QString& error);
    void treeSaveFailed(RootNode* self, const QString& error);

private:
    SettingsFile* const m_file;
    QList<Diagnostic> m_diagnostics;

    void reloadFromFile();
    void saveToFile();
};

} // namespace caelestia::settings
