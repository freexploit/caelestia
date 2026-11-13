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
    WlrLayershell.namespace: isDesktopWidget ? "desktop" : "panel"

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
