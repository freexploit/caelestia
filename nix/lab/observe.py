#!/usr/bin/env python3
"""Guest-only Plasma/Wayland observations used by interactive and VM checks."""
import json
import os
from pathlib import Path
import subprocess
import sys

import dbus

BUS = dbus.SessionBus()
FIXTURE = json.loads(Path('/etc/caelestia-lab/fixture.json').read_text())


def plain(value):
    if isinstance(value, (dict, dbus.Dictionary)):
        return {str(k): plain(v) for k, v in value.items()}
    if isinstance(value, (list, tuple, dbus.Array, dbus.Struct)):
        return [plain(v) for v in value]
    if isinstance(value, dbus.Boolean):
        return bool(value)
    if isinstance(value, (dbus.Int16, dbus.Int32, dbus.Int64, dbus.UInt16, dbus.UInt32, dbus.UInt64, dbus.Byte)):
        return int(value)
    if isinstance(value, dbus.Double):
        return float(value)
    return str(value) if isinstance(value, (dbus.String, dbus.ObjectPath, dbus.Signature)) else value


def call(service, path, interface, method, *arguments):
    obj = BUS.get_object(service, path, introspect=False)
    return plain(obj.get_dbus_method(method, interface)(*arguments, timeout=15))


def evaluate(script):
    return json.loads(call('org.kde.plasmashell', '/PlasmaShell', 'org.kde.PlasmaShell', 'evaluateScript', script).strip())


def ipc(target, method, *arguments):
    root = os.environ.get('CAELESTIA_SHELL_CONFIG')
    if not root:
        root = subprocess.check_output(['readlink', '-f', '/run/current-system/sw/bin/caelestia-shell'], text=True).strip()
        root = str(Path(root).parents[1] / 'share/caelestia/shell/shell.qml')
    process = subprocess.run(['quickshell', '-p', root, 'ipc', '--any-display', 'call', target, method, *map(str, arguments)], text=True, capture_output=True)
    if process.returncode:
        raise RuntimeError(process.stderr.strip() or process.stdout.strip())
    text = process.stdout.strip()
    return json.loads(text) if text else None


def panel_state():
    return evaluate('''print(JSON.stringify(panels().map(function(p){return {id:p.id,screen:p.screen,location:p.location,height:p.height,hiding:p.hiding,widgets:p.widgets().map(function(w){w.currentConfigGroup=["General"];return {id:w.id,type:w.type,settings:w.readConfig("globalSettings", "{}")};})};})));''')


def output_state():
    return json.loads(subprocess.check_output(['kscreen-doctor', '-j'], text=True))


def ready():
    if os.environ.get('XDG_SESSION_TYPE') != 'wayland':
        raise RuntimeError('The lab must run inside the guest Plasma Wayland session')
    names = set(map(str, BUS.list_names()))
    required = {'org.kde.plasmashell', 'org.kde.KWin', 'org.kde.ActivityManager', 'org.freedesktop.Notifications'}
    if not required <= names:
        raise RuntimeError('Missing guest services: ' + repr(required - names))
    activity = call('org.kde.ActivityManager', '/ActivityManager/Activities', 'org.kde.ActivityManager.Activities', 'CurrentActivity')
    panels = panel_state()
    outputs = output_state().get('outputs', [])
    if len(panels) != 2 or len([o for o in outputs if o.get('enabled')]) != 2:
        raise RuntimeError(f'Expected two real outputs and panels, got {len(outputs)} outputs/{len(panels)} panels')
    return {'sessionType': 'wayland', 'activity': activity, 'panels': panels, 'outputs': outputs,
        'busId': str(BUS.get_id()), 'notificationOwner': str(BUS.get_name_owner('org.freedesktop.Notifications'))}


def main():
    command = sys.argv[1] if len(sys.argv) > 1 else 'state'
    if command == 'ready':
        value = ready()
    elif command == 'panels':
        value = panel_state()
    elif command == 'outputs':
        value = output_state()
    elif command == 'ipc':
        value = ipc(*sys.argv[2:])
    elif command == 'state':
        value = {'ready': ready(), 'managed': ipc('managed', 'status'), 'windows': ipc('kwin', 'windowState'), 'notifications': ipc('notifs', 'snapshot')}
    else:
        raise ValueError('Use ready|panels|outputs|state|ipc TARGET METHOD [ARGS]')
    print(json.dumps(value, indent=2))


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
