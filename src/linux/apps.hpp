// Application index from desktop entries (GIO: /usr/share/applications, ~/.local,
// Snap and Flatpak exports), plus executables in user-configured folders.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "linux/results.hpp"

namespace beam {

struct AppEntry {
    std::wstring key;       // usage key: "app:" + desktop id (or "app:" + path for programs)
    std::wstring name;      // localized display name ("文字編輯器")
    std::wstring subtitle;  // generic name ("網頁瀏覽器") or the folder of a program
    std::vector<std::wstring> aliases;  // untranslated name, keywords, executable name
    std::string desktopId;  // empty for programs found in extraFolders
    std::string desktopFile;
    std::wstring program;   // extraFolders: absolute path of the executable
    std::wstring commandLine;  // Exec line, for copying
    GRef<GIcon> icon;
    std::vector<std::pair<std::string, std::wstring>> actions;  // desktop actions: id, localized name
};

std::vector<AppEntry> ScanApplications(const std::vector<std::wstring>& extraFolders);

}  // namespace beam
