# A plain GTK 4 window to switch to: python3 testwin.py "Title"
import sys
import gi
gi.require_version('Gtk', '4.0')
from gi.repository import Gtk

title = sys.argv[1]

def activate(app):
    win = Gtk.ApplicationWindow(application=app, title=title)
    win.set_default_size(420, 260)
    win.set_child(Gtk.Label(label=title))
    win.present()

app = Gtk.Application(application_id='local.beam.test.' + title.split()[0].lower())
app.connect('activate', activate)
app.run([])
