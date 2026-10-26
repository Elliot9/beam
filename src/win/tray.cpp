#include "win/tray.hpp"

#include <windowsx.h>

#include "core/hotkey.hpp"
#include "resource.h"
#include "win/app.hpp"

namespace beam {
namespace {

enum MenuCommand : UINT {
    kCmdOpen = 1,
    kCmdWindows,
    kCmdSettings,
    kCmdEditJson,
    kCmdReindex,
    kCmdStartup,
    kCmdQuit,
};

template <size_t N>
void CopyTruncated(wchar_t (&dst)[N], const std::wstring& src) {
    const size_t n = std::min(src.size(), N - 1);
    src.copy(dst, n);
    dst[n] = L'\0';
}

}  // namespace

TrayIcon::TrayIcon(App& app, HWND host, HINSTANCE instance) : app_(app), host_(host) {
    icon_ = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                          GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
}

TrayIcon::~TrayIcon() {
    Remove();
    if (icon_) DestroyIcon(icon_);
}

NOTIFYICONDATAW TrayIcon::Base() const {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = host_;
    nid.uID = 1;
    return nid;
}

void TrayIcon::Add() {
    NOTIFYICONDATAW nid = Base();
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = icon_;
    CopyTruncated(nid.szTip, std::wstring(kAppName) + L" - " + app_.settings().hotkey);
    if (added_ && Shell_NotifyIconW(NIM_MODIFY, &nid)) return;  // just refresh the tooltip
    added_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;          // first time, or Explorer restarted
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

void TrayIcon::Remove() {
    if (!added_) return;
    NOTIFYICONDATAW nid = Base();
    Shell_NotifyIconW(NIM_DELETE, &nid);
    added_ = false;
}

void TrayIcon::Balloon(const std::wstring& title, const std::wstring& text, bool warning) {
    if (!added_) return;
    NOTIFYICONDATAW nid = Base();
    nid.uFlags = NIF_INFO;
    CopyTruncated(nid.szInfoTitle, title);
    CopyTruncated(nid.szInfo, text);
    nid.dwInfoFlags = warning ? NIIF_WARNING : NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayIcon::OnCallback(WPARAM, LPARAM lp) {
    switch (LOWORD(lp)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            app_.ShowLauncher(Mode::Root);
            break;
        case WM_CONTEXTMENU:
            ShowMenu();
            break;
        default:
            break;
    }
}

void TrayIcon::ShowMenu() {
    HMENU menu = CreatePopupMenu();
    const Settings& s = app_.settings();
    const std::wstring open = L"開啟 Beam\t" + s.hotkey;
    AppendMenuW(menu, MF_STRING, kCmdOpen, open.c_str());
    const std::wstring windows =
        s.windowSwitcherHotkey.empty() ? std::wstring(L"切換視窗") : L"切換視窗\t" + s.windowSwitcherHotkey;
    AppendMenuW(menu, MF_STRING, kCmdWindows, windows.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdSettings, L"設定…");
    AppendMenuW(menu, MF_STRING, kCmdEditJson, L"編輯 settings.json");
    AppendMenuW(menu, MF_STRING, kCmdReindex, L"重新建立應用程式索引");
    AppendMenuW(menu, MF_STRING | (s.launchAtStartup ? MF_CHECKED : MF_UNCHECKED), kCmdStartup, L"開機時自動啟動");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdQuit, L"結束 Beam");
    SetMenuDefaultItem(menu, kCmdOpen, FALSE);

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(host_);  // otherwise the menu does not close when clicking elsewhere
    const UINT flags = TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON |
                       (GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN);
    const UINT cmd = static_cast<UINT>(TrackPopupMenuEx(menu, flags, pt.x, pt.y, host_, nullptr));
    PostMessageW(host_, WM_NULL, 0, 0);
    DestroyMenu(menu);

    switch (cmd) {
        case kCmdOpen:
            app_.ShowLauncher(Mode::Root);
            break;
        case kCmdWindows:
            app_.ShowLauncher(Mode::Windows);
            break;
        case kCmdSettings:
            app_.ShowSettings();
            break;
        case kCmdEditJson:
            app_.OpenSettingsFile();
            break;
        case kCmdReindex:
            app_.Reindex();
            break;
        case kCmdStartup: {
            Settings changed = s;
            changed.launchAtStartup = !s.launchAtStartup;
            app_.ApplySettings(changed, true);
            break;
        }
        case kCmdQuit:
            app_.Quit();
            break;
        default:
            break;
    }
}

}  // namespace beam
