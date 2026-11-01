#!/usr/bin/env python3
"""Real guest coexistence assertions; intended to run under the VM test driver."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time

from observe import BUS, FIXTURE, call, ipc, panel_state, ready

ROOT = Path('/home/lab/.local/state/caelestia-lab')


def wait_for(predicate, description, seconds=45):
    deadline = time.monotonic() + seconds
    last = None
    while time.monotonic() < deadline:
        try:
            result = predicate()
            if result:
                return result
        except Exception as error:
            last = error
        time.sleep(0.1)
    raise AssertionError(f'{description} timed out; last error: {last}')


def switch(activity):
    assert call('org.kde.ActivityManager', '/ActivityManager/Activities', 'org.kde.ActivityManager.Activities', 'SetCurrentActivity', activity)
    wait_for(lambda: call('org.kde.ActivityManager', '/ActivityManager/Activities', 'org.kde.ActivityManager.Activities', 'CurrentActivity') == activity, 'Activity switch')


def journal():
    return json.loads(Path('/home/lab/.local/state/caelestia/activities.json').read_text())


def enter():
    switch(FIXTURE['caelestia'])
    wait_for(lambda: journal()['state'] == 'caelestia', 'Coordinator entry')
    state = ipc('managed', 'status')
    assert state['ready'] and state['active']
    return state


def leave():
    switch(FIXTURE['baseline'])
    wait_for(lambda: journal()['state'] == 'baseline', 'Coordinator restoration')


def panel_identity(panels):
    return [(p['id'], p['screen'], p['location'], p['height'], [(w['id'], w['type']) for w in p['widgets']]) for p in panels]


def panels_case():
    leave()
    before = panel_state()
    enter()
    for panel in panel_state():
        settings = next(json.loads(w['settings']) for w in panel['widgets'] if w['type'] == 'luisbocanegra.panel.colorizer')
        assert settings['stockPanelSettings']['visible'] == {'enabled': True, 'value': False}
    leave()
    after = panel_state()
    assert panel_identity(before) == panel_identity(after), 'Panel identities/order/geometry changed'
    for _ in range(3):
        enter()
        leave()
    enter()
    subprocess.run(['systemctl', '--user', 'kill', '--kill-whom=all', '--signal=SIGKILL', 'caelestia-shell.service'], check=False)
    # Coordinator owns the shell directly; identify its exact packaged config rather than killing unrelated quickshells.
    subprocess.run(['pkill', '-KILL', '-f', 'quickshell.*share/caelestia/shell/shell.qml'], check=False)
    wait_for(lambda: journal()['state'] == 'baseline', 'Crash restoration')
    assert panel_identity(before) == panel_identity(panel_state())
    leave()
    return {'scenario': 'panel-identities-and-crash', 'panels': before, 'after': panel_state()}


def notifications_case():
    enter()
    ROOT.mkdir(parents=True, exist_ok=True)
    events = ROOT / 'notification-events.jsonl'
    identifier = ROOT / 'notification-id'
    client = subprocess.Popen(['lab-client', 'notification', '--title', 'Caelestia Lab Action', '--events', str(events), '--identifier', str(identifier)])
    try:
        nid = wait_for(lambda: identifier.read_text().strip() if identifier.exists() else None, 'Notification creation')
        item = wait_for(lambda: next((n for n in ipc('notifs', 'snapshot')['notifications'] if n['id'] == nid and n['popup']), None), 'Actual Caelestia notification popup')
        assert item['actions'] and item['presentation'], 'Actionable notification lacks rendered presentation'
        return {'scenario': 'notification-render', 'notification': item, 'owner': str(BUS.get_name_owner('org.freedesktop.Notifications'))}
    finally:
        client.terminate()
        client.wait(timeout=5)
        leave()


def capture_case():
    leave()
    tracked = ['kdeglobals', 'kwinrc', 'kglobalshortcutsrc', 'plasma-org.kde.plasma.desktop-appletsrc']
    config = Path('/home/lab/.config')
    def hashes():
        return {name: hashlib.sha256((config / name).read_bytes()).hexdigest() for name in tracked}
    before = hashes()
    destination = ROOT / 'capture'
    result = subprocess.run(['caelestia-plasma-capture', '--destination', str(destination)], text=True, capture_output=True)
    assert hashes() == before, 'Capture changed baseline configuration'
    assert result.returncode in [0, 2], result.stderr
    assert destination.stat().st_mode & 0o077 == 0
    assert (destination / 'recovery').is_dir() and (destination / 'review').is_dir()
    return {'scenario': 'read-only-capture', 'exitCode': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('scenario', choices=['panels', 'notifications', 'capture', 'ready'])
    args = parser.parse_args()
    ROOT.mkdir(parents=True, exist_ok=True)
    result = {'panels': panels_case, 'notifications': notifications_case, 'capture': capture_case, 'ready': ready}[args.scenario]()
    (ROOT / (args.scenario + '.json')).write_text(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
