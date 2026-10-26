# Test helper: holds a GNOME Shell accelerator the way an app behind the global-shortcuts portal does
# (owns one of the names GNOME Shell lets grab keys). Usage: python3 grabber.py <bus name> <accel>
import sys
from gi.repository import Gio, GLib
name, accel = sys.argv[1], sys.argv[2]
bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
bus.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus', 'RequestName',
              GLib.Variant('(su)', (name, 4)), None, 0, -1, None)
r = bus.call_sync('org.gnome.Shell', '/org/gnome/Shell', 'org.gnome.Shell', 'GrabAccelerator',
                  GLib.Variant('(suu)', (accel, 1, 0)), GLib.VariantType('(u)'), 0, -1, None)
print('grabbed action', r.unpack()[0], flush=True)
GLib.MainLoop().run()
