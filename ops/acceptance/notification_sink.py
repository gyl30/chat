"""Observe actual desktop notifications on the acceptance run's private D-Bus."""

from pathlib import Path
import sys

import dbus.service
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib


class Notifications(dbus.service.Object):
    @dbus.service.method("org.freedesktop.Notifications", in_signature="susssasa{sv}i", out_signature="u")
    def Notify(self, app, replaces, icon, title, body, actions, hints, timeout):
        with (root / "notifications").open("a") as output:
            print(body, file=output)
        return dbus.UInt32(1)


root = Path(sys.argv[1])
DBusGMainLoop(set_as_default=True)
name = dbus.service.BusName("org.freedesktop.Notifications", dbus.SessionBus(), do_not_queue=True)
service = Notifications(name, "/org/freedesktop/Notifications")
(root / "bus-ready").touch()
GLib.MainLoop().run()
