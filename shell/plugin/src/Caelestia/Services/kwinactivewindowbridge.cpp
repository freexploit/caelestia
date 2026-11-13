#include "kwinactivewindowbridge.hpp"

#include <qpa/qplatformnativeinterface.h>

#include <QDBusConnection>
#include <QDBusMessage>
#include <QGuiApplication>
#include <QScreen>
#include <QTimer>
#include <utility>

#include "kwinworkspacestate.hpp"
#include "plasmawindows.hpp"

namespace caelestia::services {

KWinActiveWindowBridge::KWinActiveWindowBridge(QObject* parent)
    : QObject(parent) {

    m_updateTimer.setSingleShot(true);
    m_updateTimer.setInterval(50);
    connect(&m_updateTimer, &QTimer::timeout, this, &KWinActiveWindowBridge::buildWindowList);

    auto* plasmaWindows = PlasmaWindows::instance();
    connect(plasmaWindows, &PlasmaWindows::windowAdded, this, &KWinActiveWindowBridge::onWindowAdded);
    connect(plasmaWindows, &PlasmaWindows::handleLost, this, &KWinActiveWindowBridge::onWindowLost);
    connect(plasmaWindows, &PlasmaWindows::eligibleWindowsChanged, this, &KWinActiveWindowBridge::buildWindowList);
    connect(plasmaWindows, &PlasmaWindows::currentActivityChanged, this, &KWinActiveWindowBridge::currentActivityChanged);
    for (auto* handle : plasmaWindows->handles()) {
        onWindowAdded(handle->uuid());
    }
    buildWindowList();

    // Clear any stale highlight from a previous session or crash on startup
    clearHighlight();
}

KWinActiveWindowBridge::~KWinActiveWindowBridge() {
    clearHighlight();
}

QVariantMap KWinActiveWindowBridge::activeWindow() const {
    return m_activeWindow;
}

QString KWinActiveWindowBridge::activeOutputName() const {
    return m_activeOutputName;
}

QString KWinActiveWindowBridge::highlightedAddress() const {
    return m_highlightedAddress;
}

void KWinActiveWindowBridge::setActiveOutputName(const QString& outputName) {
    if (m_activeOutputName != outputName) {
        m_activeOutputName = outputName;
        emit activeOutputNameChanged();
    }
}

QVariantList KWinActiveWindowBridge::windowList() const {
    return m_windowList;
}

QString KWinActiveWindowBridge::currentActivity() const {
    return PlasmaWindows::instance()->currentActivity();
}

bool KWinActiveWindowBridge::activityAvailable() const {
    return PlasmaWindows::instance()->activityAvailable();
}

bool KWinActiveWindowBridge::isWindowEligible(const QString& address) const {
    return PlasmaWindows::instance()->eligibleHandleFor(address) != nullptr;
}

QVariantList KWinActiveWindowBridge::windowsForWorkspace(const QVariant& workspace, bool includeOnAllWorkspaces) const {
    bool isInt = false;
    const int targetId = workspace.toInt(&isInt);
    const bool hasNumberTarget = isInt && targetId > 0;
    const QString targetUuid = workspace.toString();
    const bool hasStringTarget = !hasNumberTarget && !targetUuid.isEmpty();

    auto* wsState = KWinWorkspaceState::instance();

    QVariantList out;
    for (const QVariant& v : m_windowList) {
        const QVariantMap window = v.toMap();
        const QVariantMap ws = window.value(QStringLiteral("workspace")).toMap();
        if (ws.isEmpty()) {
            out.push_back(v);
            continue;
        }
        int id = ws.value(QStringLiteral("id")).toInt();
        const QString uuid = ws.value(QStringLiteral("uuid")).toString();

        if (id <= 0 && !uuid.isEmpty() && wsState) {
            const int resolved = wsState->indexForId(uuid);
            if (resolved > 0) {
                id = resolved;
            }
        }

        const bool onAll = (id == -1 || id == 0) && uuid.isEmpty();
        if (onAll) {
            if (includeOnAllWorkspaces)
                out.push_back(v);
            continue;
        }
        if (!hasNumberTarget && !hasStringTarget) {
            out.push_back(v);
            continue;
        }
        if (hasNumberTarget) {
            if (id > 0 && id == targetId) {
                out.push_back(v);
                continue;
            }
            if (!uuid.isEmpty() && wsState && uuid == wsState->uuidForIndex(targetId)) {
                out.push_back(v);
                continue;
            }
        }
        if (hasStringTarget) {
            if (!uuid.isEmpty() && uuid == targetUuid) {
                out.push_back(v);
                continue;
            }
            if (id > 0 && wsState && wsState->uuidForIndex(id) == targetUuid) {
                out.push_back(v);
                continue;
            }
        }
    }
    return out;
}

QString KWinActiveWindowBridge::pendingFocusAddress() const {
    return m_pendingFocusAddress;
}

QString KWinActiveWindowBridge::cursorOutputName() const {
    const auto message = QDBusMessage::createMethodCall(QStringLiteral("org.kde.KWin"), QStringLiteral("/KWin"),
        QStringLiteral("org.kde.KWin"), QStringLiteral("activeOutputName"));
    return QDBusConnection::sessionBus().call(message).arguments().value(0).toString();
}

void KWinActiveWindowBridge::onWindowAdded(const QString& uuid) {
    if (auto* handle = PlasmaWindows::instance()->handleFor(uuid)) {
        connect(handle, &PlasmaWindowHandle::titleChanged, this, &KWinActiveWindowBridge::scheduleWindowListUpdate);
        connect(handle, &PlasmaWindowHandle::appIdChanged, this, &KWinActiveWindowBridge::scheduleWindowListUpdate);
        connect(handle, &PlasmaWindowHandle::pidChanged, this, &KWinActiveWindowBridge::scheduleWindowListUpdate);
        connect(handle, &PlasmaWindowHandle::geometryChanged, this, &KWinActiveWindowBridge::scheduleWindowListUpdate);
        connect(handle, &PlasmaWindowHandle::stateChanged, this, [this, handle]() {
            onStateChanged(handle);
        });
        connect(handle, &PlasmaWindowHandle::desktopsChanged, this, &KWinActiveWindowBridge::scheduleWindowListUpdate);
        scheduleWindowListUpdate();
    }
}

void KWinActiveWindowBridge::onWindowLost(const QString& uuid) {
    if (!m_highlightedAddress.isEmpty() && m_highlightedAddress == uuid) {
        clearHighlight();
    }
    buildWindowList();
}

void KWinActiveWindowBridge::onStateChanged(PlasmaWindowHandle* handle) {
    if (!PlasmaWindows::instance()->isEligible(handle)) {
        buildWindowList();
        return;
    }
    if (!m_windowIndex.contains(handle->uuid())) {
        buildWindowList();
        return;
    }
    const QString uuid = handle->uuid();
    const bool isNowActive = handle->isActive();
    const bool wasActive = m_activeWindow.value(QStringLiteral("address")).toString() == uuid;

    if (isNowActive && !wasActive) {
        const QString oldUuid = m_activeWindow.value(QStringLiteral("address")).toString();
        if (!oldUuid.isEmpty()) {
            const int oldIdx = m_windowIndex.value(oldUuid, -1);
            if (oldIdx >= 0 && oldIdx < m_windowList.size()) {
                QVariantMap map = m_windowList[oldIdx].toMap();
                map[QStringLiteral("focused")] = false;
                map[QStringLiteral("floating")] = !map.value(QStringLiteral("fullscreen")).toBool() &&
                                                  !map.value(QStringLiteral("maximized")).toBool();
                m_windowList[oldIdx] = map;
            }
        }
        QVariantMap w = windowToVariant(handle);
        const int newIdx = m_windowIndex.value(uuid, -1);
        if (newIdx >= 0 && newIdx < m_windowList.size()) {
            m_windowList[newIdx] = w;
        }
        emit windowListChanged();
        if (m_activeWindow != w) {
            m_activeWindow = w;
            emit activeWindowChanged();
            const QString newOutput = w.value(QStringLiteral("output")).toString();
            if (!newOutput.isEmpty())
                setActiveOutputName(newOutput);
            if (m_activeWindow.value(QStringLiteral("address")).toString() == m_pendingFocusAddress) {
                m_pendingFocusAddress.clear();
                emit pendingFocusAddressChanged();
            }
        }
        return;
    }

    if (!isNowActive && wasActive) {
        const int idx = m_windowIndex.value(uuid, -1);
        if (idx >= 0 && idx < m_windowList.size()) {
            m_windowList[idx] = windowToVariant(handle);
        }
        m_activeWindow.clear();
        emit windowListChanged();
        emit activeWindowChanged();
        return;
    }

    scheduleWindowListUpdate();
}

void KWinActiveWindowBridge::scheduleWindowListUpdate() {
    if (!m_updateTimer.isActive()) {
        m_updateTimer.start();
    }
}

void KWinActiveWindowBridge::rebuildIndex() {
    m_windowIndex.clear();
    m_windowIndex.reserve(m_windowList.size());
    for (int i = 0; i < m_windowList.size(); ++i) {
        const QString addr = m_windowList[i].toMap().value(QStringLiteral("address")).toString();
        if (!addr.isEmpty())
            m_windowIndex.insert(addr, i);
    }
}

void KWinActiveWindowBridge::sendToOutput(const QString& address, const QString& outputName) {
    auto* handle = PlasmaWindows::instance()->eligibleHandleFor(address);
    auto* native = QGuiApplication::platformNativeInterface();
    if (!handle || !native || outputName.isEmpty()) {
        return;
    }
    for (auto* screen : QGuiApplication::screens()) {
        if (screen->name() == outputName) {
            if (auto* output = static_cast<wl_output*>(native->nativeResourceForScreen(QByteArrayLiteral("output"), screen))) {
                // Address this handle directly: a delayed global shortcut can move
                // a different window after focus or Activity changes.
                handle->send_to_output(output);
            }
            return;
        }
    }
}

QString KWinActiveWindowBridge::getOutputNameForGeometry(int x, int y, int w, int h) const {
    const QRect windowRect(x, y, w, h);

    QScreen* bestScreen = nullptr;
    int maxIntersectArea = 0;

    for (QScreen* screen : QGuiApplication::screens()) {
        const QRect intersect = screen->geometry().intersected(windowRect);
        const int area = intersect.width() * intersect.height();
        if (area > maxIntersectArea) {
            maxIntersectArea = area;
            bestScreen = screen;
        }
    }

    if (!bestScreen) {
        qreal bestDistance = -1;
        const QPoint windowCentre = windowRect.center();
        for (QScreen* screen : QGuiApplication::screens()) {
            const QPoint delta = screen->geometry().center() - windowCentre;
            const qreal distance =
                static_cast<qreal>(delta.x()) * delta.x() + static_cast<qreal>(delta.y()) * delta.y();
            if (bestDistance < 0 || distance < bestDistance) {
                bestDistance = distance;
                bestScreen = screen;
            }
        }
    }

    return bestScreen ? bestScreen->name() : QString();
}

QVariantMap KWinActiveWindowBridge::windowToVariant(PlasmaWindowHandle* w) const {
    QVariant desktopId = -1;
    QVariant desktopUuid = QString();
    if (!w->desktops().isEmpty()) {
        QString firstDesktop = w->desktops().first();
        bool ok;
        int parsed = firstDesktop.toInt(&ok);
        if (ok) {
            desktopId = parsed;
            if (auto wsState = KWinWorkspaceState::instance()) {
                QString resolvedUuid = wsState->uuidForIndex(parsed);
                desktopUuid = resolvedUuid.isEmpty() ? firstDesktop : resolvedUuid;
            } else {
                desktopUuid = firstDesktop;
            }
        } else {
            desktopUuid = firstDesktop;
            if (auto wsState = KWinWorkspaceState::instance()) {
                int idx = wsState->indexForId(firstDesktop);
                if (idx != -1)
                    desktopId = idx;
            }
        }
    }

    QVariantMap map = { { QStringLiteral("address"), w->uuid() }, { QStringLiteral("pid"), w->pid() },
        { QStringLiteral("title"), w->title() }, { QStringLiteral("class"), w->appId() },
        { QStringLiteral("x"), w->x() }, { QStringLiteral("y"), w->y() }, { QStringLiteral("width"), w->width() },
        { QStringLiteral("height"), w->height() }, { QStringLiteral("fullscreen"), w->isFullscreen() },
        { QStringLiteral("maximized"), w->isMaximized() }, { QStringLiteral("minimized"), w->isMinimized() },
        { QStringLiteral("focused"), w->isActive() },
        { QStringLiteral("activities"), w->activities() },
        { QStringLiteral("floating"), !w->isFullscreen() && !w->isMaximized() },
        { QStringLiteral("output"), getOutputNameForGeometry(w->x(), w->y(), w->width(), w->height()) },
        { QStringLiteral("workspace"),
            QVariantMap{ { QStringLiteral("id"), desktopId }, { QStringLiteral("uuid"), desktopUuid } } } };
    return map;
}

void KWinActiveWindowBridge::buildWindowList() {
    m_updateTimer.stop();
    auto* plasmaWindows = PlasmaWindows::instance();
    QVariantList newList;
    newList.reserve(plasmaWindows->handles().size());
    QVariantMap newActiveWindow;
    for (auto* handle : plasmaWindows->handles()) {
        if (!plasmaWindows->isEligible(handle)) {
            continue;
        }
        QVariantMap window = windowToVariant(handle);
        newList.append(window);
        if (handle->isActive()) {
            newActiveWindow = std::move(window);
        }
    }

    if (!m_pendingFocusAddress.isEmpty() &&
        (!isWindowEligible(m_pendingFocusAddress) ||
            newActiveWindow.value(QStringLiteral("address")).toString() == m_pendingFocusAddress)) {
        m_pendingFocusAddress.clear();
        emit pendingFocusAddressChanged();
    }
    if (!m_highlightedAddress.isEmpty() && !isWindowEligible(m_highlightedAddress)) {
        clearHighlight();
    }

    const bool listChanged = newList != m_windowList;
    const bool activeChanged = newActiveWindow != m_activeWindow;
    if (listChanged) {
        m_windowList = std::move(newList);
        rebuildIndex();
    }
    if (activeChanged) {
        m_activeWindow = std::move(newActiveWindow);
        const auto output = m_activeWindow.value(QStringLiteral("output")).toString();
        if (!output.isEmpty()) {
            setActiveOutputName(output);
        }
    }
    if (listChanged) {
        emit windowListChanged();
    }
    if (activeChanged) {
        emit activeWindowChanged();
    }
}

void KWinActiveWindowBridge::focusWindow(const QString& address) {
    if (!m_highlightedAddress.isEmpty()) {
        clearHighlight();
    }
    if (auto* handle = PlasmaWindows::instance()->eligibleHandleFor(address)) {
        m_pendingFocusAddress = handle->uuid();
        emit pendingFocusAddressChanged();

        handle->setState(QtWayland::org_kde_plasma_window_management::state_minimized, false);
        handle->setState(QtWayland::org_kde_plasma_window_management::state_active, true);
    }
}

void KWinActiveWindowBridge::closeWindow(const QString& address) {
    if (!m_highlightedAddress.isEmpty() && m_highlightedAddress == address) {
        clearHighlight();
    }
    if (auto* handle = PlasmaWindows::instance()->eligibleHandleFor(address)) {
        handle->close();
    }
}

void KWinActiveWindowBridge::minimizeWindow(const QString& address) {
    if (auto* handle = PlasmaWindows::instance()->eligibleHandleFor(address)) {
        handle->setState(QtWayland::org_kde_plasma_window_management::state_minimized, true);
    }
}

void KWinActiveWindowBridge::maximizeWindow(const QString& address, bool horz, bool vert) {
    if (auto* handle = PlasmaWindows::instance()->eligibleHandleFor(address)) {
        const auto max = QtWayland::org_kde_plasma_window_management::state_maximized;
        handle->setState(max, horz || vert);
    }
}

void KWinActiveWindowBridge::raiseWindow(const QString& address) {
    focusWindow(address);
}

void KWinActiveWindowBridge::setWindowProperty(const QString& address, const QString& property, bool enable) {
    if (auto* handle = PlasmaWindows::instance()->eligibleHandleFor(address)) {
        uint32_t state = 0;
        if (property == QStringLiteral("keep_above"))
            state = QtWayland::org_kde_plasma_window_management::state_keep_above;
        else if (property == QStringLiteral("keep_below"))
            state = QtWayland::org_kde_plasma_window_management::state_keep_below;
        else if (property == QStringLiteral("skip_taskbar"))
            state = QtWayland::org_kde_plasma_window_management::state_skiptaskbar;
        else if (property == QStringLiteral("demands_attention"))
            state = QtWayland::org_kde_plasma_window_management::state_demands_attention;

        if (state != 0) {
            handle->setState(state, enable);
        }
    }
}

void KWinActiveWindowBridge::setWindowDesktop(const QString& address, int desktopId) {
    if (auto* handle = PlasmaWindows::instance()->eligibleHandleFor(address)) {
        if (auto wsState = KWinWorkspaceState::instance()) {
            QString uuid = wsState->uuidForIndex(desktopId);
            if (!uuid.isEmpty()) {
                QStringList currentDesktops = handle->desktops();
                for (const QString& oldUuid : currentDesktops) {
                    if (oldUuid != uuid) {
                        handle->request_leave_virtual_desktop(oldUuid);
                    }
                }
                handle->request_enter_virtual_desktop(uuid);
            }
        }
    }
}

void KWinActiveWindowBridge::setFullscreen(const QString& address, bool fullscreen) {
    if (auto* handle = PlasmaWindows::instance()->eligibleHandleFor(address)) {
        handle->setState(QtWayland::org_kde_plasma_window_management::state_fullscreen, fullscreen);
    }
}

void KWinActiveWindowBridge::setMaximized(const QString& address, bool maximized) {
    if (auto* handle = PlasmaWindows::instance()->eligibleHandleFor(address)) {
        handle->setState(QtWayland::org_kde_plasma_window_management::state_maximized, maximized);
    }
}

void KWinActiveWindowBridge::highlightWindow(const QString& address) {
    if (!address.isEmpty() && !isWindowEligible(address)) {
        return;
    }
    if (address == m_highlightedAddress && !address.isEmpty()) {
        return;
    }
    m_highlightedAddress = address;
    emit highlightedAddressChanged();

    auto msg =
        QDBusMessage::createMethodCall(QStringLiteral("org.kde.KWin"), QStringLiteral("/org/kde/KWin/HighlightWindow"),
            QStringLiteral("org.kde.KWin.HighlightWindow"), QStringLiteral("highlightWindows"));
    QStringList list;
    if (!address.isEmpty()) {
        list << address;
    }
    msg << list;
    QDBusConnection::sessionBus().send(msg);
}

void KWinActiveWindowBridge::clearHighlight() {
    if (!m_highlightedAddress.isEmpty()) {
        highlightWindow(QString());
    } else {
        auto msg = QDBusMessage::createMethodCall(QStringLiteral("org.kde.KWin"),
            QStringLiteral("/org/kde/KWin/HighlightWindow"), QStringLiteral("org.kde.KWin.HighlightWindow"),
            QStringLiteral("highlightWindows"));
        msg << QStringList();
        QDBusConnection::sessionBus().send(msg);
    }
}

void KWinActiveWindowBridge::refreshWindows() {
    if (auto* plasmaWindows = PlasmaWindows::instance()) {
        plasmaWindows->refresh();
    }
    scheduleWindowListUpdate();
}

} // namespace caelestia::services
