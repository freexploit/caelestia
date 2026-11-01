import QtQuick
import Quickshell
import Quickshell.Wayland
import Caelestia.Config
import qs.services

// qmllint disable uncreatable-type
PanelWindow {
    // qmllint enable uncreatable-type
    id: root
    required property string name

    property bool isDesktopWidget: false
    // KWin maps a layer surface's namespace to a window type. "dock"/"desktop"
    // windows are placed on all virtual desktops; anything else (including
    // "panel") is pinned to the desktop where it was created, which also pins
    // its strut there.
    property string layerNamespace: isDesktopWidget ? "desktop" : "panel"
    WlrLayershell.namespace: layerNamespace

    color: "transparent"

    contentItem.Config.screen: screen.name
    contentItem.Tokens.screen: screen.name

    Binding {
        target: root
        property: "visible"
        value: false
        when: ManagedSession.managed && !ManagedSession.surfaceActive
        restoreMode: Binding.RestoreBindingOrValue
    }
}
