#!/usr/bin/env python3
"""Real guest applications and notification callbacks, not mocked service responses."""
import argparse
import json
import os
from pathlib import Path
import sys

import dbus
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib


def notification(args):
    DBusGMainLoop(set_as_default=True)
    bus = dbus.SessionBus()
    events = Path(args.events)
    events.parent.mkdir(parents=True, exist_ok=True)
    def record(kind, *values):
        with events.open('a') as stream:
            stream.write(json.dumps({'event': kind, 'values': [str(v) for v in values]}) + '\n')
    bus.add_signal_receiver(lambda *v: record('action', *v), signal_name='ActionInvoked', dbus_interface='org.freedesktop.Notifications')
    bus.add_signal_receiver(lambda *v: record('closed', *v), signal_name='NotificationClosed', dbus_interface='org.freedesktop.Notifications')
    server = dbus.Interface(bus.get_object('org.freedesktop.Notifications', '/org/freedesktop/Notifications'), 'org.freedesktop.Notifications')
    actions = ['default', 'Open lab marker']
    hints = {'resident': dbus.Boolean(True), 'urgency': dbus.Byte(args.urgency)}
    if args.reply:
        # Plasma marks a notification reply-capable from the inline-reply action.
        # The placeholder hint only labels the field it renders.
        actions = ['inline-reply', 'Reply', *actions]
        hints['x-kde-reply-placeholder-text'] = 'Reply to Caelestia Lab'
    identifier = server.Notify('Caelestia Lab', dbus.UInt32(args.replaces), 'dialog-information', args.title,
        'A real actionable notification', dbus.Array(actions, signature='s'),
        dbus.Dictionary(hints, signature='sv'), dbus.Int32(0))
    record('sent', identifier)
    Path(args.identifier).write_text(str(identifier))
    loop = GLib.MainLoop()
    GLib.timeout_add_seconds(args.seconds, lambda: loop.quit() or False)
    loop.run()


def window(args):
    from PySide6.QtCore import QTimer
    from PySide6.QtGui import QGuiApplication, QColor
    from PySide6.QtQuick import QQuickView
    app = QGuiApplication(sys.argv[:1])
    app.setApplicationName('caelestia-lab-' + args.title.lower().replace(' ', '-'))
    view = QQuickView()
    view.setTitle(args.title)
    view.setColor(QColor('#29415b'))
    view.resize(420, 280)
    view.show()
    marker = Path(args.events)
    marker.parent.mkdir(parents=True, exist_ok=True)
    def record(kind):
        with marker.open('a') as stream:
            stream.write(json.dumps({'event': kind, 'title': args.title, 'pid': os.getpid()}) + '\n')
    view.activeChanged.connect(lambda: record('active' if view.isActive() else 'inactive'))
    app.aboutToQuit.connect(lambda: record('closed'))
    record('opened')
    QTimer.singleShot(args.seconds * 1000, app.quit)
    app.exec()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kind', choices=['notification', 'window'])
    parser.add_argument('--title', required=True)
    parser.add_argument('--events', required=True)
    parser.add_argument('--identifier', default='/tmp/caelestia-lab-notification-id')
    parser.add_argument('--replaces', type=int, default=0)
    parser.add_argument('--urgency', type=int, default=1)
    parser.add_argument('--reply', action='store_true')
    parser.add_argument('--seconds', type=int, default=600)
    args = parser.parse_args()
    (notification if args.kind == 'notification' else window)(args)


if __name__ == '__main__':
    main()
