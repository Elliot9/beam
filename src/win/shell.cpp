#include "win/shell.hpp"

#include <powrprof.h>

#include <cstring>
#include <vector>

#include "core/text.hpp"
#include "win/log.hpp"

namespace beam {

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw)) && raw) path = raw;
    if (raw) CoTaskMemFree(raw);
    return path;
}

std::wstring ConfigDirectory() {
    std::wstring dir = KnownFolder(FOLDERID_RoamingAppData);
    if (dir.empty()) dir = L".";
    dir += L"\\Beam";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring ExecutablePath() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (n == 0) return {};
        if (n < buffer.size()) return std::wstring(buffer.data(), n);
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring ExpandUserPath(const std::wstring& path) {
    std::wstring p = path;
    if (!p.empty() && p[0] == L'~' && (p.size() == 1 || p[1] == L'\\' || p[1] == L'/'))
        p = KnownFolder(FOLDERID_Profile) + p.substr(1);
    if (p.find(L'%') != std::wstring::npos) {
        const DWORD n = ExpandEnvironmentStringsW(p.c_str(), nullptr, 0);
        if (n > 0) {
            std::wstring out(n, L'\0');
            if (ExpandEnvironmentStringsW(p.c_str(), out.data(), n) > 0) {
                out.resize(n - 1);
                p = out;
            }
        }
    }
    for (auto& c : p)
        if (c == L'/') c = L'\\';
    return p;
}

bool IsNetworkPath(const std::wstring& path) {
    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') return true;
    if (path.size() >= 2 && path[1] == L':') {
        const wchar_t root[] = {path[0], L':', L'\\', L'\0'};
        return GetDriveTypeW(root) == DRIVE_REMOTE;
    }
    return false;
}

bool FileExists(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirectoryExists(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring FileNameOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring StemOf(const std::wstring& path) {
    std::wstring name = FileNameOf(path);
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) name.resize(dot);
    return name;
}

std::wstring ParentOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    if (slash == 2 && path.size() > 1 && path[1] == L':') return path.substr(0, 3);  // keep "C:\"
    return path.substr(0, slash);
}

std::wstring ExtensionOf(const std::wstring& path) {
    const std::wstring name = FileNameOf(path);
    const size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos ? std::wstring() : Fold(name.substr(dot));
}

bool ShellOpen(const std::wstring& target, const LaunchOptions& options, std::wstring* error) {
    AllowSetForegroundWindow(ASFW_ANY);
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    // NO_UI: the shell's own error box would open behind our topmost window; we report errors ourselves.
    sei.fMask = SEE_MASK_FLAG_LOG_USAGE | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = options.admin ? L"runas" : nullptr;
    sei.lpFile = target.c_str();
    sei.lpParameters = options.arguments.empty() ? nullptr : options.arguments.c_str();
    sei.lpDirectory = options.directory.empty() ? nullptr : options.directory.c_str();
    sei.nShow = options.show;
    if (ShellExecuteExW(&sei)) return true;
    DWORD e = GetLastError();
    if (e == ERROR_NO_ASSOCIATION && !options.admin) {
        sei.fMask = SEE_MASK_FLAG_LOG_USAGE;
        sei.lpVerb = L"openas";  // let the user pick an app, like Explorer does
        if (ShellExecuteExW(&sei)) return true;
        e = GetLastError();
    }
    if (error) error->clear();
    if (e == ERROR_CANCELLED) return false;  // UAC prompt declined
    if (error) *error = log::ErrorText(e);
    log::Error(L"ShellExecuteEx(" + target + L") failed: " + log::ErrorText(e));
    return false;
}

void RevealInExplorer(const std::wstring& path) {
    AllowSetForegroundWindow(ASFW_ANY);
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr)) && pidl) {
        const HRESULT hr = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        CoTaskMemFree(pidl);
        if (SUCCEEDED(hr)) return;
    }
    ShellOpen(L"explorer.exe", {L"/select,\"" + path + L"\""});
}

