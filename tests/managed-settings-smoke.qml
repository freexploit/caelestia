pragma ComponentBehavior: Bound

import QtQuick
import Quickshell
import Quickshell.Io
import Caelestia.Config

// Run only with a disposable HOME/XDG_CONFIG_HOME and private runtime/bus. This
// loads the real native settings singletons, but no shell surfaces or services.
// IPC target: managed-settings. Empty screen selects the writable global root.
// inspect/set/reset take (tree: shell|tokens, screen, dotted path[, JSON value]).
// set/reset report in-memory state; the driver must await and inspect the real
// sparse file for persistence, and poll failures() for asynchronous write errors.
ShellRoot {
    id: root

    settings.watchFiles: false

    property var trackedLayers: ({})
    property var writeFailures: []
    property var loadFailures: []

    readonly property var shellPaths: [
        "enabled", "general.mediaGifSpeedAdjustment", "general.logo",
        "general.battery.warnLevels"
    ]
    readonly property var tokenPaths: [
        "appearance.rounding.small", "appearance.animDurations.small"
    ]

    function layerFor(tree, screen) {
        if (tree !== "shell" && tree !== "tokens")
            throw new Error("tree must be shell or tokens");
        const singleton = tree === "shell" ? GlobalConfig : TokenConfig;
        const layer = screen === "" ? singleton : singleton.forScreen(screen);
        const id = tree + ":" + screen;
        if (!trackedLayers[id]) {
            trackedLayers[id] = true;
            layer.treeSaveFailed.connect((_self, error) => {
                root.writeFailures = [...root.writeFailures, { tree: tree, screen: screen, error: error }];
            });
            layer.treeLoadFailed.connect((_self, error) => {
                root.loadFailures = [...root.loadFailures, { tree: tree, screen: screen, error: error }];
            });
        }
        return layer;
    }

    function optionFor(tree, screen, path) {
        const paths = tree === "shell" ? shellPaths : tokenPaths;
        if (paths.indexOf(path) === -1)
            throw new Error("unsupported smoke path: " + path);
        const layer = layerFor(tree, screen);
        const parts = path.split(".");
        let node = layer;
        for (let i = 0; i < parts.length - 1; ++i)
            node = node[parts[i]];
        return { layer: layer, node: node, key: parts[parts.length - 1] };
    }

    function warnLevels(list) {
        const values = [];
        for (let i = 0; i < list.count; ++i) {
            const entry = list.at(i);
            values.push({
                level: entry.level,
                title: entry.title,
                message: entry.message,
                icon: entry.icon,
                critical: entry.critical
            });
        }
        return values;
    }

    function inspectOption(tree, screen, path) {
        const option = optionFor(tree, screen, path);
        const list = path === "general.battery.warnLevels";
        const value = list ? warnLevels(option.node[option.key]) : option.node[option.key];
        const override = list ? option.node[option.key].isOverride("values") : option.node.isOverride(option.key);
        const descriptor = option.node.descriptorFor(option.key);
        return {
            tree: tree,
            screen: screen,
            path: path,
            value: value,
            override: override,
            globalOnly: option.node.isGlobalOnly() || descriptor.globalOnly,
            diagnostics: option.layer.diagnostics.map(d => ({
                type: d.type, option: d.option, message: d.message
            }))
        };
    }

    function inspectLayer(screen) {
        return {
            shell: shellPaths.map(path => inspectOption("shell", screen, path)),
            tokens: tokenPaths.map(path => inspectOption("tokens", screen, path)),
            writeFailures: writeFailures,
            loadFailures: loadFailures
        };
    }

    Component.onCompleted: {
        layerFor("shell", "");
        layerFor("tokens", "");
        console.info("managed-settings-ready");
    }

    IpcHandler {
        target: "managed-settings"

        function snapshot(screen: string): string {
            return JSON.stringify(root.inspectLayer(screen));
        }

        function inspect(tree: string, screen: string, path: string): string {
            return JSON.stringify(root.inspectOption(tree, screen, path));
        }

        function set(tree: string, screen: string, path: string, json: string): string {
            const option = root.optionFor(tree, screen, path);
            const value = JSON.parse(json);
            if (path === "general.battery.warnLevels") {
                if (!Array.isArray(value))
                    throw new Error("warnLevels requires a JSON array");
                const list = option.node[option.key];
                list.clear();
                for (const entry of value)
                    list.insert(entry);
            } else {
                option.node[option.key] = value;
            }
            return JSON.stringify(root.inspectOption(tree, screen, path));
        }

        function reset(tree: string, screen: string, path: string): string {
            const option = root.optionFor(tree, screen, path);
            option.node.resetOption(option.key);
            return JSON.stringify(root.inspectOption(tree, screen, path));
        }

        function save(tree: string, screen: string): void {
            root.layerFor(tree, screen).save();
        }

        function failures(): string {
            return JSON.stringify({ write: root.writeFailures, load: root.loadFailures });
        }

        function clearFailures(): void {
            root.writeFailures = [];
            root.loadFailures = [];
        }
    }
}
