pragma ComponentBehavior: Bound

import QtQuick
import org.kde.plasma.plasmoid
import org.kde.notificationmanager as NotificationManager
import org.kde.plasma.core as PlasmaCore
import plasma.applet.org.kde.plasma.notifications as Notifications

PlasmoidItem {
    id: root

    // All Plasma applets share PlasmaQuick::SharedQmlEngine. This is the
    // existing notification singleton, not a second notification model/server.
    readonly property var globals: Notifications.Globals
    readonly property var instantiator: globals.popupInstantiator

    NotificationManager.Notifications {
        id: history
        showExpired: true
        showDismissed: true
        showAddedDuringInhibition: true
        showJobs: false
        groupMode: NotificationManager.Notifications.GroupDisabled
    }

    Plasmoid.status: PlasmaCore.Types.HiddenStatus
    preferredRepresentation: fullRepresentation
    fullRepresentation: Item { implicitWidth: 1; implicitHeight: 1 }
    compactRepresentation: Item { implicitWidth: 1; implicitHeight: 1 }

    Binding {
        target: root.instantiator
        property: "active"
        when: Plasmoid.leased
        value: false
        restoreMode: Binding.RestoreBindingOrValue
        delayed: false
    }

    Connections {
        target: root.instantiator
        function onActiveChanged(): void { Plasmoid.checkPresentation(); }
    }
    Connections {
        target: root.globals
        function onInhibitedChanged(): void { Plasmoid.policyUpdated(); }
    }
    Connections {
        target: root.globals.notificationSettings
        function onSettingsChanged(): void { Plasmoid.policyUpdated(); }
    }

    Component.onCompleted: Plasmoid.attach(root.globals, history)
    Component.onDestruction: Plasmoid.detach()
}
