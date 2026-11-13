pragma Singleton
pragma ComponentBehavior: Bound

import QtQuick
import Quickshell
import Quickshell.Io
import qs.services

Singleton {
    id: root

    readonly property bool managed: Quickshell.env("CAELESTIA_ACTIVITY_MANAGED") === "1"
    property bool surfaceActive: !managed
    readonly property bool ready: !managed || (Kwin.activityAvailable && Kwin.currentActivity.length > 0
        && Screens.screens.length > 0
        && Screens.screens.every(s => ShellState.componentsFor(s)?.rootWindow != null)
        && Notifs.managedReady)

    function snapshot(): string {
        return JSON.stringify({ready: ready, active: surfaceActive, activity: Kwin.currentActivity,
            notificationReady: Notifs.managedReady, notificationError: Notifs.managedError,
            outputs: Screens.screens.map(s => ({name: s.name,
                constructed: ShellState.componentsFor(s)?.rootWindow != null,
                visible: ShellState.componentsFor(s)?.rootWindow?.visible ?? false}))});
    }

    IpcHandler {
        target: "managed"
        function status(): string { return root.snapshot(); }
        function commit(): bool {
            if (!root.ready || !Notifs.setManagedActive(true))
                return false;
            root.surfaceActive = true;
            return true;
        }
        function release(): bool {
            root.surfaceActive = false;
            return Notifs.setManagedActive(false);
        }
    }
}
