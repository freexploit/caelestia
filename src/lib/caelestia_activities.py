# SPDX-License-Identifier: GPL-3.0-or-later
"""One serialized owner for reversible Plasma/Caelestia Activity transitions."""
import argparse
import asyncio
import contextlib
import fcntl
import json
import os
from pathlib import Path
import signal
import sys
import tempfile
import uuid

from dbus_next import Message, MessageType, Variant
from dbus_next.aio import MessageBus

ACTIVITY_SERVICE = "org.kde.ActivityManager"
ACTIVITY_PATH = "/ActivityManager/Activities"
ACTIVITY_IFACE = "org.kde.ActivityManager.Activities"
SHORTCUT_SERVICE = "org.caelestia.Shortcuts"
SHORTCUT_PATH = "/org/caelestia/Shortcuts"
PRESENTER_SERVICE = "org.caelestia.PlasmaNotifications"
PRESENTER_PATH = "/org/caelestia/PlasmaNotifications"

# Sentinel so save(state=...) can leave a recorded error alone while
# save(error=None) removes it.
_UNSET = object()
RETRY_DELAYS = (2, 5, 15, 60)


class EntryFailure(RuntimeError):
    """An entry failure that may resolve without an Activity switch."""

    def __init__(self, message, transient=False):
        super().__init__(message)
        self.transient = transient

PANEL_QUERY = r'''print(JSON.stringify(panels().map(function(p) {
    return {id:p.id,screen:p.screen,location:p.location,height:p.height,widgets:p.widgets().filter(function(w) {
        return w.type === "luisbocanegra.panel.colorizer";
    }).map(function(w) {
        w.currentConfigGroup=["General"];
        return {id:w.id,settings:w.readConfig("globalSettings","{}"),autoload:w.readConfig("presetAutoloading","{}"),
            service:w.readConfig("enableDBusService",true),enabled:w.readConfig("isEnabled",true)};
    })};
})));'''


