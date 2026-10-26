// Beam Window Switcher: a small D-Bus service inside GNOME Shell. On Wayland other
// programs cannot list or activate windows themselves, so the Beam launcher asks here.
//
//   org.gnome.Shell  /local/beam/Windows  local.beam.Windows.List / Activate / Close
//
// Wayland keeps programs from seeing each other's windows on purpose, so only the installed
// Beam launcher may use the interface: a process running an executable called "beam" that only
// root can change (/usr/bin/beam), so another program cannot pass by copying itself to a file
// with that name. Beam run from a build folder is refused unless GNOME Shell was started with
// BEAM_WINDOWS_ALLOW_UNINSTALLED=1 (the GNOME integration tests do that).
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

const IFACE = `
<node>
  <interface name="local.beam.Windows">
    <method name="List">
      <arg type="s" direction="out" name="windows"/>
    </method>
    <method name="Activate">
      <arg type="s" direction="in" name="id"/>
      <arg type="b" direction="out" name="ok"/>
    </method>
    <method name="Close">
      <arg type="s" direction="in" name="id"/>
      <arg type="b" direction="out" name="ok"/>
    </method>
    <property name="Version" type="u" access="read"/>
  </interface>
</node>`;

// All normal windows on all workspaces, most recently used first.
function windowList() {
    return global.display.get_tab_list(Meta.TabList.NORMAL_ALL, null);
}

function findWindow(id) {
    return windowList().find(w => String(w.get_id()) === id) ?? null;
}

// Owned by root and writable by nobody else: a file or folder another user-level program cannot change.
function rootOwned(path, type) {
    const info = Gio.File.new_for_path(path).query_info('standard::type,unix::uid,unix::mode',
        Gio.FileQueryInfoFlags.NONE, null);
    return info.get_file_type() === type && info.get_attribute_uint32('unix::uid') === 0 &&
        (info.get_attribute_uint32('unix::mode') & 0o022) === 0;
}

function isBeamProcess(pid, allowUninstalled) {
    // "/usr/bin/beam", or "/usr/bin/beam (deleted)" right after a package upgrade
    const exe = GLib.file_read_link(`/proc/${pid}/exe`).replace(/ \(deleted\)$/, '');
    if (GLib.path_get_basename(exe) !== 'beam')
        return false;
    if (allowUninstalled)
        return true;
    // /proc/<pid>/exe is the file the process really runs, even if its path was replaced since.
    return rootOwned(`/proc/${pid}/exe`, Gio.FileType.REGULAR) &&
        rootOwned(GLib.path_get_dirname(exe), Gio.FileType.DIRECTORY);
}

class WindowService {
    constructor() {
        this._allowed = new Set();  // unique bus names already checked (never reused on a bus)
        this._allowUninstalled = GLib.getenv('BEAM_WINDOWS_ALLOW_UNINSTALLED') === '1';
    }

    get Version() {
        return 2;
    }

    // Asynchronous, so a slow bus never stalls GNOME Shell.
    _checkCaller(invocation, done) {
        const sender = invocation.get_sender();
        if (this._allowed.has(sender)) {
            done(true);
            return;
        }
        Gio.DBus.session.call('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',
            'GetConnectionUnixProcessID', new GLib.Variant('(s)', [sender]), new GLib.VariantType('(u)'),
            Gio.DBusCallFlags.NONE, 1000, null, (connection, result) => {
                let ok = false;
                try {
                    const [pid] = connection.call_finish(result).deepUnpack();
                    ok = isBeamProcess(pid, this._allowUninstalled);
                } catch (e) {
                    ok = false;
                }
                if (ok)
                    this._allowed.add(sender);
                done(ok);
            });
    }

    _handle(invocation, signature, produce) {
        this._checkCaller(invocation, ok => {
            if (!ok) {
                invocation.return_dbus_error('org.freedesktop.DBus.Error.AccessDenied',
                    'Only the installed Beam launcher may use this interface');
                return;
            }
            try {
                invocation.return_value(new GLib.Variant(signature, [produce()]));
            } catch (e) {
                invocation.return_dbus_error('org.freedesktop.DBus.Error.Failed', String(e));
            }
        });
    }

    ListAsync(_params, invocation) {
        this._handle(invocation, '(s)', () => {
            const tracker = Shell.WindowTracker.get_default();
            const windows = windowList()
                .filter(w => !w.skip_taskbar)
                .map(w => {
                    const app = tracker.get_window_app(w);
                    return {
                        id: String(w.get_id()),
                        title: w.get_title() ?? '',
                        app: app?.get_name() ?? '',
                        appId: app?.get_id() ?? '',
                        wmClass: w.get_wm_class() ?? '',
                        pid: w.get_pid(),
                        workspace: w.get_workspace()?.index() ?? -1,
                        minimized: w.minimized,
                    };
                });
            return JSON.stringify(windows);
        });
    }

    ActivateAsync([id], invocation) {
        this._handle(invocation, '(b)', () => {
            const window = findWindow(id);
            if (!window)
                return false;
            Main.activateWindow(window);  // switches workspace and unminimizes as needed
            return true;
        });
    }

    CloseAsync([id], invocation) {
        this._handle(invocation, '(b)', () => {
            const window = findWindow(id);
            if (!window)
                return false;
            window.delete(global.get_current_time());
            return true;
        });
    }
}

export default class BeamWindowsExtension extends Extension {
    enable() {
        this._service = new WindowService();
        this._dbus = Gio.DBusExportedObject.wrapJSObject(IFACE, this._service);
        this._dbus.export(Gio.DBus.session, '/local/beam/Windows');
    }

    disable() {
        this._dbus?.unexport();
        this._dbus = null;
        this._service = null;
    }
}
