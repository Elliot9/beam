// Thin wrappers over the Windows shell: launching, clipboard, Explorer,
// startup registration and system power commands.
#pragma once

#include "win/common.hpp"

#include <string>

namespace beam {

std::wstring KnownFolder(REFKNOWNFOLDERID id);
std::wstring ConfigDirectory();  // %APPDATA%\Beam, created on demand
std::wstring ExecutablePath();
std::wstring ExpandUserPath(const std::wstring& path);  // "~", "%VAR%"
bool FileExists(const std::wstring& path);
// UNC paths and mapped network drives: never touch them on the UI thread (they can stall for 20 s+).
bool IsNetworkPath(const std::wstring& path);
bool DirectoryExists(const std::wstring& path);
std::wstring FileNameOf(const std::wstring& path);      // "C:\a\b.exe" -> "b.exe"
std::wstring StemOf(const std::wstring& path);          // "C:\a\b.exe" -> "b"
std::wstring ParentOf(const std::wstring& path);
std::wstring ExtensionOf(const std::wstring& path);     // ".exe" (lower case)

struct LaunchOptions {
    std::wstring arguments;
    std::wstring directory;
    bool admin = false;
    int show = SW_SHOWNORMAL;
};
// ShellExecuteEx; returns false on failure (error filled) or when UAC was declined (error empty).
bool ShellOpen(const std::wstring& target, const LaunchOptions& options = {}, std::wstring* error = nullptr);
void RevealInExplorer(const std::wstring& path);
bool CopyToClipboard(HWND owner, const std::wstring& text);
std::wstring ReadClipboardText(HWND owner);

void RunCommandLine(const std::wstring& command, bool powershell, bool admin);
void OpenTerminalAt(const std::wstring& directory);

bool SetLaunchAtStartup(bool enabled);
bool SystemUsesLightTheme();

void LockComputer();
void SleepComputer();
void ShutdownComputer(bool restart);
void SignOut();
void EmptyRecycleBin();

}  // namespace beam