def plain(value):
    if isinstance(value, Variant):
        return plain(value.value)
    if isinstance(value, dict):
        return {k: plain(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [plain(v) for v in value]
    return value


def atomic_json(path, data):
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".activities-", dir=path.parent)
    try:
        with os.fdopen(fd, "w") as stream:
            json.dump(data, stream, indent=2)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


class Coordinator:
    def __init__(self, config):
        self.config = config
        self.state_dir = Path(os.environ.get("XDG_STATE_HOME", str(Path.home() / ".local/state"))) / "caelestia"
        self.journal_path = self.state_dir / "activities.json"
        self.identity_path = self.state_dir / "activity-identity.json"
        self.bus = None
        self.process = None
        self.child_task = None
        self.wake = asyncio.Event()
        self.stopping = False
        self.desired = ""
        self.target = config.get("activity")
        self.retry_index = 0
        self.retry_at = 0.0
        self.failure = None
        self.journal = {"version": 1, "state": "baseline", "panels": [], "meta": None, "trackerOwned": False}
        if self.journal_path.exists():
            self.journal = json.loads(self.journal_path.read_text())
        self.lock_file = None

    def lock(self):
        self.state_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.lock_file = open(self.state_dir / "activities.lock", "a+")
        os.chmod(self.lock_file.name, 0o600)
        try:
            fcntl.flock(self.lock_file, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as exc:
            raise RuntimeError("Activity coordinator is already running; stop its user service before recovery") from exc

    def save(self, state=None, error=_UNSET):
        if state:
            self.journal["state"] = state
        if error is not _UNSET:
            if error is None:
                self.journal.pop("error", None)
            else:
                self.journal["error"] = error
        atomic_json(self.journal_path, self.journal)

    def reset_retry(self):
        self.failure = None
        self.retry_index = 0
        self.retry_at = 0.0

    def retry_ready(self):
        return self.failure is None or self.retry_at <= asyncio.get_running_loop().time()

    def record_failure(self, exc):
        transient = isinstance(exc, EntryFailure) and exc.transient
        state = (transient, str(exc))
        if self.failure != state:
            print(str(exc), file=sys.stderr, flush=True)
            self.failure = state
        if transient:
            delay = RETRY_DELAYS[self.retry_index]
            self.retry_index = min(self.retry_index + 1, len(RETRY_DELAYS) - 1)
            self.retry_at = asyncio.get_running_loop().time() + delay
        else:
            self.retry_at = float("inf")

    async def call(self, service, path, interface, member, signature="", body=None):
        reply = await asyncio.wait_for(self.bus.call(Message(destination=service, path=path, interface=interface,
            member=member, signature=signature, body=body or [])), timeout=10)
        if reply.message_type == MessageType.ERROR:
            raise RuntimeError(f"{service}.{member}: {reply.error_name}: {reply.body}")
        return plain(reply.body)

    async def connect(self):
        self.bus = await MessageBus().connect()
        self.bus.add_message_handler(self.message)
        for rule in [
            "type='signal',interface='org.kde.ActivityManager.Activities',member='CurrentActivityChanged'",
            "type='signal',interface='org.freedesktop.DBus',member='NameOwnerChanged'",
        ]:
            await self.call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "AddMatch", "s", [rule])

    def message(self, message):
        if message.message_type != MessageType.SIGNAL:
            return
        if message.interface == ACTIVITY_IFACE and message.member == "CurrentActivityChanged":
            self.desired = message.body[0]
            self.reset_retry()
            self.wake.set()
        elif message.interface == "org.freedesktop.DBus" and message.member == "NameOwnerChanged":
            name, old, new = message.body
            if name in [ACTIVITY_SERVICE, "org.kde.plasmashell", "org.kde.KWin", PRESENTER_SERVICE]:
                self.reset_retry()
                self.wake.set()

    async def current(self):
        return (await self.call(ACTIVITY_SERVICE, ACTIVITY_PATH, ACTIVITY_IFACE, "CurrentActivity"))[0]

    async def resolve_identity(self):
        baseline = str(uuid.UUID(self.config["baselineActivity"]))
        activities = (await self.call(ACTIVITY_SERVICE, ACTIVITY_PATH, ACTIVITY_IFACE, "ListActivities"))[0]
        if baseline not in activities:
            raise RuntimeError("Configured baseline Activity UUID does not exist")
        if self.target:
            self.target = str(uuid.UUID(self.target))
            if self.target not in activities:
                raise RuntimeError("Configured Caelestia Activity UUID does not exist")
        else:
            name = self.config.get("createActivity")
            if not name:
                raise RuntimeError("Set exactly one Activity UUID or explicit createActivity name")
            if self.identity_path.exists():
                identity = json.loads(self.identity_path.read_text())
                if identity.get("name") != name or identity.get("uuid") not in activities:
                    raise RuntimeError("Persisted Activity identity no longer matches; review it explicitly rather than recreating")
                self.target = identity["uuid"]
            else:
                matches = []
                for activity in activities:
                    current_name = (await self.call(ACTIVITY_SERVICE, ACTIVITY_PATH, ACTIVITY_IFACE, "ActivityName", "s", [activity]))[0]
                    if current_name == name:
                        matches.append(activity)
                if len(matches) > 1:
                    raise RuntimeError("Activity name is ambiguous; specify its UUID")
                self.target = matches[0] if matches else (await self.call(ACTIVITY_SERVICE, ACTIVITY_PATH, ACTIVITY_IFACE, "AddActivity", "s", [name]))[0]
                self.target = str(uuid.UUID(self.target))
                atomic_json(self.identity_path, {"name": name, "uuid": self.target})
        if self.target == baseline:
            raise RuntimeError("Baseline and Caelestia Activities must be distinct")

    async def evaluate(self, source):
        result = (await self.call("org.kde.plasmashell", "/PlasmaShell", "org.kde.PlasmaShell", "evaluateScript", "s", [source]))[0]
        return json.loads(result.strip())

    async def panels(self):
        return await self.evaluate(PANEL_QUERY)

    @staticmethod
    def visibility(widget):
        return json.loads(widget["settings"]).get("stockPanelSettings", {}).get("visible", None)

    async def set_visibility(self, panel, widget, value):
        # Some Panel Colorizer releases ship without the DBusSignalMonitor
        # component, so the property signal never reaches the widget. Write the
        # colorizer's own config instead and verify it through the same read
        # path the coordinator uses everywhere else.
        script = '''(function(){
            var ps = panels();
            for (var i = 0; i < ps.length; i++) {
                if (ps[i].id !== %d) continue;
                var ws = ps[i].widgets();
                for (var j = 0; j < ws.length; j++) {
                    if (ws[j].id !== %d) continue;
                    ws[j].currentConfigGroup = ["General"];
                    var g = JSON.parse(ws[j].readConfig("globalSettings", "{}"));
                    g.stockPanelSettings = g.stockPanelSettings || {};
                    g.stockPanelSettings.visible = %s;
                    ws[j].writeConfig("globalSettings", JSON.stringify(g));
                }
            }
            print("ok");
        })()''' % (int(panel), int(widget), json.dumps(value, separators=(",", ":")))
        await self.call("org.kde.plasmashell", "/PlasmaShell", "org.kde.PlasmaShell", "evaluateScript", "s", [script])
        deadline = asyncio.get_running_loop().time() + 5
        while asyncio.get_running_loop().time() < deadline:
            for current in await self.panels():
                if current["id"] == panel:
                    for item in current["widgets"]:
                        if item["id"] == widget and self.visibility(item) == value:
                            return
            await asyncio.sleep(0.05)
        raise RuntimeError(f"Panel {panel} visibility change was not acknowledged")

    async def preflight(self):
        panels = await self.panels()
        if not panels:
            raise RuntimeError("No baseline Plasma panels found; refusing an unverified surface transition")
        for panel in panels:
            if len(panel["widgets"]) != 1:
                raise RuntimeError(f"Panel {panel['id']} requires exactly one supported Panel Colorizer widget")
            widget = panel["widgets"][0]
            autoload = json.loads(widget["autoload"])
            if autoload.get("enabled", False):
                raise RuntimeError("Panel Colorizer preset auto-loading must be disabled before managed coexistence")
            self.visibility(widget)
        snapshot = (await self.call(PRESENTER_SERVICE, PRESENTER_PATH, PRESENTER_SERVICE, "Snapshot"))[0]
        if not snapshot.get("supported"):
            # Capability-state failure: retry with backoff, because a plasmashell
            # restart or applet reload can make the presenter usable again
            # without any Activity change.
            raise EntryFailure("The scoped Plasma notification presenter is not supported: " + str(snapshot),
                transient=True)
        if self.config.get("trackerRequired", True):
            supported = (await self.call("org.kde.KWin", "/Effects", "org.kde.kwin.Effects", "isEffectSupported", "s", ["kwin_workspace_tracker"]))[0]
            if not supported:
                raise RuntimeError("Matching KWin workspace tracker is not discoverable/supported")
            loaded = (await self.call("org.kde.KWin", "/Effects", "org.kde.kwin.Effects", "isEffectLoaded", "s", ["kwin_workspace_tracker"]))[0]
            if not loaded:
                self.journal["trackerOwned"] = True
                self.save()
                if not (await self.call("org.kde.KWin", "/Effects", "org.kde.kwin.Effects", "loadEffect", "s", ["kwin_workspace_tracker"]))[0]:
                    raise RuntimeError("KWin workspace tracker failed to load")
            await self.call("org.kde.KWin", "/Caelestia/Workspaces", "org.freedesktop.DBus.Introspectable", "Introspect")
        return panels

    async def ipc(self, method):
        process = await asyncio.create_subprocess_exec("quickshell", "-p", os.environ["CAELESTIA_SHELL_CONFIG"],
            "ipc", "--any-display", "call", "managed", method,
            stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
        output, error = await asyncio.wait_for(process.communicate(), timeout=5)
        if process.returncode:
            raise RuntimeError(error.decode().strip() or "Managed shell IPC failed")
        text = output.decode().strip()
        if text in ["true", "false"]:
            return text == "true"
        return json.loads(text)

    async def check_shortcuts(self):
        status = (await self.call(SHORTCUT_SERVICE, SHORTCUT_PATH, SHORTCUT_SERVICE, "status"))[0]
        if not status.get("ok") or status.get("pending"):
            raise RuntimeError("Shortcut registration failed: " + str(status))

    async def command(self, *args):
        process = await asyncio.create_subprocess_exec(*args, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
        output, error = await process.communicate()
        if process.returncode:
            raise RuntimeError(f"{args[0]} failed: {error.decode().strip()}")
        return output.decode().strip()

    async def enter(self):
        self.save("entering", error=None)
        panels = await self.preflight()
        env = dict(os.environ, CAELESTIA_ACTIVITY_MANAGED="1", CAELESTIA_MANAGED="1")
        self.process = await asyncio.create_subprocess_exec(*self.config["shellCommand"], env=env, start_new_session=True)
        self.child_task = asyncio.create_task(self.process.wait())
        self.child_task.add_done_callback(lambda _: self.wake.set())
        deadline = asyncio.get_running_loop().time() + self.config.get("startupTimeout", 30)
        while True:
            if self.process.returncode is not None:
                raise RuntimeError("Caelestia exited during startup")
            if await self.current() != self.target:
                raise RuntimeError("Activity changed during shell entry")
            try:
                status = await self.ipc("status")
                if status.get("ready") and status.get("activity") == self.target:
                    break
            except (RuntimeError, asyncio.TimeoutError, json.JSONDecodeError):
                pass
            if asyncio.get_running_loop().time() >= deadline:
                raise RuntimeError("Caelestia did not reach managed readiness")
            await asyncio.sleep(0.1)
        if not await self.ipc("commit"):
            raise RuntimeError("Notification/surface commit failed")
        await self.check_shortcuts()
        for panel in panels:
            widget = panel["widgets"][0]
            record = {"panel": panel["id"], "widget": widget["id"], "before": self.visibility(widget),
                "owned": {"enabled": True, "value": False}}
            self.journal["panels"].append(record)
            self.save()
            await self.set_visibility(record["panel"], record["widget"], record["owned"])
        self.save("caelestia", error=None)

    async def leave(self):
        self.save("recovering")
        errors = []
        if self.process is not None:
            with contextlib.suppress(Exception):
                await self.ipc("release")
            if self.process.returncode is None:
                self.process.terminate()
                try:
                    await asyncio.wait_for(self.process.wait(), 8)
                except asyncio.TimeoutError:
                    self.process.kill()
                    await self.process.wait()
            self.process = None
        try:
            await self.command("caelestia-shortcut-recovery", "recover")
        except Exception as exc:
            errors.append(str(exc))
        remaining = []
        if self.journal.get("panels"):
            try:
                panels = await self.panels()
                for record in self.journal["panels"]:
                    try:
                        widget = next((w for p in panels if p["id"] == record["panel"] for w in p["widgets"] if w["id"] == record["widget"]), None)
                        if widget and self.visibility(widget) == record["owned"]:
                            await self.set_visibility(record["panel"], record["widget"], record["before"] or {"enabled": False, "value": True})
                        elif not widget:
                            raise RuntimeError(f"Panel {record['panel']} unavailable for recovery")
                    except Exception as exc:
                        errors.append(str(exc))
                        remaining.append(record)
            except Exception as exc:
                errors.append(str(exc))
                remaining = self.journal["panels"]
        self.journal["panels"] = remaining
        if self.journal.get("trackerOwned"):
            try:
                await self.call("org.kde.KWin", "/Effects", "org.kde.kwin.Effects", "unloadEffect", "s", ["kwin_workspace_tracker"])
                self.journal["trackerOwned"] = False
            except Exception as exc:
                errors.append(str(exc))
        self.save("recovering" if errors else "baseline", "; ".join(errors) if errors else None)
        if errors:
            raise RuntimeError("Recovery incomplete: " + "; ".join(errors))

    async def run(self, recover=False):
        self.lock()
        await self.connect()
        await self.leave()
        if recover:
            return
        await self.resolve_identity()
        loop = asyncio.get_running_loop()
        for number in [signal.SIGINT, signal.SIGTERM]:
            loop.add_signal_handler(number, self.stop)
        try:
            while not self.stopping:
                self.wake.clear()
                try:
                    self.desired = await self.current()
                    if self.process and self.process.returncode is not None:
                        await self.leave()
                        self.save(error="Caelestia exited")
                        self.record_failure(RuntimeError("Caelestia exited"))
                    if self.desired != self.target:
                        self.reset_retry()
                        if self.process or self.journal.get("state") != "baseline":
                            await self.leave()
                        elif "error" in self.journal:
                            self.save(error=None)
                    elif not self.process and self.retry_ready():
                        await self.enter()
                        self.reset_retry()
                    elif self.process:
                        # Rediscover external service/output changes without overlapping transitions.
                        await self.preflight()
                except Exception as exc:
                    await self.leave()
                    self.save(error=str(exc))
                    self.record_failure(exc)
                if not self.stopping:
                    try:
                        await asyncio.wait_for(self.wake.wait(), timeout=2)
                    except asyncio.TimeoutError:
                        pass
        finally:
            await self.leave()
            self.bus.disconnect()

    def stop(self):
        self.stopping = True
        self.wake.set()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--recover", action="store_true")
    parser.add_argument("--status", action="store_true")
    args = parser.parse_args()
    try:
        config = json.loads(args.config.read_text())
        if not isinstance(config.get("shellCommand"), list) or not config["shellCommand"] or not Path(config["shellCommand"][0]).is_absolute():
            raise ValueError("shellCommand must name an absolute managed launcher")
        coordinator = Coordinator(config)
        if args.status:
            print(json.dumps(coordinator.journal, indent=2))
            return 0
        asyncio.run(coordinator.run(recover=args.recover))
        return 0
    except (OSError, ValueError, RuntimeError) as exc:
        print(f"caelestia-activities: {exc}", file=sys.stderr)
        return 1
