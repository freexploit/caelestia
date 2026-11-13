# Caelestia QML API

This folder exposes the shell's public QML APIs through the `qs.services.api` module.

Use it like this:

```qml
import qs.services.api

Text {
    text: "Plugins: " + CaelestiaApi.plugins.available.count
}
```

The main entry point is the singleton `CaelestiaApi`.

## Access pattern

All public APIs are exposed as properties on the singleton:

```qml
CaelestiaApi.system
CaelestiaApi.windows
CaelestiaApi.network
CaelestiaApi.media
CaelestiaApi.visuals
CaelestiaApi.ui
CaelestiaApi.shortcuts
CaelestiaApi.plugins
```

## API groups

### `CaelestiaApi.system`

Source: `SystemApi.qml`

Provides system-level information and utility objects.

- `session`: `Backend.SessionManager`
- `utils`: `Backend.CUtils`
- `cpu`: `Backend.Cpu`
- `memory`: `Backend.Memory`
- `gpu`: `Backend.Gpu`
- `storage`: `Backend.Storage`
- `sysInfo`: `Utils.SysInfo`
- `time`: `Services.Time`
- `update`: `Services.UpdateChecker`

Use for OS state, system info, time, and update checking.

### `CaelestiaApi.windows`

Source: `WindowsApi.qml`

Provides window and workspace related information.

- `kwin`: `Backend.KWinActiveWindowBridge`
- `workspaces`: `Backend.KWinWorkspaceState`
- `geometry`: `Backend.MinimizeGeometry`
- `edges`: `Backend.ScreenEdges`

Use for active window state, workspace details, geometry helpers, and screen edge behavior.

`kwin.windowList`, `kwin.activeWindow`, and `kwin.windowsForWorkspace(...)` are
views of the current KDE Activity, in managed and non-managed sessions alike.
Empty window Activity membership means all Activities; a window is not exposed
until its initial protocol state is complete. `currentActivity` is the current
UUID and `activityAvailable` is false while the Activity service is unavailable.
The view is empty during that interval and is requeried when the service returns.
All window actions revalidate membership, including cached UUIDs. Use
`kwin.isWindowEligible(address)` to gate previews retained beyond a model update;
it does not create handles for unknown or unmapped windows.

The shell's `kwin windowState` IPC returns the authoritative view and actual
per-output taskbar model membership for isolated Activity regression scenarios.
Its `focusWindow`, `closeWindow`, and `isWindowEligible` IPC methods use the same
native membership checks as the UI.
The `overview-windows-<output-name> windowState` IPC reads instantiated overview
page models directly; a page marked `instantiated: false` is not rendering proof.

The isolated regression scenario creates distinguishable A-only, B-only,
explicitly shared, and empty-membership/all-Activity windows. In B, taskbar and
overview models must omit A-only windows while retaining B/shared/all windows,
including minimized windows on different virtual desktops and both outputs.
Reassign a displayed window to A, then invoke its previously cached focus, close,
highlight, state, desktop, and output actions: none may affect it. Repeat with an
unmapped UUID, rapid A-B-A switching, and Activity service loss/reappearance;
the view must empty during service loss and converge to the queried Activity on
return. Run the same view/action scenario without managed mode.

### `CaelestiaApi.network`

Source: `NetworkApi.qml`

Provides network managers and status helpers.

- `manager`: `Backend.NmQt`
- `requests`: `Backend.Requests`
- `usage`: `Services.NetworkUsage`
- `vpn`: `Services.VPN`

Use for network adapters, traffic information, VPN state, and request helpers.

### `CaelestiaApi.media`

Source: `MediaApi.qml`

Provides media and entertainment integrations.

- `audio`: `Services.Audio`
- `players`: `Services.Players`
- `lyrics`: `Backend.Lyrics`
- `recorder`: `Services.Recorder`
- `discord`: `Backend.DiscordIpc`

Use for playback state, recording, lyrics, and Discord integration.

### `CaelestiaApi.visuals`

Source: `VisualsApi.qml`

Provides visual theming and brightness helpers.

- `wallpaper`: `Services.Wallpapers`
- `colors`: `Services.Colours`
- `nightLight`: `Backend.NightColorBridge`
- `brightness`: `Backend.KdeOutputDevice`

Use for wallpaper access, theme colors, night light, and brightness control.

### `CaelestiaApi.ui`

Source: `UiApi.qml`

Provides UI-level helpers for notifications, clipboard, emoji lookup, and keyboard shortcuts.

- `toast`: `Backend.Toast`
- `clipboard`: `Backend.ClipboardManager`
- `emojis`: `Backend.EmojiDb`
- `shortcuts`: `Backend.GlobalShortcutDispatcher`
- `keyboard`: `Services.KbLayout`
- `topBarLeft`: `ListModel`
- `topBarMiddle`: `ListModel`
- `topBarRight`: `ListModel`
- `launcherWidgets`: `ListModel`

Use for onboarding UI state, clipboard actions, top bar widgets, keyboard layout, and global UI helpers.

### `CaelestiaApi.shortcuts`

Source: `ShortcutsApi.qml`

Provides a small helper API for dynamically registering shortcuts from QML.

```qml
CaelestiaApi.shortcuts.register("My Shortcut", "Does something", "Meta+Shift+K", function() {
    console.log("Triggered")
})
```

This returns a shortcut object and connects its `pressed` signal to the callback when provided.

### `CaelestiaApi.plugins`

Source: `PluginsApi.qml`

Provides plugin system access.

```qml
property ListModel available: ListModel {}
```

This is the list of discovered plugins and is used by the loader, plugin store, and plugin page UI.

Typical usage:

```qml
for (let i = 0; i < CaelestiaApi.plugins.available.count; i++) {
    let p = CaelestiaApi.plugins.available.get(i);
    console.log(p.name, p.enabled, p.source);
}
```

## Singleton file

The actual root singleton is defined in `CaelestiaApi.qml`:

```qml
QtObject {
    readonly property SystemApi system: SystemApi {}
    readonly property WindowsApi windows: WindowsApi {}
    readonly property NetworkApi network: NetworkApi {}
    readonly property MediaApi media: MediaApi {}
    readonly property VisualsApi visuals: VisualsApi {}
    readonly property UiApi ui: UiApi {}
    readonly property ShortcutsApi shortcuts: ShortcutsApi {}
    readonly property PluginsApi plugins: PluginsApi {}
}
```

## Recommended use

- Use `CaelestiaApi.system` for shell/system telemetry.
- Use `CaelestiaApi.windows` for active window and workspace state.
- Use `CaelestiaApi.network` for connectivity and network helpers.
- Use `CaelestiaApi.media` for media playback and audio features.
- Use `CaelestiaApi.visuals` for wallpapers and color/brightness state.
- Use `CaelestiaApi.ui` for UI-related shell tools.
- Use `CaelestiaApi.shortcuts` to add custom keyboard actions.
- Use `CaelestiaApi.plugins` to discover and manage plugins.

## Module declaration

The module is registered in `qmldir`:

```text
module qs.services.api
singleton CaelestiaApi 1.0 CaelestiaApi.qml
SystemApi 1.0 SystemApi.qml
WindowsApi 1.0 WindowsApi.qml
NetworkApi 1.0 NetworkApi.qml
MediaApi 1.0 MediaApi.qml
VisualsApi 1.0 VisualsApi.qml
UiApi 1.0 UiApi.qml
ShortcutsApi 1.0 ShortcutsApi.qml
PluginsApi 1.0 PluginsApi.qml
```

This makes the APIs available anywhere in the shell by importing:

```qml
import qs.services.api
```
