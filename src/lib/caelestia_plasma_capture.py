# SPDX-License-Identifier: GPL-3.0-or-later
"""Private Plasma recovery and review inputs; never an automatic restore tool."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
from urllib.parse import unquote, urlsplit

# Deliberately excludes wallets, browser/application profiles, histories and the
# activity usage database. rc2nix sees ONLY these recovery copies, not the home.
CONFIG_FILES = (
    "kdeglobals", "kwinrc", "kwinrulesrc", "kglobalshortcutsrc",
    "kactivitymanagerdrc", "kactivitymanagerd-statsrc", "plasmarc",
    "plasmashellrc", "plasma-org.kde.plasma.desktop-appletsrc",
    "ksplashrc", "kscreenlockerrc", "kcminputrc", "kxkbrc", "auroraerc",
    "plasmanotifyrc", "plasma-localerc", "kwinoutputconfig.json",
)
REQUIRED_FILES = {"kdeglobals", "kwinrc", "plasma-org.kde.plasma.desktop-appletsrc"}
MAX_FILE_BYTES = 64 * 1024 * 1024
PRIVATE_KEY = re.compile(r"password|passwd|secret|token|credential|authorization|api.?key|private.?key|cookie", re.I)
PRIVATE_VALUE = re.compile(r"(?:https?|ftp|smb|ssh)://|\b[A-Z0-9._%+-]+@[A-Z0-9.-]+\.[A-Z]{2,}|-----BEGIN .*PRIVATE KEY", re.I)
ABSOLUTE_PATH = re.compile(r"(?<![A-Za-z0-9_:/])(?:file://)?/(?!/)(?:[^\s\"'<>;,\[\]{}()\\]|\\ )+")


def digest(data):
    return hashlib.sha256(data).hexdigest()


def timestamp():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def nix_string(value):
    # JSON strings and Nix quoted strings share these escapes; Nix additionally
    # interpolates ${...}, even in values originating in a widget configuration.
    return json.dumps(value, ensure_ascii=False).replace("${", "\\${")


def parse_rc(data):
    """Keep KConfig's encoded values, nesting and flags for lossless review."""
    groups = {}
    group = ()
    problems = []
    for number, raw in enumerate(data.decode("utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith(("#", ";")):
            continue
        if line.startswith("[") and re.fullmatch(r"(?:\[[^\]]*\])+", line):
            group = tuple(re.findall(r"\[([^\]]*)\]", line))
            groups.setdefault(group, {})
        elif "=" in line:
            key, value = line.split("=", 1)
            groups.setdefault(group, {})[key.strip()] = value
        else:
            problems.append({"line": number, "reason": "unsupported KConfig line (retained in recovery)"})
    return groups, problems


def group_name(group):
    # plasma-manager uses / for nesting and \/ for a literal slash.
    return "/".join(part.replace("/", "\\/") for part in group)


def module_from_rc(configs, description):
    lines = ["# PRIVATE, UNREVIEWED capture candidate. Do not add directly to a flake/store.",
             "# " + description, "{ lib, ... }: {", "  programs.plasma = {",
             "    overrideConfig = lib.mkDefault false;", "    configFile = {"]
    for filename, groups in sorted(configs.items()):
        for group, entries in sorted(groups.items()):
            if not group or any(part.startswith("$") for part in group):
                continue
            for key, value in sorted(entries.items()):
                # Keep raw KDE escapes; let plasma-manager preserve expansion
                # and immutability flags instead of encoding them into the key.
                marking = re.search(r"\[\$([a-z]+)\]$", key)
                base_key = key[:marking.start()] if marking else key
                flags = marking.group(1) if marking else ""
                if set(flags) - {"e", "i"}:
                    continue
                encoded = ("{ value = " + nix_string(value) + "; escapeValue = false; "
                           + "shellExpand = " + str("e" in flags).lower() + "; "
                           + "immutable = " + str("i" in flags).lower() + "; }")
                lines.append("      " + ".".join(nix_string(p) for p in (filename, group_name(group), base_key)) + " = " + encoded + ";")
    lines.extend(["    };", "  };", "}", ""])
    return "\n".join(lines)


def command_path(environment, *names):
    selected = os.environ.get(environment)
    if selected:
        return shutil.which(selected)
    return next((found for name in names if (found := shutil.which(name))), None)


class Capture:
    def __init__(self, destination, offline):
        self.destination = destination
        self.offline = offline
        self.home = Path.home().resolve()
        self.config_home = Path(os.environ.get("XDG_CONFIG_HOME", self.home / ".config")).resolve()
        self.data_home = Path(os.environ.get("XDG_DATA_HOME", self.home / ".local/share")).resolve()
        data_dirs = os.environ.get("XDG_DATA_DIRS", "/usr/local/share:/usr/share")
        self.data_dirs = [self.data_home] + [Path(p) for p in data_dirs.split(":") if p and Path(p).is_absolute()]
        self.busctl = command_path("CAELESTIA_BUSCTL", "busctl")
        self.sources = []
        self.rc = {}
        self.diagnostics = []
        self.privacy = []
        self.assets = []
        self.start = timestamp()
        self.bus_queries = []
        self.created = False

    def diagnostic(self, code, message, severity="warning", **details):
        self.diagnostics.append({"code": code, "severity": severity, "message": message, **details})

    def put(self, relative, data):
        path = self.destination / relative
        path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        # All intermediate directories are created in this private tree, never
        # copied from source permissions. No existing destination is accepted.
        for directory in path.parents:
            if directory == self.destination.parent:
                break
            directory.chmod(0o700)
        content = data.encode("utf-8") if isinstance(data, str) else data
        with path.open("xb") as target:
            target.write(content)
        path.chmod(0o600)

    def put_json(self, relative, data):
        self.put(relative, json.dumps(data, ensure_ascii=False, indent=2, sort_keys=True) + "\n")

    @staticmethod
    def fingerprint(path):
        try:
            link = os.readlink(path) if path.is_symlink() else None
            descriptor = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
            with os.fdopen(descriptor, "rb") as source:
                before = os.fstat(source.fileno())
                if not stat.S_ISREG(before.st_mode):
                    return {"status": "unsupported-file-type"}, None
                if before.st_size > MAX_FILE_BYTES:
                    return {"status": "too-large", "bytes": before.st_size}, None
                data = source.read(MAX_FILE_BYTES + 1)
                after = os.fstat(source.fileno())
            stable = (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns) == (
                after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns)
            if len(data) > MAX_FILE_BYTES:
                return {"status": "too-large"}, None
            return {"status": "ok", "sha256": digest(data), "bytes": len(data),
                    "mtime_ns": after.st_mtime_ns, "ctime_ns": after.st_ctime_ns,
                    "inode": after.st_ino, "device": after.st_dev, "mode": stat.S_IMODE(after.st_mode),
                    "symlink_target": link, "stable_during_read": stable}, data
        except FileNotFoundError:
            return {"status": "missing"}, None
        except OSError as error:
            return {"status": "unreadable", "error": str(error)}, None

    def source_list(self):
        result = [("config", name, self.config_home / name) for name in CONFIG_FILES]
        # KScreen uses hashes rather than arbitrary names for saved output data.
        # Never recurse over all XDG data or follow directories/symlink trees.
        directory = self.data_home / "kscreen"
        try:
            if directory.is_symlink():
                self.diagnostic("kscreen-symlink", "Skipping symlinked KScreen directory; review target explicitly")
            elif directory.is_dir():
                for path in sorted(directory.iterdir()):
                    if path.is_file() and re.fullmatch(r"[0-9a-fA-F]{16,64}(?:\.json)?", path.name):
                        result.append(("data", "kscreen/" + path.name, path))
                outputs = directory / "outputs"
                if outputs.is_dir() and not outputs.is_symlink():
                    for path in sorted(outputs.iterdir()):
                        if path.is_file() and re.fullmatch(r"[0-9a-fA-F]{16,64}(?:\.json)?", path.name):
                            result.append(("data", "kscreen/outputs/" + path.name, path))
        except OSError as error:
            self.diagnostic("kscreen-unreadable", str(error))
        return result

    def copy_sources(self):
        for kind, name, path in self.source_list():
            fingerprint, data = self.fingerprint(path)
            entry = {"kind": kind, "relative_path": name, "source": str(path), "before": fingerprint}
            self.sources.append(entry)
            if data is None:
                if name in REQUIRED_FILES or fingerprint["status"] != "missing":
                    self.diagnostic("source-unavailable", "Allowlisted source could not be captured", source=str(path), status=fingerprint["status"])
                continue
            recovery = "recovery/" + kind + "/" + name
            self.put(recovery, data)
            entry["recovery"] = recovery
            if not fingerprint["stable_during_read"]:
                self.diagnostic("source-changing", "Source changed while it was read", source=str(path), severity="error")
            if kind == "config" and not name.endswith(".json"):
                try:
                    groups, problems = parse_rc(data)
                    self.rc[name] = groups
                    for problem in problems:
                        self.diagnostic("kconfig-unparsed", "KConfig requires manual review", source=name, **problem)
                    for group, values in groups.items():
                        if not group or any(part.startswith("$") for part in group):
                            self.diagnostic("kconfig-group-review", "Group cannot be represented automatically in the plasma-manager supplement; use raw recovery for review", source=name, group=list(group))
                        for key in values:
                            marking = re.search(r"\[\$([a-z]+)\]$", key)
                            if marking and set(marking.group(1)) - {"e", "i"}:
                                self.diagnostic("kconfig-flag-review", "KConfig deletion/unknown flags require manual conversion", source=name, group=list(group), key=key)
                    for group, values in groups.items():
                        for key, value in values.items():
                            self.scan_value(value, {"file": name, "group": list(group), "key": key})
                except UnicodeError:
                    self.diagnostic("kconfig-encoding", "Non-UTF-8 KConfig retained only as raw recovery", source=name)
            elif name.endswith(".json") or kind == "data":
                try:
                    self.scan_tree(json.loads(data), {"file": name})
                except (ValueError, UnicodeError):
                    self.diagnostic("json-unparsed", "Output configuration is not valid UTF-8 JSON", source=name)

    def scan_value(self, value, location):
        text = str(value)
        reasons = []
        if PRIVATE_KEY.search(str(location.get("key", ""))):
            reasons.append("possible-secret-key")
        if PRIVATE_VALUE.search(text):
            reasons.append("private-endpoint-or-identity")
        if "$HOME" in text or "~/" in text or "/home/" in text or "/run/user/" in text:
            reasons.append("user-specific-path")
        paths = ABSOLUTE_PATH.findall(text)
        if paths:
            reasons.append("absolute-path")
        for path in paths:
            local = Path(unquote(path.removeprefix("file://")))
            if str(local).startswith("/nix/store/"):
                reasons.append("store-path" if local.exists() else "stale-store-path")
            elif not local.exists():
                reasons.append("missing-path")
        if reasons:
            # Values themselves remain solely in the private candidate/recovery.
            # Locations + hashes let reviewers locate secrets without duplicating.
            self.privacy.append({"location": location, "reasons": sorted(set(reasons)), "value_sha256": digest(text.encode())})

    def scan_tree(self, value, location):
        if isinstance(value, dict):
            for key, child in value.items():
                self.scan_tree(child, {**location, "key": str(key), "path": [*location.get("path", []), str(key)]})
        elif isinstance(value, list):
            for index, child in enumerate(value):
                self.scan_tree(child, {**location, "index": index, "path": [*location.get("path", []), index]})
        else:
            self.scan_value(value, location)

    def run_command(self, arguments, environment=None):
        try:
            result = subprocess.run(arguments, capture_output=True, timeout=30, env=environment, check=False)
            return {"status": "ok" if result.returncode == 0 else "failed", "returncode": result.returncode,
                    "stdout": result.stdout.decode("utf-8", errors="replace"), "stderr": result.stderr.decode("utf-8", errors="replace")}
        except (OSError, subprocess.TimeoutExpired) as error:
            return {"status": "failed", "error": str(error)}

    def bus_call(self, label, service, path, interface, method, signature=None, arguments=()):
        query = {"label": label, "service": service, "path": path, "interface": interface, "method": method}
        if self.offline or not self.busctl or not os.environ.get("DBUS_SESSION_BUS_ADDRESS"):
            query["status"] = "unavailable"
            query["reason"] = "offline" if self.offline else "busctl or explicit session bus address unavailable"
        else:
            command = [self.busctl, "--user", "--auto-start=no", "--timeout=10", "--json=short", "call", service, path, interface, method]
            if signature:
                command += [signature, *arguments]
            query.update(self.run_command(command))
            if query["status"] == "ok":
                try:
                    reply = json.loads(query["stdout"])
                    query["reply"] = reply
                    query["data"] = reply["data"]
                except (ValueError, KeyError, TypeError):
                    query["status"] = "invalid-reply"
        self.bus_queries.append(query)
        return query

    @staticmethod
    def single(query):
        data = query.get("data", [])
        return data[0] if query.get("status") == "ok" and len(data) == 1 else None

    def session_state(self, phase):
        activities = "org.kde.ActivityManager"
        activity_interface = "org.kde.ActivityManager.Activities"
        current = self.bus_call(phase + "-current-activity", activities, "/ActivityManager/Activities", activity_interface, "CurrentActivity")
        ids = self.bus_call(phase + "-activities", activities, "/ActivityManager/Activities", activity_interface, "ListActivities")
        desktops = self.bus_call(phase + "-virtual-desktops", "org.kde.KWin", "/VirtualDesktopManager", "org.freedesktop.DBus.Properties", "GetAll", "s", ("org.kde.KWin.VirtualDesktopManager",))
        state = {"current_activity": self.single(current), "activity_ids": self.single(ids), "virtual_desktops": self.single(desktops),
                 "query_status": {"current_activity": current["status"], "activity_ids": ids["status"], "virtual_desktops": desktops["status"]}}
        if phase == "before" and isinstance(state["activity_ids"], list):
            state["activities"] = []
            for activity in state["activity_ids"]:
                info = {"id": activity}
                for field, method in (("name", "ActivityName"), ("icon", "ActivityIcon"), ("description", "ActivityDescription")):
                    reply = self.bus_call("activity-" + field, activities, "/ActivityManager/Activities", activity_interface, method, "s", (activity,))
                    info[field] = self.single(reply)
                    if reply["status"] != "ok":
                        self.diagnostic("activity-metadata-unavailable", "Activity metadata query failed", activity=activity, field=field)
                state["activities"].append(info)
        return state

    def native_layout(self, phase):
        query = self.bus_call(phase + "-native-layout", "org.kde.plasmashell", "/PlasmaShell", "org.kde.PlasmaShell", "dumpCurrentLayoutJS")
        data = self.single(query)
        # Qt exposes QByteArray here; busctl encodes ay as a JSON byte array.
        if isinstance(data, list) and all(isinstance(value, int) and 0 <= value < 256 for value in data):
            try:
                data = bytes(data).decode("utf-8")
            except UnicodeError:
                data = None
        if not isinstance(data, str) or not data.strip():
            self.diagnostic("native-layout-unavailable", "Native dumpCurrentLayoutJS export unavailable; no synthetic replacement is presented as a native dump", phase=phase)
            return None, None
        try:
            match = re.search(r"\bvar\s+layout\s*=\s*", data)
            if not match:
                raise ValueError("native layout variable not found")
            layout, _ = json.JSONDecoder().raw_decode(data[match.end():])
            if not isinstance(layout, dict) or not isinstance(layout.get("panels"), list) or not isinstance(layout.get("desktops"), list):
                raise ValueError("unsupported native layout shape")
        except (ValueError, TypeError) as error:
            self.diagnostic("native-layout-unparsed", "Native JS retained but cannot be interpreted safely", detail=str(error))
            layout = None
        return data, layout

    def output_state(self, phase):
        executable = command_path("CAELESTIA_KSCREEN_DOCTOR", "kscreen-doctor")
        if self.offline or not executable:
            result = {"status": "unavailable", "reason": "offline" if self.offline else "kscreen-doctor unavailable"}
        else:
            result = self.run_command([executable, "--json"])
            if result["status"] == "ok":
                try:
                    result["outputs"] = json.loads(result["stdout"])
                except ValueError:
                    result["status"] = "invalid-reply"
        self.put_json("recovery/session/outputs-" + phase + ".json", result)
        return result

    def convert_rc2nix(self):
        executable = command_path("CAELESTIA_RC2NIX", "rc2nix", "plasma-manager-rc2nix")
        if not executable:
            self.diagnostic("rc2nix-unavailable", "Set CAELESTIA_RC2NIX to the packaged converter executable or install rc2nix")
            return {"status": "unavailable"}
        # Isolate converter discovery and runtime writes to the destination. Its
        # broader default allowlist must never expose extra host files.
        work = self.destination / "recovery/converter-home"
        for name in ("config", "data", "cache", "state"):
            (work / name).mkdir(mode=0o700, parents=True, exist_ok=True)
        environment = dict(os.environ)
        environment.update({"HOME": str(work), "XDG_CONFIG_HOME": str(self.destination / "recovery/config"),
                            "XDG_DATA_HOME": str(self.destination / "recovery/data"), "XDG_CACHE_HOME": str(work / "cache"),
                            "XDG_STATE_HOME": str(work / "state"), "PYTHONDONTWRITEBYTECODE": "1"})
        for name in ("DBUS_SESSION_BUS_ADDRESS", "DISPLAY", "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR"):
            environment.pop(name, None)
        result = self.run_command([executable], environment)
        result["executable"] = executable
        if result["status"] == "ok" and result.get("stdout", "").strip():
            self.put("review/rc2nix.nix", "# PRIVATE, UNREVIEWED converter output. Review before Nix/store/commit use.\n" + result["stdout"])
            self.put("review/settings.nix", "# PRIVATE, UNREVIEWED candidate for programs.caelestia-kde.baseline.settings.\n"
                     "# Sanitize into a separate user-owned reviewed module before importing.\n"
                     "{ lib }: lib.recursiveUpdate\n"
                     "  (import ./rc2nix.nix).programs.plasma\n"
                     "  ((import ./appearance.nix { inherit lib; }).programs.plasma)\n")
        else:
            result["status"] = "failed"
            self.diagnostic("rc2nix-failed", "Converter failed; raw recovery and explicit supplements are still available")
        self.put_json("recovery/rc2nix-result.json", result)
        return {"status": result["status"], "executable": executable}

    def setting(self, filename, group, key):
        values = self.rc.get(filename, {}).get(tuple(group), {})
        return next((value for candidate, value in values.items()
                     if re.sub(r"\[\$[a-z]+\]$", "", candidate) == key), None)

    def asset_inventory(self, layout):
        requests = set()
        categories = {
            "plasmoid": "plasma/plasmoids", "wallpaper-plugin": "plasma/wallpapers",
            "plasma-theme": "plasma/desktoptheme", "look-and-feel": "plasma/look-and-feel",
            "color-scheme": "color-schemes", "icons": "icons", "aurorae": "aurorae/themes",
        }
        for group, values in self.rc.get("plasma-org.kde.plasma.desktop-appletsrc", {}).items():
            plugin = values.get("plugin")
            if plugin and ("Applets" in group or (group and group[0] == "Containments")):
                requests.add(("plasmoid", plugin))
            wallpaper = values.get("wallpaperplugin")
            if wallpaper:
                requests.add(("wallpaper-plugin", wallpaper))
        if layout:
            for containment in layout.get("panels", []) + layout.get("desktops", []):
                for applet in containment.get("applets", []):
                    if isinstance(applet.get("plugin"), str):
                        requests.add(("plasmoid", applet["plugin"]))
                if containment.get("wallpaperPlugin"):
                    requests.add(("wallpaper-plugin", containment["wallpaperPlugin"]))
        for category, filename, group, key in (
            ("plasma-theme", "plasmarc", ("Theme",), "name"),
            ("look-and-feel", "kdeglobals", ("KDE",), "LookAndFeelPackage"),
            ("color-scheme", "kdeglobals", ("General",), "ColorScheme"),
            ("icons", "kdeglobals", ("Icons",), "Theme"),
            ("aurorae", "kwinrc", ("org.kde.kdecoration2",), "theme"),
        ):
            value = self.setting(filename, group, key)
            if value and (category != "aurorae" or value.startswith("__aurorae__svg__") or self.setting("kwinrc", ("org.kde.kdecoration2",), "library") == "org.kde.kwin.aurorae"):
                requests.add((category, value.removeprefix("__aurorae__svg__") if category == "aurorae" else value))
        for category, identity in sorted(requests):
            if "/" in identity or identity in (".", ".."):
                self.diagnostic("asset-invalid-id", "Asset identifier requires manual path review", category=category, identity=identity)
                continue
            candidates = [root / categories[category] / (identity + ".colors" if category == "color-scheme" else identity) for root in self.data_dirs]
            found = next((path for path in candidates if path.exists()), None)
            entry = {"kind": category, "id": identity, "candidates": [str(path) for path in candidates],
                     "status": "found" if found else "missing", "source_revision": None,
                     "redistribution": "unreviewed; supply pinned redistributable source or explicit machine-local asset"}
            if found:
                entry["path"] = str(found)
                entry["machine_local"] = found.is_relative_to(self.home)
                entry["resolved_path"] = str(found.resolve())
                if found.is_file():
                    entry["fingerprint"], _ = self.fingerprint(found)
                else:
                    metadata = []
                    for name in ("metadata.json", "metadata.desktop", "index.theme"):
                        path = found / name
                        fingerprint, data = self.fingerprint(path)
                        if data is not None:
                            metadata.append({"name": name, **fingerprint})
                            if name == "metadata.json":
                                try:
                                    meta = json.loads(data)
                                    entry["metadata"] = {key: meta[key] for key in ("KPlugin", "X-Plasma-API", "X-Plasma-MainScript", "X-KDE-ParentApp") if key in meta}
                                except (ValueError, UnicodeError):
                                    self.diagnostic("asset-metadata-invalid", "Asset metadata is not readable JSON", asset=identity)
                    entry["metadata_files"] = metadata
                    entry["content_inventory"] = self.inventory_tree(found)
            else:
                self.diagnostic("asset-missing", "Referenced widget/theme asset not found in XDG data roots", category=category, identity=identity)
            self.assets.append(entry)
        # Paths referenced by widgets/wallpapers stay explicit. Contents of local
        # wallpapers, service endpoints and other external files are not copied.
        paths = set()
        asset_files = {"plasma-org.kde.plasma.desktop-appletsrc", "kdeglobals", "plasmarc", "kwinrc", "auroraerc", "kscreenlockerrc"}
        for filename, groups in self.rc.items():
            if filename not in asset_files:
                continue
            for values in groups.values():
                for value in values.values():
                    paths.update(ABSOLUTE_PATH.findall(value))
        if layout:
            def collect_paths(value):
                if isinstance(value, dict):
                    for child in value.values():
                        collect_paths(child)
                elif isinstance(value, list):
                    for child in value:
                        collect_paths(child)
                elif isinstance(value, str):
                    paths.update(ABSOLUTE_PATH.findall(value))

            collect_paths(layout)
        for value in sorted(paths):
            path = Path(unquote(value.removeprefix("file://")))
            self.assets.append({"kind": "external-path", "path": str(path), "status": "found" if path.exists() else "missing",
                                "in_store": str(path).startswith("/nix/store/"), "copied": False,
                                "review": "Keep machine-local or replace with reviewed redistributable input; never blindly copy to store"})
            if not path.exists():
                self.diagnostic("external-path-missing", "Referenced absolute path is missing or stale", path=str(path))
        library = self.setting("kwinrc", ("org.kde.kdecoration2",), "library")
        if library:
            self.assets.append({"kind": "decoration-library", "id": library, "status": "unresolved",
                                "review": "Resolve the decoration plugin against the guest's matching KWin package; data-directory lookup cannot prove availability"})
            self.diagnostic("decoration-abi-unverified", "Decoration library availability and ABI require isolated restore verification", library=library, severity="info")

    def inventory_tree(self, root):
        records = []
        try:
            def walk_error(error):
                self.diagnostic("asset-inventory-failed", str(error), path=str(root))

            for directory, dirs, files in os.walk(root, followlinks=False, onerror=walk_error):
                dirs.sort()
                files.sort()
                for name in list(dirs):
                    path = Path(directory) / name
                    if path.is_symlink():
                        records.append({"path": str(path.relative_to(root)), "symlink": os.readlink(path)})
                        dirs.remove(name)
                for name in files:
                    path = Path(directory) / name
                    if len(records) >= 10000:
                        self.diagnostic("asset-inventory-truncated", "Asset contains more than 10000 files; inventory is incomplete", path=str(root))
                        return records
                    if path.is_symlink():
                        records.append({"path": str(path.relative_to(root)), "symlink": os.readlink(path)})
                    else:
                        fingerprint, _ = self.fingerprint(path)
                        records.append({"path": str(path.relative_to(root)), **fingerprint})
                        if fingerprint["status"] != "ok" or not fingerprint.get("stable_during_read", False):
                            self.diagnostic("asset-unreadable", "Asset inventory is incomplete or changed during read", path=str(path))
        except OSError as error:
            self.diagnostic("asset-inventory-failed", str(error), path=str(root))
        return records

    def review_inputs(self, native, layout, before, outputs):
        appearance = {}
        for filename, groups in self.rc.items():
            selected = {}
            for group, values in groups.items():
                include = filename in ("kdeglobals", "plasmarc", "ksplashrc", "auroraerc")
                include |= filename == "kwinrc" and bool(group) and (group[0] == "org.kde.kdecoration2" or group[0].startswith("Style"))
                include |= filename == "kscreenlockerrc" and bool(group) and group[0] in ("Greeter", "Greeter/Wallpaper")
                if include:
                    selected[group] = values
            if selected:
                appearance[filename] = selected
        self.put("review/appearance.nix", module_from_rc(appearance, "Appearance/decoration supplement: retains KDE color/theme keys filtered by rc2nix."))
        applets = self.rc.get("plasma-org.kde.plasma.desktop-appletsrc", {})
        self.put_json("review/applets.json", {group_name(group): values for group, values in applets.items()})
        self.put("review/applets-config.nix", module_from_rc({"plasma-org.kde.plasma.desktop-appletsrc": applets}, "EXPLICIT RESTORE ONLY: raw containment IDs/settings, not safe for an existing session. Do not combine with panel reconstruction."))
        self.put_json("review/identities.json", {"session": before, "activity_config": self.rc_json("kactivitymanagerdrc"),
                                                "virtual_desktop_config": self.rc_json("kwinrc", lambda group: group and group[0] == "Desktops")})
        mapping = {"runtime": outputs.get("outputs"), "screen_connectors": self.rc_json("plasmashellrc", lambda group: group and group[0] == "ScreenConnectors"),
                   "containments": [{"id": group[1], **values} for group, values in applets.items() if len(group) == 2 and group[0] == "Containments"],
                   "review_required": "Map containment lastScreen/screen and native panel order to guest output connectors. Native dump may omit screen assignment; never assume panel index equals output index."}
        self.put_json("review/output-mapping.json", mapping)
        if layout is not None:
            self.put_json("review/native-layout.json", layout)
            self.scan_tree(layout, {"file": "native-layout"})
        if native:
            self.put("review/layout.nix", "# PRIVATE, UNREVIEWED. Explicit isolated restore only; can append duplicate panels.\n"
                     "# Review output-mapping.json and all widget paths first. Not imported by default.\n"
                     "{ lib, restoreLayout ? false, ... }: {\n"
                     "  programs.plasma.startup.desktopScript.caelestia-baseline-layout = lib.mkIf restoreLayout {\n"
                     "    runAlways = false;\n    text = " + nix_string(native) + ";\n  };\n}\n")
        self.diagnostic("output-mapping-review", "Native layout output mapping is not sufficient by itself; compare saved containment lastScreen and connector/output configuration before restore", severity="info")
        self.diagnostic("native-layout-activity-scope", "Native dump includes panels and only current Activity desktops; all saved containment settings are separately retained in applets.json/recovery", severity="info")

    def rc_json(self, filename, predicate=lambda group: True):
        return {group_name(group): values for group, values in self.rc.get(filename, {}).items() if predicate(group)}

    def verify_sources(self):
        unchanged = True
        initial_names = {(entry["kind"], entry["relative_path"]) for entry in self.sources}
        final_names = {(kind, name) for kind, name, _ in self.source_list()}
        if initial_names != final_names:
            unchanged = False
            self.diagnostic("source-set-changed", "KScreen output source set changed during capture", severity="error")
        for entry in self.sources:
            current, _ = self.fingerprint(Path(entry["source"]))
            entry["after"] = current
            entry["unchanged"] = current == entry["before"]
            if not entry["unchanged"]:
                unchanged = False
                self.diagnostic("source-changed", "Source changed across capture; this is not a consistent snapshot", source=entry["source"], severity="error")
        return unchanged

    def run(self):
        self.destination.mkdir(mode=0o700)
        self.created = True
        self.destination.chmod(0o700)
        self.put(".gitignore", "*\n")
        self.put("PRIVACY.txt", "PRIVATE RECOVERY AND UNREVIEWED CANDIDATES\nDo not commit this directory, add it as a flake input, or copy it wholesale to the Nix store.\nRecovery is separate from reviewed declarative intent. Review every value, endpoint, path, widget source and license.\nNeither capture nor generated files activate Home Manager, switch Activities, delete panels or restore configuration.\n")
        before = self.session_state("before")
        native, layout = self.native_layout("before")
        outputs_before = self.output_state("before")
        self.copy_sources()
        if native:
            self.put("recovery/session/layout.js", native)
        self.put_json("recovery/session/before.json", before)
        converter = self.convert_rc2nix()
        self.asset_inventory(layout)
        self.review_inputs(native, layout, before, outputs_before)
        after = self.session_state("after")
        native_after, _ = self.native_layout("after")
        outputs_after = self.output_state("after")
        self.put_json("recovery/session/after.json", after)
        if native_after:
            self.put("recovery/session/layout-after.js", native_after)
        files_unchanged = self.verify_sources()
        state_fields = ("current_activity", "activity_ids", "virtual_desktops")
        session_available = all(before.get(field) is not None and after.get(field) is not None for field in state_fields)
        session_unchanged = session_available and all(before[field] == after[field] for field in state_fields)
        layout_unchanged = bool(native and native_after and native == native_after)
        outputs_available = outputs_before["status"] == "ok" and outputs_after["status"] == "ok"
        outputs_unchanged = outputs_available and outputs_before.get("outputs") == outputs_after.get("outputs")
        if not session_available:
            self.diagnostic("session-unobserved", "No complete D-Bus Activity/desktop state; offline recovery is not a complete session capture")
        elif not session_unchanged:
            self.diagnostic("session-changed", "Activity or virtual-desktop state changed during capture", severity="error")
        if not layout_unchanged:
            self.diagnostic("layout-consistency-unproven", "Native layout is missing or changed between observations", severity="error" if native and native_after else "warning")
        if not outputs_unchanged:
            self.diagnostic("output-consistency-unproven", "Live output state is unavailable or changed between observations")
        warnings = any(item["severity"] in ("warning", "error") for item in self.diagnostics)
        consistent = files_unchanged and session_unchanged and layout_unchanged and outputs_unchanged
        status = "captured-needs-review" if consistent and not warnings and converter["status"] == "ok" else "incomplete"
        self.put_json("recovery/session/dbus-queries.json", self.bus_queries)
        self.put_json("review/assets.json", self.assets)
        self.put_json("review/privacy-findings.json", self.privacy)
        self.put_json("review/diagnostics.json", self.diagnostics)
        self.put_json("manifest.json", {
            "format_version": 1, "started_at": self.start, "finished_at": timestamp(), "status": status,
            "offline": self.offline, "sources": self.sources, "converter": converter,
            "consistency": {"source_files_unchanged": files_unchanged, "session_state_observed": session_available,
                            "session_state_unchanged": session_unchanged, "native_layout_unchanged": layout_unchanged,
                            "outputs_unchanged": outputs_unchanged,
                            "limit": "Read-only observations are not an atomic snapshot. Unsaved/transient state and changes that revert between observations cannot be excluded. Source atime may change on filesystems that track reads."},
            "native_layout_sha256": digest(native.encode()) if native else None,
            "native_layout_after_sha256": digest(native_after.encode()) if native_after else None,
            "restore_verified": False, "review_required": True,
            "excluded": ["wallets", "browser/application profiles", "activity usage database", "agent/runtime sockets", "external asset contents"],
            "boundary": "No automatic HM import, store export, publishing or restore. Complete restore requires reviewed asset sources/output mapping plus fresh-guest tests; parsing Nix alone is insufficient.",
        })
        self.put("REVIEW.txt", "Capture status: " + status + "\n"
                 "1. Keep recovery/ private. It is allowlisted raw recovery, not a declarative module.\n"
                 "2. Inspect manifest.json, review/diagnostics.json and review/privacy-findings.json. Missing DBus or changed sources means incomplete capture (exit 2).\n"
                 "3. Review rc2nix.nix plus appearance.nix (settings.nix merges their programs.plasma data for the baseline.settings boundary). Values may contain secrets, personal endpoints and stale store paths. No automated scan proves absence of secrets.\n"
                 "4. Inspect applets.json, native-layout.json, output-mapping.json, identities.json and assets.json. Resolve pinned redistributable sources or explicit machine-local resources, and matching decoration libraries. Asset metadata version is not a source revision.\n"
                 "5. Build a separate reviewed module outside this capture; only that sanitized data may enter Nix. Keep programs.plasma.overrideConfig = false; do not import applets-config.nix or layout.nix implicitly.\n"
                 "6. Raw recovery and declarative reconstruction are DIFFERENT fresh-guest restore tests. layout.nix requires explicit restoreLayout = true, can append duplicates, exports only current Activity desktops, and may need reviewed output mapping. Do not use on a running baseline.\n"
                 "7. Never infer successful restore from capture success, a native export or parseable converter output. No restore was executed.\n")
        print("Private Plasma capture: " + status + "; see manifest.json and REVIEW.txt")
        return 0 if status == "captured-needs-review" else 2


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Read-only, allowlisted Plasma recovery and unreviewed plasma-manager candidates into a NEW private directory. Never activates or restores anything.",
        epilog="Exit 0: stable observations captured, human review/restore proof still required. Exit 2: incomplete capture or invalid invocation. Missing session bus, assets, converter or inconsistent observations are not hidden. Never put raw captures in a flake/store/commit.")
    parser.add_argument("--destination", required=True, type=Path, help="explicit absent destination; parent must already exist; directories 0700, files 0600")
    parser.add_argument("--offline", action="store_true", help="only file recovery/review; no DBus/output queries; reports incomplete (exit 2)")
    arguments = parser.parse_args(argv)
    destination = arguments.destination.expanduser()
    if not destination.is_absolute():
        parser.error("--destination must be an absolute path")
    destination = destination.parent.resolve() / destination.name
    if not destination.parent.is_dir():
        parser.error("destination parent must already exist")
    if destination.exists() or destination.is_symlink():
        parser.error("destination already exists; refusing overwrite")
    capture = Capture(destination, arguments.offline)
    if any(destination.is_relative_to(root) for root in (capture.config_home, capture.data_home, Path("/nix/store"))):
        parser.error("destination must not be inside source XDG configuration/data or /nix/store")
    old_umask = os.umask(0o077)
    try:
        return capture.run()
    except (OSError, ValueError, TypeError, KeyError) as error:
        # A partial destination is deliberately left private for diagnosis. Never
        # remove it or silently overwrite on retry; choose a new destination.
        if capture.created:
            try:
                capture.put_json("CAPTURE-FAILED.json", {"status": "failed", "error": str(error), "restore_verified": False})
            except OSError:
                pass
        print("Plasma capture failed; private destination may be partial: " + str(error), file=sys.stderr)
        return 2
    finally:
        os.umask(old_umask)
