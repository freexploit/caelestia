pragma Singleton
pragma ComponentBehavior: Bound

import QtQuick
import Quickshell
import Quickshell.Io
import Quickshell.Services.Notifications
import Caelestia
import Caelestia.Config
import Caelestia.Services
import qs.components.misc
import qs.services
import qs.utils

Singleton {
    id: root

    property list<NotifData> list: []
    property int openCount: 0
    property int popupCount: 0

    readonly property int notifCap: Math.max(1, GlobalConfig.notifs.maxNotifs)

    property alias dnd: props.dnd
    property string lastSavedState: ""

    property string activeTargetOutput: ""
    property bool loaded

    readonly property bool activityManaged: Quickshell.env("CAELESTIA_ACTIVITY_MANAGED") === "1"
    readonly property bool managedReady: !activityManaged || plasma.ready
    readonly property bool managedActive: activityManaged && plasma.active
    readonly property string managedError: plasma.error
    readonly property var managedPolicy: plasma.policySnapshot

    function setManagedActive(active: bool): bool {
        return !activityManaged || plasma.setActive(active);
    }

    function acceptPlasmaNotification(data: var): void {
        root.activeTargetOutput = root.getTargetOutput();
        const existing = root.list.find(n => n.plasmaBacked && (n.id === String(data.id) || n.id === String(data.replacesId)));
        const comp = existing || notifComp.createObject(root, { plasmaBacked: true });
        comp.updatePlasma(data);
        // Plasma's unchanged popup model evaluates DND, other inhibitors,
        // per-app policy, critical exceptions, and native sound hints.
        if (!existing)
            root.registerNotification(comp);
        else {
            root.list = [comp, ...root.list.filter(n => n !== comp)];
            saveTimer.restart();
        }
        comp.popup = root.managedActive && root.shouldShowPopup() && plasma.present(data.id);
    }

    function snapshot(): string {
        return JSON.stringify({
            provider: activityManaged ? "plasma-watcher" : "quickshell",
            ready: managedReady, active: activityManaged ? managedActive : true,
            error: managedError, policy: managedPolicy,
            notifications: root.list.map(n => ({
                id: n.id, summary: n.summary, body: n.body, appName: n.appName,
                image: n.image, popup: n.popup, closed: n.closed, closeReason: n.closeReason,
                actions: n.actions.map(a => ({identifier: a.identifier, text: a.text})),
                presentation: n.presentationSnapshot()
            }))
        });
    }

    function getCursorOutputName(): string {
        const monitor = Kwin.monitors.find(m => m.name === Kwin.cursorOutputName()) || Kwin.focusedMonitor;
        return monitor?.name || Kwin.cursorOutputName() || "";
    }

    function getTargetOutput(): string {
        const cursorScreen = root.getCursorOutputName();
        if (GlobalConfig.notifs.monitor === "focused") {
            if (GlobalConfig.notifs.fullscreen === "off" && Kwin.hasFullscreenOn(cursorScreen)) {
                const scrList = Screens.screens || [];
                for (let i = 0; i < scrList.length; i++) {
                    const candidate = scrList[i].name;
                    if (candidate !== cursorScreen && !Kwin.hasFullscreenOn(candidate))
                        return candidate;
                }
                return "";
            }
            return cursorScreen;
        }
        return cursorScreen;
    }

    function hasFullscreen(): bool {
        return Kwin.hasFullscreen();
    }

    // Called only when an actual list of items is needed (serialisation, clear).
    // Not used as a binding anywhere.
    function notClosed(): list<NotifData> { return list.filter(n => !n.closed) }

    /// Files a freshly created notification: counts it, puts it at the top of the
    /// list and evicts whatever the cap pushes out. Both the D-Bus path below and
    /// the shell-authored path go through here so the cap policy lives once.
    function registerNotification(comp: NotifData): void {
        root.openCount++;
        if (comp.popup)
            root.popupCount++;

        const next = [comp, ...root.list];
        const cap = root.notifCap;
        if (next.length > cap) {
            const evicted = next.splice(cap);
            for (const old of evicted) old.close();
        }
        root.list = next;
    }

    /// Raises a notification the shell authored itself: there is no D-Bus
    /// notification behind it, and its actions run shell callbacks rather than
    /// the sender's. Kept out of the on-disk history, since those callbacks
    /// cannot survive a restart (see NotifData.shellRaised).
    function addShellNotification(params: var): NotifData {
        // The same bookkeeping the D-Bus path does: which screen a popup belongs to
        // is settled before it is created, because shouldShowPopup() reads it.
        root.activeTargetOutput = root.getTargetOutput();

        const comp = notifComp.createObject(root, {
            popup: root.shouldShowPopup() && (!root.activityManaged || !root.managedPolicy.inhibited),
            shellRaised: true,
            image: "",
            hints: ({}),
            appName: params.appName ?? qsTr("Caelestia"),
            summary: params.summary ?? "",
            body: params.body ?? "",
            appIcon: params.appIcon ?? "",
            materialIcon: params.materialIcon ?? "",
            actions: params.actions ?? [],
            resident: params.resident ?? true
        });

        root.registerNotification(comp);

        return comp;
    }

    function shouldShowPopup(): bool {
        if (activityManaged && !managedActive)
            return false;
        if (props.dnd || [...Visibilities.screens.values()].some(v => v.sidebar))
            return false;
        if (GlobalConfig.notifs.fullscreen === "off") {
            if (GlobalConfig.notifs.monitor === "focused") {
                const targetName = root.activeTargetOutput || root.getTargetOutput();
                if (targetName === "" || Kwin.hasFullscreenOn(targetName))
                    return false;
            } else {
                const scrList = Screens.screens || [];
                if (scrList.length > 0 && scrList.every(s => Kwin.hasFullscreenOn(s.name)))
                    return false;
                if (scrList.length === 0 && hasFullscreen())
                    return false;
            }
        }
        return true;
    }

    function shouldPlaySound(notif: var): bool {
        if (props.dnd)
            return false;
        if (notif.appName === "caelestia-cli" || GlobalConfig.audio.sounds.disabledNotifApps.includes(notif.appName))
            return false;
        if (GlobalConfig.notifs.fullscreen === "off") {
            if (GlobalConfig.notifs.monitor === "focused") {
                const targetName = root.activeTargetOutput || root.getTargetOutput();
                if (targetName === "" || Kwin.hasFullscreenOn(targetName))
                    return false;
            } else {
                const scrList = Screens.screens || [];
                if (scrList.length > 0 && scrList.every(s => Kwin.hasFullscreenOn(s.name)))
                    return false;
                if (scrList.length === 0 && hasFullscreen())
                    return false;
            }
        }
        return true;
    }

    function clear(): void {
        const toClose = root.list;
        root.list = [];
        root.openCount = 0;
        root.popupCount = 0;
        for (let i = 0; i < toClose.length; i++)
            toClose[i].close();
        saveTimer.stop();
        root.lastSavedState = "[]";
        storage.setText("[]");
    }

    function serializeState(): string {
        return JSON.stringify(root.notClosed().filter(n => !n.shellRaised).map(n => ({
                        time: n.time,
                        id: n.id,
                        summary: n.summary,
                        body: n.body,
                        appIcon: n.appIcon,
                        appName: n.appName,
                        image: n.image,
                        expireTimeout: n.expireTimeout,
                        urgency: n.urgency,
                        resident: n.resident,
                        hasActionIcons: n.hasActionIcons,
                        actions: n.actions
                    })));
    }

    onDndChanged: {
        if (!GlobalConfig.utilities.toasts.dndChanged)
            return;

        if (dnd)
            Toaster.toast(qsTr("Do not disturb enabled"), qsTr("Popup notifications are now disabled"), "do_not_disturb_on");
        else
            Toaster.toast(qsTr("Do not disturb disabled"), qsTr("Popup notifications are now enabled"), "do_not_disturb_off");
    }

    onListChanged: {
        if (loaded)
            saveTimer.restart();
    }

    Timer {
        id: saveTimer

        interval: Math.min(10000, 3000 + root.list.length * 10)
        onTriggered: {
            const serialized = root.serializeState();
            if (serialized === root.lastSavedState)
                return;
            root.lastSavedState = serialized;
            storage.setText(serialized);
        }
    }

    PersistentProperties {
        id: props

        property bool dnd

        reloadableId: "notifs"
    }

    PlasmaNotifications {
        id: plasma
        enabled: root.activityManaged
        onNotificationReceived: data => root.acceptPlasmaNotification(data)
        onNotificationClosed: (id, reason) => {
            const comp = root.list.find(n => n.plasmaBacked && n.id === String(id));
            comp?.closeFromServer(reason);
        }
        onInvalidated: {
            for (const comp of root.list)
                if (comp.plasmaBacked)
                    comp.invalidatePlasma();
        }
        onNotificationPolicyChanged: {
            for (const comp of root.list)
                if (comp.plasmaLive && comp.popup && !plasma.eligible(Number(comp.id)))
                    comp.popup = false;
        }
    }

    // A Loader is essential: disabling callbacks on a constructed
    // NotificationServer would still compete for the authoritative bus name.
    Loader {
        active: !root.activityManaged
        sourceComponent: Component {
            NotificationServer {
                keepOnReload: false
                actionsSupported: true
                bodyHyperlinksSupported: true
                bodyImagesSupported: true
                bodyMarkupSupported: true
                imageSupported: true
                persistenceSupported: true

                onNotification: notif => {
                    notif.tracked = true;
                    root.activeTargetOutput = root.getTargetOutput();
                    const comp = notifComp.createObject(root, {
                        popup: root.shouldShowPopup(), notification: notif
                    });
                    root.registerNotification(comp);
                    if (root.shouldPlaySound(notif))
                        Audio.playNotification();
                }
            }
        }
    }

    function invokePlasmaAction(id: string, action: string): bool {
        return plasma.invokeAction(Number(id), action);
    }

    function dismissPlasma(id: string): bool {
        return plasma.dismiss(Number(id));
    }

    function expirePlasma(id: string): void {
        plasma.expire(Number(id));
    }

    FileView {
        id: storage

        printErrors: false
        path: `${Paths.state}/notifs.json`
        onLoaded: {
            let data;
            try {
                data = JSON.parse(text());
            } catch (err) {
                Logger.log("[Notifs] saved notifications are corrupt, starting fresh:", err);
                data = [];
            }
            if (!Array.isArray(data)) {
                Logger.log("[Notifs] saved notifications are not a list, starting fresh");
                data = [];
            }
            data.sort((a, b) => b.time - a.time);
            const cap = root.notifCap;
            // A live watcher can deliver while FileView is loading. Never
            // replace that feed or revive persisted callbacks for dead senders.
            const liveIds = new Set(root.list.map(n => n.id));
            const restored = data.filter(n => !liveIds.has(String(n.id))).slice(0, cap).map(n => {
                if (root.activityManaged) {
                    n.actions = [];
                    n.plasmaBacked = true;
                }
                return notifComp.createObject(root, n);
            });
            root.list = [...root.list, ...restored].slice(0, cap);

            root.openCount = root.list.filter(n => !n.closed).length;
            root.popupCount = root.list.filter(n => n.popup).length;
            root.lastSavedState = root.serializeState();
            root.loaded = true;
        }
        onLoadFailed: err => {
            if (err === FileViewError.FileNotFound) {
                root.loaded = true;
                root.lastSavedState = "[]";
                Qt.callLater(() => setText("[]"));
            }
        }
    }

    // qmllint disable unresolved-type
    CustomShortcut {
        // qmllint enable unresolved-type
        name: "clearNotifs"
        description: qsTr("Clear all notifications")
        onPressed: root.clear()
    }

    IpcHandler {
        function snapshot(): string {
            return root.snapshot();
        }

        function clear(): void {
            root.clear();
        }

        function isDndEnabled(): bool {
            return props.dnd;
        }

        function toggleDnd(): void {
            props.dnd = !props.dnd;
        }

        function enableDnd(): void {
            props.dnd = true;
        }

        function disableDnd(): void {
            props.dnd = false;
        }

        target: "notifs"
    }

    Component {
        id: notifComp

        NotifData {
            onClosedChanged: {
                root.openCount = closed ? Math.max(0, root.openCount - 1) : root.openCount + 1;
            }
            onPopupChanged: {
                root.popupCount = popup ? root.popupCount + 1 : Math.max(0, root.popupCount - 1);
                if (root.popupCount === 0)
                    root.activeTargetOutput = "";
            }
        }
    }
}
