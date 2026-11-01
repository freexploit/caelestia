import json
import shlex

start_all()
machine.wait_for_unit("display-manager.service")
machine.wait_for_file("/run/user/1000/bus")
user = "runuser -u lab -- env HOME=/home/lab XDG_RUNTIME_DIR=/run/user/1000 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus XDG_SESSION_TYPE=wayland "

def guest(command):
    return machine.succeed(user + command)

machine.wait_until_succeeds(user + "lab-observe ready", timeout=180)
ready = json.loads(guest("lab-observe ready"))
assert ready["sessionType"] == "wayland"
assert len(ready["panels"]) == 2
machine.screenshot("baseline-wayland")
machine.send_key("meta")
machine.screenshot("baseline-real-keyboard")
if STRICT:
    guest("lab-suite panels")
    machine.screenshot("panels-restored")
    guest("lab-suite notifications")
    guest("lab-suite capture")
    # Full acceptance cannot pass without each material behavior's guest evidence.
    for scenario in ["work-area-recovery", "keyboard-handoff", "notification-actions", "window-membership", "fresh-restore", "settings-layering", "service-output-recovery"]:
        machine.succeed("test -s /home/lab/.local/state/caelestia-lab/" + scenario + ".json")
    machine.copy_from_vm("/home/lab/.local/state/caelestia-lab", "evidence")
