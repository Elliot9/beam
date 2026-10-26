// Linux desktop operations: paths, launching (with Wayland activation tokens so
// new windows get focus), clipboard, terminals, notifications, session commands.
#pragma once

#include <string>
#include <vector>

namespace beam {

std::wstring HomeDir();
std::wstring ExpandUserPath(const std::wstring& path);  // "~", "~/x", "$VAR/x", "${VAR}/x"
bool FileExists(const std::wstring& path);
bool DirectoryExists(const std::wstring& path);
bool IsExecutableFile(const std::wstring& path);
std::wstring FileNameOf(const std::wstring& path);
std::wstring ParentOf(const std::wstring& path);
std::wstring ExtensionOf(const std::wstring& path);  // ".png" (lower case)
std::wstring FindProgram(const std::wstring& name);   // PATH lookup; empty when missing
// Network / FUSE mounts (NFS, SMB, sshfs, gvfs...): never touched on the UI thread.
bool IsRemotePath(const std::wstring& path);
std::string ExecutablePath();  // this program (/proc/self/exe)

// Starts argv detached in `directory`, handing over an activation token (focus on Wayland).
bool SpawnWithActivation(const std::vector<std::string>& argv, const std::wstring& directory, std::wstring* error);
bool OpenTarget(const std::wstring& target, std::wstring* error);  // URL, file or folder in its default app
// The default text editor (JSON files would often open in a browser).
bool OpenInTextEditor(const std::wstring& path, std::wstring* error);
// `argumentsTemplate` is split like a shell would (quotes), then {query} is put into the words,
// so the typed text never changes how the arguments are split or expanded.
bool RunProgram(const std::wstring& program, const std::wstring& argumentsTemplate, const std::wstring& query,
                std::wstring* error);
bool LaunchApp(const std::string& desktopId, const std::string& action, std::wstring* error);
void RevealInFileManager(const std::wstring& path);
bool CopyText(const std::wstring& text);
// `command` runs through bash in the default terminal, which stays open afterwards.
bool RunInTerminal(const std::wstring& command, bool asRoot, const std::wstring& directory, std::wstring* error);
bool RunInBackground(const std::wstring& command, std::wstring* error);  // bash -c, no window
bool OpenTerminalAt(const std::wstring& directory, std::wstring* error);
void Notify(const std::wstring& title, const std::wstring& body, bool warning);

bool PrefersDarkTheme();
bool SetAutostart(bool enabled);

void LockScreen();
void Suspend();
void PowerOff();
void Reboot();
void LogOut();
void EmptyTrash();

}  // namespace beam
