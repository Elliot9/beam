// Global hotkeys on GNOME. Wayland does not let programs grab keys, so Beam registers
// GNOME custom keyboard shortcuts that run `beam --toggle` / `beam --windows`.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace beam {

struct HotkeySetup {
    bool supported = false;  // GNOME custom shortcuts are available
    bool changed = false;    // something in GNOME's settings was written
    std::wstring problem;    // invalid hotkey text, or a conflict Beam left alone
    std::wstring note;       // what Beam changed besides its own shortcuts
};

// Something else bound to the same keys.
struct HotkeyConflict {
    bool windowMenu = false;  // GNOME's window menu (Alt+Space by default): Beam takes these keys over
    std::wstring what;        // "GNOME 的「switch-input-source」快捷鍵", "自訂快捷鍵「Ulauncher」"...
};

// Beam's own custom shortcut as GNOME has it.
struct GnomeShortcut {
    bool listed = false;  // in media-keys custom-keybindings (gsd-media-keys only grabs listed ones)
    std::string name, command, binding;
};

bool GnomeShortcutsSupported();
// "Alt+Space" -> "<Alt>space"; empty when the text is not a usable hotkey.
std::string GnomeAccelerator(const std::wstring& hotkey);
// The command of Beam's shortcut: "/usr/bin/beam --toggle" (or --windows).
std::string BeamShortcutCommand(const std::string& executable, bool windowSwitcher);
GnomeShortcut ReadBeamShortcut(bool windowSwitcher);
// GNOME's own shortcuts, other custom shortcuts and GNOME Shell extensions on these keys.
std::vector<HotkeyConflict> FindHotkeyConflicts(const std::string& accelerator);
// Registering a hotkey takes two steps, with a pause in between (see app.cpp):
// 1. GNOME's own shortcuts on the same keys: the window menu's Alt+Space is released
//    (setup.changed); every other conflict is only reported (setup.problem).
HotkeySetup ReleaseHotkeyConflicts(const std::wstring& launcherHotkey, const std::wstring& windowsHotkey);
// 2. Beam's custom shortcuts (an empty windowsHotkey removes the second one). gsd-media-keys then
//    asks GNOME Shell to grab the keys, once. `regrab` lists them anew even when nothing changed,
//    which makes gsd-media-keys ask again.
struct ShortcutWrite {
    bool changed = false;  // values written (new or different shortcut)
    bool asked = false;    // gsd-media-keys was made to grab the keys
};
ShortcutWrite WriteGnomeShortcuts(const std::wstring& launcherHotkey, const std::wstring& windowsHotkey,
                                  const std::string& executable, bool regrab);
// Removes Beam's shortcuts and gives Alt+Space back to GNOME's window menu (beam --unregister).
void UnregisterGnomeShortcuts();
// gsd-media-keys gives up when GNOME Shell refuses to grab a shortcut's keys (they still belong to
// something else), logging "Failed to grab accelerator for keybinding custom:<path>". It is the
// only trace of a dead shortcut.
long MediaKeysPid();  // the running gsd-media-keys; 0 if none
// How many such refusals of Beam's shortcuts the gsd-media-keys process `gsdPid` logged (journal,
// this boot), and when the last one was ("13:05"). -1: no journal to read.
int CountShortcutGrabFailures(long gsdPid, std::string* lastTime);
void CountShortcutGrabFailuresAsync(long gsdPid, std::function<void(int)> done);
// JavaScript for GNOME's Looking Glass (Alt+F2, lg) that names whatever holds the hotkey's keys
// inside GNOME Shell (a GNOME shortcut, a program and its app, an extension's keybinding): the
// shell keeps that to itself. Empty if the keys cannot be looked up (needs the display).
std::string KeyOwnerProbe(const std::wstring& hotkey);

}  // namespace beam
