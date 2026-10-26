// Test-only driver for Beam's Wayland tests (never shipped): runs JS inside the shell,
// takes screenshots and injects keyboard / pointer input through Clutter virtual devices.
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

Gio._promisify(Shell.Screenshot.prototype, 'screenshot', 'screenshot_finish');

const IFACE = `
<node><interface name="local.beam.Test">
  <method name="Eval"><arg type="s" direction="in" name="code"/><arg type="s" direction="out" name="result"/></method>
  <method name="Screenshot"><arg type="s" direction="in" name="path"/><arg type="b" direction="out" name="ok"/></method>
  <method name="Keys"><arg type="s" direction="in" name="combo"/></method>
  <method name="Type"><arg type="s" direction="in" name="text"/></method>
  <method name="Clipboard"><arg type="s" direction="out" name="text"/></method>
  <method name="Click"><arg type="d" direction="in" name="x"/><arg type="d" direction="in" name="y"/><arg type="u" direction="in" name="button"/></method>
</interface></node>`;

const MODS = {ctrl: 'Control_L', control: 'Control_L', shift: 'Shift_L', alt: 'Alt_L', super: 'Super_L'};

class Driver {
    constructor() {
        // A headless seat has no keyboard: without one, windows never get keyboard focus.
        const seat = Clutter.get_default_backend().get_default_seat();
        this._seatKeyboard = seat.create_virtual_device(Clutter.InputDeviceType.KEYBOARD_DEVICE);
        this._ptr = seat.create_virtual_device(Clutter.InputDeviceType.POINTER_DEVICE);
    }

    // Each Keys() / Type() call gets a fresh keyboard: a long-lived virtual keyboard sometimes
    // stopped applying modifiers (keymap changes while the session starts), so no shortcut worked.
    _withKeyboard(fn) {
        this._kbd = Clutter.get_default_backend().get_default_seat()
            .create_virtual_device(Clutter.InputDeviceType.KEYBOARD_DEVICE);
        try {
            fn();
        } finally {
            this._kbd = null;
        }
    }


    _key(keyval, pressed) {
        this._kbd.notify_keyval(GLib.get_monotonic_time(), keyval,
            pressed ? Clutter.KeyState.PRESSED : Clutter.KeyState.RELEASED);
    }

    _keyval(name) {
        const v = Clutter[`KEY_${name}`];
        if (v === undefined)
            throw new Error(`unknown key ${name}`);
        return v;
    }

    Eval(code) {
        try {
            const fn = new Function('Main', 'Meta', 'Shell', 'GLib', 'Gio', 'Clutter', `return (${code});`);
            const r = fn(Main, Meta, Shell, GLib, Gio, Clutter);
            if (r === undefined) return 'undefined';
            return typeof r === 'string' ? r : JSON.stringify(r) ?? String(r);
        } catch (e) {
            return `ERROR: ${e}`;
        }
    }

    async ScreenshotAsync([path], invocation) {
        try {
            const file = Gio.File.new_for_path(path);
            const stream = file.replace(null, false, Gio.FileCreateFlags.NONE, null);
            const shooter = new Shell.Screenshot();
            await shooter.screenshot(false, stream);
            stream.close(null);
            invocation.return_value(new GLib.Variant('(b)', [true]));
        } catch (e) {
            invocation.return_value(new GLib.Variant('(b)', [false]));
        }
    }

    ClipboardAsync(params, invocation) {
        St.Clipboard.get_default().get_text(St.ClipboardType.CLIPBOARD, (_clipboard, text) => {
            invocation.return_value(new GLib.Variant('(s)', [text ?? '']));
        });
    }

    // "alt+space", "ctrl+shift+f", "Return", "Escape"
    Keys(combo) {
        this._withKeyboard(() => this._keys(combo));
    }

    _keys(combo) {
        const parts = combo.split('+');
        const key = parts.pop();
        const mods = parts.map(m => this._keyval(MODS[m.toLowerCase()] ?? m));
        const kv = key.length === 1 ? Clutter.unicode_to_keysym(key.codePointAt(0)) : this._keyval(key);
        for (const m of mods)
            this._key(m, true);
        this._key(kv, true);
        this._key(kv, false);
        for (const m of mods.reverse())
            this._key(m, false);
    }

    Type(text) {
        this._withKeyboard(() => this._type(text));
    }

    _type(text) {
        for (const ch of text) {
            const kv = Clutter.unicode_to_keysym(ch.codePointAt(0));
            const upper = ch !== ch.toLowerCase();
            if (upper)
                this._key(Clutter.KEY_Shift_L, true);
            this._key(kv, true);
            this._key(kv, false);
            if (upper)
                this._key(Clutter.KEY_Shift_L, false);
        }
    }

    Click(x, y, button) {
        const t = GLib.get_monotonic_time();
        this._ptr.notify_absolute_motion(t, x, y);
        this._ptr.notify_button(t + 1000, button, Clutter.ButtonState.PRESSED);
        this._ptr.notify_button(t + 2000, button, Clutter.ButtonState.RELEASED);
    }
}

export default class BeamTestExtension extends Extension {
    enable() {
        this._driver = new Driver();
        this._dbus = Gio.DBusExportedObject.wrapJSObject(IFACE, this._driver);
        this._dbus.export(Gio.DBus.session, '/local/beam/Test');
    }

    disable() {
        this._dbus?.unexport();
        this._dbus = null;
        this._driver = null;
    }
}
