# GNOME integration tests (Wayland)

These scripts start a headless GNOME Shell 50 (`gnome-shell --headless --wayland --virtual-monitor`),
gnome-settings-daemon's media-keys plugin and a few GTK 4 test windows, then drive Beam with real
keyboard / pointer input through `beam-test@local`, a test-only Shell extension (eval, screenshots,
Clutter virtual input devices). Beam's hidden `--dump-state` option prints what the launcher shows.

| Phase | What it checks |
|---|---|
| 1 | first run registers Alt+Space as a GNOME custom shortcut and frees it from the window menu; the shortcut shows Beam with keyboard focus; typing, toggling, click-outside, overview |
| 2 | installing the window-switcher extension from Beam; after a new "login": listing, switching (previous window preselected), Super+W switcher hotkey, Ctrl+W, windows in the root search |
| 3 | 注音 (IBus Chewing): Enter commits the composition instead of running an action, Esc cancels it, Super+Space does not close Beam |
| 4 | launching desktop apps and desktop actions hands focus to the new window, clipboard, terminal commands, a Shell plugin |
| 5 | the shortcut starts Beam when it is not running, autostart entry, dark style, `beam --unregister` |
| 6 | `beam --doctor` before / after the first start; repairing a deleted shortcut and the window menu; conflicts with another custom shortcut and with an extension keybinding (also reported when Beam starts); gsd-media-keys not running |
| 7 | GNOME Shell refuses Beam's shortcut (keys still taken at that moment; gsd-media-keys never retries): healed when Beam starts, by `beam --doctor`, and right after registering (retry); after three refusals Beam gives up and says so; the doctor names an app's global shortcut (portal) and an extension by name, and prints a Looking Glass probe that names the holder when no setting explains it |

Beam reads gsd-media-keys' messages with journalctl; the headless session has no journal, so `fakebin/journalctl` serves them from `gsd.log`.

Run them only in a throw-away container or chroot (they wipe and replace `HOME`, `XDG_RUNTIME_DIR` and
the GNOME settings): `BEAM=/path/to/beam ./run-all.sh`.