bool CopyToClipboard(HWND owner, const std::wstring& text) {
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (!OpenClipboard(owner)) {
            Sleep(20);
            continue;
        }
        EmptyClipboard();
        bool ok = false;
        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
            if (void* p = GlobalLock(mem)) {
                std::memcpy(p, text.c_str(), bytes);
                GlobalUnlock(mem);
                ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
            }
            if (!ok) GlobalFree(mem);
        }
        CloseClipboard();
        return ok;
    }
    return false;
}

std::wstring ReadClipboardText(HWND owner) {
    std::wstring text;
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (!OpenClipboard(owner)) {
            Sleep(20);
            continue;
        }
        if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
            if (const auto* p = static_cast<const wchar_t*>(GlobalLock(data))) {
                text = p;
                GlobalUnlock(data);
            }
        }
        CloseClipboard();
        break;
    }
    return text;
}

namespace {

std::wstring Base64(const unsigned char* data, size_t size) {
    static const char* kTable = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::wstring out;
    for (size_t i = 0; i < size; i += 3) {
        const uint32_t b0 = data[i];
        const uint32_t b1 = i + 1 < size ? data[i + 1] : 0;
        const uint32_t b2 = i + 2 < size ? data[i + 2] : 0;
        const uint32_t v = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(static_cast<wchar_t>(kTable[(v >> 18) & 63]));
        out.push_back(static_cast<wchar_t>(kTable[(v >> 12) & 63]));
        out.push_back(i + 1 < size ? static_cast<wchar_t>(kTable[(v >> 6) & 63]) : L'=');
        out.push_back(i + 2 < size ? static_cast<wchar_t>(kTable[v & 63]) : L'=');
    }
    return out;
}

}  // namespace

void RunCommandLine(const std::wstring& command, bool powershell, bool admin) {
    const std::wstring home = KnownFolder(FOLDERID_Profile);
    if (powershell) {
        // -EncodedCommand takes base64 of UTF-16LE, which sidesteps all quoting rules.
        const std::wstring encoded =
            Base64(reinterpret_cast<const unsigned char*>(command.data()), command.size() * sizeof(wchar_t));
        ShellOpen(L"powershell.exe", {L"-NoExit -EncodedCommand " + encoded, home, admin});
    } else {
        ShellOpen(L"cmd.exe", {L"/k " + command, home, admin});
    }
}

void OpenTerminalAt(const std::wstring& directory) {
    std::wstring dir = directory;
    if (!dir.empty() && dir.back() == L'\\') dir += L'.';  // a trailing \" would escape the quote
    const std::wstring wt = KnownFolder(FOLDERID_LocalAppData) + L"\\Microsoft\\WindowsApps\\wt.exe";
    if (FileExists(wt)) {
        ShellOpen(wt, {L"-d \"" + dir + L"\""});
    } else {
        ShellOpen(L"cmd.exe", {L"", directory});
    }
}

bool SetLaunchAtStartup(bool enabled) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0,
                        KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    LSTATUS status = ERROR_SUCCESS;
    if (enabled) {
        const std::wstring value = L"\"" + ExecutablePath() + L"\" --background";
        status = RegSetValueExW(key, kAppName, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                                static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    } else {
        status = RegDeleteValueW(key, kAppName);
        if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

bool SystemUsesLightTheme() {
    DWORD value = 1;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) {
        return true;  // Windows default
    }
    return value != 0;
}

void LockComputer() { LockWorkStation(); }

namespace {
bool EnableShutdownPrivilege() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return false;
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    const bool ok = LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid) &&
                    AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr) &&
                    GetLastError() == ERROR_SUCCESS;
    CloseHandle(token);
    return ok;
}
}  // namespace

void SleepComputer() {
    EnableShutdownPrivilege();  // SetSuspendState requires SE_SHUTDOWN_NAME to be enabled
    if (!SetSuspendState(FALSE, FALSE, FALSE)) log::Error(L"SetSuspendState: " + log::ErrorText(GetLastError()));
}

void ShutdownComputer(bool restart) {
    ShellOpen(L"shutdown.exe", {restart ? L"/r /t 0" : L"/s /t 0", L"", false, SW_HIDE});
}

void SignOut() { ExitWindowsEx(EWX_LOGOFF, SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED); }

void EmptyRecycleBin() { SHEmptyRecycleBinW(nullptr, nullptr, SHERB_NOCONFIRMATION); }

}  // namespace beam
