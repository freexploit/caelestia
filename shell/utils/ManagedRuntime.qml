pragma Singleton

import QtQuick
import Quickshell

Singleton {
    readonly property bool enabled: Quickshell.env("CAELESTIA_MANAGED") === "1"
    readonly property string lifecycleInstructions: qsTr("Nix/Home Manager owns this installation. Update the Caelestia input in your flake and rebuild your Home Manager or NixOS configuration. To remove it, disable programs.caelestia-kde.enable and rebuild; writable user settings are preserved. Configure login and lock screens declaratively in your system configuration.")
}
