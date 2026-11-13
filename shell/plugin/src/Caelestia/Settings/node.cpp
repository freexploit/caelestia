#include "node.hpp"

namespace caelestia::settings {

Node::Node(Node* fallback, QObject* parent, bool globalOnly, LayerScope scope)
    : QObject(parent)
    , m_isRoot(scope != LayerScope::Inherit)
    , m_monitorLayer(scope == LayerScope::Monitor ||
                     (scope == LayerScope::Inherit &&
                         (parentNode() ? parentNode()->isMonitorLayer() : fallback != nullptr)))
    , m_globalOnly(globalOnly || (parentNode() && parentNode()->m_globalOnly))
    , m_rootNode(parentNode() ? parentNode()->rootNode() : this)
    , m_fallbackNode(fallback)
    , m_writeOrigin(WriteOrigin::Qml)
    , m_internalRead(false)
    , m_batcher(m_rootNode == this ? new ChangeBatcher(this) : nullptr) {
    // Monitor layers do not mirror global-only values; global defaults do.
    if (fallback && (!m_globalOnly || !isMonitorLayer()))
        QObject::connect(fallback, &Node::optionChanged, this, &Node::onFallbackNotify);
}

QString Node::key() const {
    return parentNode() ? parentNode()->keyOf(this) : QString();
}

QString Node::path() const {
    return parentNode() ? parentNode()->pathFor(key()) : key();
}

QString Node::pathFor(const QString& key) const {
    const auto p = path();
    return p.isEmpty() ? key : p + QLatin1Char('.') + key;
}

Node* Node::parentNode() const {
    return m_isRoot ? nullptr : qobject_cast<Node*>(parent());
}

Node* Node::rootNode() const {
    return m_rootNode;
}

Node* Node::fallbackNode() const {
    return m_fallbackNode;
}

bool Node::isMonitorLayer() const {
    return m_monitorLayer;
}

void Node::detachFallback() {
    if (m_fallbackNode) {
        QObject::disconnect(m_fallbackNode, nullptr, this, nullptr);
        m_fallbackNode = nullptr;
    }

    const auto childNodes = findChildren<Node*>(Qt::FindDirectChildrenOnly);
    for (auto* const child : childNodes)
        child->detachFallback();
}

bool Node::isGlobalOnly() const {
    return m_globalOnly;
}

bool Node::isOverride(const QString& key) const {
    return m_overrides.contains(key);
}

QStringList Node::overrides() const {
    return m_overrides.values();
}

bool Node::hasContent() const {
    return !m_overrides.isEmpty() || m_quarantine ||
           std::ranges::any_of(findChildren<Node*>(Qt::FindDirectChildrenOnly), &Node::hasContent);
}

QVariant Node::value(const QString& key) const {
    const auto* desc = schema().get(key);
    if (!desc) {
        qCCritical(
            lcSettings, "Attempted to read an unknown key %s, something is wrong.", qUtf8Printable(pathFor(key)));
        return QVariant();
    }

    const InternalRead guard(m_rootNode);
    return metaObject()->property(desc->metaIndex).read(this);
}

bool Node::setValue(const QString& key, const QVariant& value) {
    const auto* desc = schema().get(key);
    if (!desc) {
        qCCritical(lcSettings, "Attempted to set an unknown key %s, something is wrong.", qUtf8Printable(pathFor(key)));
        return false;
    }

    if (desc->isNode) {
        qCCritical(lcSettings, "Attempted to set node %s directly, something is wrong.", qUtf8Printable(pathFor(key)));
        return false;
    }

    auto coerced = value;
    if (!desc->accepts(coerced.metaType())) {
        const auto target = desc->coercionTarget(coerced.metaType());
        if (!target.isValid() || !coerced.convert(target)) {
            qCWarning(lcSettings, "Type mismatch for %s, expected %s got %s", qUtf8Printable(pathFor(key)),
                qUtf8Printable(desc->typeString()), value.metaType().name());
            return false;
        }
    }

    return metaObject()->property(desc->metaIndex).write(this, coerced);
}

void Node::resetToDefaults() {
    if (m_globalOnly && isMonitorLayer())
        return;

    if (m_quarantine && writeOrigin() == WriteOrigin::QmlReset)
        batcher()->dirty();
    m_quarantine.reset();

    for (const auto& desc : schema().descriptors()) {
        if (desc.isNode)
            value(desc.key).value<Node*>()->resetToDefaults();
        else if (!desc.globalOnly() || !isMonitorLayer())
            setValue(desc.key, m_fallbackNode ? m_fallbackNode->value(desc.key) : desc.defaultValue());
    }
}

const Quarantine* Node::quarantine() const {
    return m_quarantine.get();
}

bool Node::rejectInvalidWrite(const QString& key, const QVariant& value) const {
    const auto* desc = schema().get(key);
    if (!desc || desc->accepts(value.metaType()))
        return false;

    qCWarning(lcSettings, "Type mismatch for %s, expected %s got %s", qUtf8Printable(pathFor(key)),
        qUtf8Printable(desc->typeString()), value.metaType().name());
    return true;
}

void Node::warnGlobalRead(const QString& key) const {
    if (!isMonitorLayer() || m_rootNode->m_internalRead)
        return;

    qCWarning(lcSettings,
        "Global option %s was read from an overlay layer. "
        "This should not be used, read global options from the global layer instead.",
        qUtf8Printable(pathFor(key)));
}

bool Node::rejectGlobalWrite(const QString& key) {
    const auto* desc = schema().get(key);
    if (!desc) {
        qCCritical(lcSettings, "Attempted to check a write for an unknown key %s, something is seriously wrong...",
            qUtf8Printable(pathFor(key)));
        return false;
    }

    const auto origin = m_rootNode->m_writeOrigin;
    const auto fromUser = origin == WriteOrigin::Qml || origin == WriteOrigin::QmlReset;

    if ((!m_globalOnly && !desc->globalOnly()) || !fromUser || !isMonitorLayer())
        return false;

    // Overlays cannot write the global layer, whatever the origin
    if (origin == WriteOrigin::QmlReset)
        qCWarning(lcSettings,
            "Attempted to reset global option %s from an overlay layer, ignoring. "
            "This should not be used, reset global options from the global layer instead.",
            qUtf8Printable(pathFor(key)));
    else
        qCWarning(lcSettings,
            "Attempted to write global option %s from an overlay layer, ignoring. "
            "This should not be used, write global options from the global layer instead.",
            qUtf8Printable(pathFor(key)));

    return true;
}

void Node::warnGlobalSync(QList<Diagnostic>& diagnostics, const QString& path) {
    qCWarning(lcSettings, "Global option definition %s found in overlay file, ignoring.", qUtf8Printable(path));
    diagnostics << Diagnostic{
        .type = DiagnosticType::GlobalOption,
        .option = path,
        .message = QStringLiteral("Global options should not be defined in overlay files"),
    };
}

bool Node::rejectGlobalSync(QList<Diagnostic>& diagnostics) const {
    if (!m_globalOnly || !isMonitorLayer())
        return false;

    warnGlobalSync(diagnostics, path());
    return true;
}

bool Node::recordWrite(const QString& key, bool changed) {
    const auto* desc = schema().get(key);
    if (!desc) {
        qCCritical(lcSettings, "Attempted to record a write for an unknown key %s, something is seriously wrong...",
            qUtf8Printable(pathFor(key)));
        return false;
    }

    const auto origin = m_rootNode->m_writeOrigin;
    const auto fromUser = origin == WriteOrigin::Qml || origin == WriteOrigin::QmlReset;

    bool dirty = changed;
    bool overrideSetChanged = false;
    switch (origin) {
    case WriteOrigin::Init:
        return false;

    case WriteOrigin::File:
    case WriteOrigin::Qml:
        overrideSetChanged = !m_overrides.contains(key);
        m_overrides << key;
        dirty |= overrideSetChanged;
        break;

    case WriteOrigin::Layer:
        break;

    case WriteOrigin::FileReset:
    case WriteOrigin::QmlReset:
        overrideSetChanged = m_overrides.remove(key);
        dirty |= overrideSetChanged;
        break;
    }

    if (fromUser)
        dirty |= removeQuarantined(key);

    if (fromUser && dirty)
        m_rootNode->m_batcher->dirty();

    if (overrideSetChanged)
        emit overridesChanged();

    if (changed)
        emit optionChanged(key);

    return changed;
}

bool Node::removeQuarantined(const QString& key) {
    if (!m_quarantine)
        return false;

    const auto removed = m_quarantine->remove(key);
    if (m_quarantine->isEmpty())
        m_quarantine.reset();

    return removed;
}

ChangeBatcher* Node::batcher() const {
    return m_rootNode->m_batcher;
}

WriteOrigin Node::writeOrigin() const {
    return m_rootNode->m_writeOrigin;
}

QString Node::keyOf(const Node* child) const {
    for (const auto& desc : schema().descriptors()) {
        if (desc.isNode && child == value(desc.key).value<Node*>())
            return desc.key;
    }

    return QString();
}

void Node::onFallbackNotify(const QString& key) {
    if (m_overrides.contains(key))
        return;

    const auto* desc = schema().get(key);
    if (desc && desc->globalOnly() && isMonitorLayer())
        return;

    const WriteScope scope(this, WriteOrigin::Layer);
    setValue(key, m_fallbackNode->value(key));
}

} // namespace caelestia::settings
