# Shortcut system architecture

This document provides a simplified overview of how the Caelestia shortcut system works, from the backend C++ components to the QML frontend.

---

## 1. High-Level Overview

```text
[ QML UI (Shortcuts.qml, Nexus) ]
              │
              ▼
[ GlobalShortcut (C++ / QML) ]
              │ registers keys & handles conflicts
              ▼
[ GlobalShortcutDispatcher ]  <──  [ KeybindsModel ]
  (Tracks stolen keys)             (Loads user settings)
```

- **`GlobalShortcut`**: Registers a single shortcut.
- **`GlobalShortcutDispatcher`**: A central hub that tracks shortcut collisions.
- **`KeybindsModel`**: Manages user-configured overrides and default bindings.

---

## 2. Core Components

### `GlobalShortcut`

This handles binding a hotkey (like `Meta+P`) to an action. It uses KDE's `kglobalaccel` under the hood.

**Key Features:**
- **Key Conflict Resolution:** If you try to use a shortcut that another KDE app is already using (like Spectacle using `Print`), Caelestia will temporarily "steal" it.
- **Smart Restoration:** If you change or remove a Caelestia shortcut, any stolen shortcuts are automatically given back to their original KDE apps.

### `GlobalShortcutDispatcher`

A singleton that acts as the central coordinator.
- **Collision Index:** It keeps track of which keys are currently "stolen" from other apps.
- This allows the UI to easily show a warning (like a red dot) if a shortcut conflicts with another app.

### `KeybindsModel`

Manages saving and loading your shortcut preferences.
1. Loads defaults from `keybindsdefaults.hpp`.
2. Applies user customizations from `${XDG_CONFIG_HOME:-~/.config}/caelestia/keybinds.json`.
3. Communicates with the UI to update the list of active shortcuts.

---

## 3. Crash Recovery (Safety First)

If the shell crashes or is force-killed, we don't want your KDE shortcuts to be permanently broken because we stole them and never gave them back.

**How it works:**
1. Whenever Caelestia steals a shortcut, it immediately writes this to a recovery file: `${XDG_CONFIG_HOME:-~/.config}/caelestia/stolen-shortcuts.json`.
2. When the shell closes normally, it restores the shortcuts and deletes the file.
3. If the shell crashes, the file remains on disk. On the **next startup**, Caelestia reads this file, restores all the stolen shortcuts, and cleans up.

### Managed Activity sessions

With `CAELESTIA_MANAGED=1`, the legacy detached `gdbus` restoration path is not
used. The native dispatcher records the exact original and integration-owned
bindings in `${XDG_STATE_HOME:-~/.local/state}/caelestia/shortcuts.json` **before**
mutation. `CAELESTIA_SHORTCUT_JOURNAL` can select another private journal. Atomic
replacement, owner-only permissions, file/directory sync, and a lifetime lock
keep recovery durable and prevent a second shell/helper from racing it.

Acquisition removes only the conflicting chord, preserving an action's other
bindings and its explicit KDE shortcut context. Both foreign writes and shell
registration require synchronous daemon readback. Errors fail managed readiness;
they never become successful registration merely because a subprocess exited.
Changing a shell binding first releases its previous claim. Shared baseline
actions track each displaced chord's owner so changing one shell binding does not
prematurely restore a chord another shell action still needs.

Clean shutdown disconnects shell callbacks and restores the owned values with
readback. The independent command below is also required after a shell process
has stopped, including `SIGKILL`; it does not start Caelestia or need its UI:

```sh
caelestia-shortcut-recovery recover
caelestia-shortcut-recovery status
caelestia-shortcut-recovery recover --journal /private/path/shortcuts.json
```

Exit `0` means all selected recovery claims were restored/verified or retired
because a later editor now owns a different value; `1` means a lock, journal,
D-Bus, or readback failure, with unfinished records retained for retry; `2` means
invalid arguments. Commands emit JSON (`ok`, `pending`, `error`; status includes
`records`). Recovery only writes an entry if its current value exactly matches
the value Caelestia recorded as its own. It does not overwrite a later edit,
clear entire components, remove journals before acknowledgment, or restart Plasma.
The legacy `stolen-shortcuts.json` has no ownership/readback information and is
not imported into the managed journal or blindly restored by managed sessions.

### Coordinator and Meta boundary

The managed dispatcher owns D-Bus service/interface `org.caelestia.Shortcuts` at
`/org/caelestia/Shortcuts`. `status()` and `release()` return `a{sv}` with `ok`
(boolean), `pending` (integer), `registered` (integer), `metaAction` (empty string),
`metaBackend` (`kglobalaccel`), and `error` (string). Registration/release is
synchronous, so a status call after shortcut construction observes settled work
(`pending = 0`). `ok = false` is a failed entry, not a readiness fallback. The
coordinator stops the shell and invokes independent recovery before declaring
baseline restoration; release failure cannot be ignored as a successful exit.

The target Plasma/KWin 6.6.6 supports modifier-only shortcuts in KGlobalAccel.
Managed parsing translates bare `Meta` to `QKeySequence(Qt::MetaModifier)` rather
than the ordinary Meta key symbol, and acquires/restores it through the same
journal as chords. KGlobalAccel dispatches the action on modifier release and
cancels the modifier-only sequence when combined with other input. This is not
the older `kwinrc [ModifierOnlyShortcuts]` D-Bus-route mechanism: the target KWin
has removed that implementation. The coordinator must not mutate `kwinrc` for
this backend. `metaAction` is empty because no separate route is required.

Backend source references (not a substitute for guest behavioral acceptance):
[modifier-release dispatch](https://github.com/KDE/kglobalacceld/blob/v6.6.6/src/globalshortcutsregistry.cpp#L437-L463),
[modifier-only input cases](https://github.com/KDE/kglobalacceld/blob/v6.6.6/autotests/shortcutstest.cpp),
and [KWin accelerator delegation](https://github.com/KDE/kwin/blob/v6.6.6/src/globalshortcuts.cpp#L188-L233).

Guest acceptance must inject real chord **and** press/release Meta events, count
exactly one intended action, and compare daemon keys across clean exit, forced
kill, and independent recovery. Also edit a displaced baseline binding during
ownership and verify cleanup preserves that newer value. These checks require
the real guest KWin/KGlobalAccel services; direct handler invocation or asserting
source strings is not input-routing proof.

---

## 4. User Interface (QML)

### Defining Shortcuts (`Shortcuts.qml`)
Shortcuts are defined using a `CustomShortcut` component. It automatically detects if you are running KDE (KWin) or Hyprland and uses the correct backend.

### Nexus Shortcut Manager
The Nexus settings panel is where users manage their shortcuts:
- **Groups:** Organizes shortcuts into categories (Shell, Apps, etc.).
- **Collision Blinker:** Shows a red warning dot if a key sequence conflicts with an existing KDE app. Hovering shows which app was overridden.
- **Editing:** Users can capture new keystrokes or reset back to default settings.

---

## 5. Important Files

| File | Purpose |
|---|---|
| `globalshortcut.hpp / .cpp` | Core logic for binding, stealing, and restoring shortcuts. |
| `keybindsmodel.hpp / .cpp` | Manages user settings and feeds data to the UI. |
| `keybindsdefaults.hpp` | The default key combinations. |
| `Shortcuts.qml` | Where all shell shortcuts are declared. |
| `keybinds.json` | Where user overrides are saved. |
| `stolen-shortcuts.json` | Crash recovery file. |
