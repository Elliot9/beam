// Application index: Start Menu shortcuts, packaged (Store/UWP) apps from
// shell:AppsFolder, desktop shortcuts and user-configured tool folders.
#pragma once

#include <atomic>
#include <string>
#include <vector>

namespace beam {

enum class AppKind { Shortcut, Packaged, Program, Script, Protocol };

struct AppEntry {
    std::wstring key;        // stable usage key ("app:" + launch path, lower case)
    std::wstring name;       // localized display name ("記事本")
    std::wstring launch;     // what ShellExecute opens (.lnk, shell:AppsFolder\AUMID, .exe, ...)
    std::wstring arguments;  // extra arguments (scripts)
    std::wstring target;     // resolved file on disk, if known ("C:\...\notepad.exe")
    std::wstring iconPath;   // shell parsing name used to extract the icon
    std::wstring subtitle;   // short hint shown next to the name
    std::vector<std::wstring> aliases;  // extra searchable names ("Notepad", "notepad")
    AppKind kind = AppKind::Shortcut;
};

// Must run on a thread that initialized COM (STA). Takes ~0.2-1 s.
// Returns early (partial result) once *cancel becomes true.
std::vector<AppEntry> ScanApplications(const std::vector<std::wstring>& extraFolders,
                                       const std::atomic<bool>* cancel = nullptr);

}  // namespace beam
