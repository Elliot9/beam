// Window switching on GNOME through the bundled "Beam Window Switcher" Shell extension
// (on Wayland, only GNOME Shell itself can list and activate other programs' windows).
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "linux/results.hpp"

namespace beam {

struct WindowInfo {
    std::string id;
    std::wstring title;
    std::wstring appName;
    std::string appId;  // desktop id, e.g. "org.gnome.TextEditor.desktop"
    long pid = 0;
    GRef<GIcon> icon;
};

enum class ExtensionState {
    Running,        // answering on D-Bus
    NeedsRelogin,   // installed and enabled; GNOME Shell loads it at the next login (Wayland)
    NotInstalled,
    Unsupported,    // not a GNOME session
};

bool IsGnomeSession();  // XDG_CURRENT_DESKTOP mentions GNOME (Ubuntu: "ubuntu:GNOME")

class WindowService {
public:
    // Most recently used first, without Beam's own window. nullopt: the extension is not running.
    std::optional<std::vector<WindowInfo>> List(bool includeSelf = false);
    bool Activate(const std::string& id);
    bool Close(const std::string& id);
    // Asks GNOME Shell to focus Beam's own window (when the compositor did not give it focus).
    bool ActivateOwnWindow();

    ExtensionState State();
    ExtensionState StateWhenNotRunning();  // State() for when List() already failed
    // Puts the extension into ~/.local/share/gnome-shell/extensions (unless the package installed
    // it system-wide) and enables it. Returns what the user should do next.
    static bool InstallExtension(std::wstring* message);
    static void DisableExtension();  // beam --unregister

private:
    GIcon* IconFor(const std::string& appId);
    std::map<std::string, GRef<GIcon>> icons_;
};

}  // namespace beam
