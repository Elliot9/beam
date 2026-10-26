#include "win/winlist.hpp"

#include <dwmapi.h>

#include <unordered_map>

#include "core/text.hpp"
#include "win/shell.hpp"

namespace beam {
namespace {

std::wstring WindowText(HWND hwnd) {
    const int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return {};
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    const int got = GetWindowTextW(hwnd, text.data(), len + 1);
    text.resize(got > 0 ? static_cast<size_t>(got) : 0);
    return text;
}

std::wstring ClassName(HWND hwnd) {
    wchar_t buf[256] = {};
    const int n = GetClassNameW(hwnd, buf, ARRAYSIZE(buf));
    return std::wstring(buf, n > 0 ? static_cast<size_t>(n) : 0);
}

bool IsCloaked(HWND hwnd) {
    int cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0;
}

std::wstring ProcessPath(DWORD pid) {
    UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process) return {};
    wchar_t buf[MAX_PATH * 2];
    DWORD size = ARRAYSIZE(buf);
    if (!QueryFullProcessImageNameW(process.get(), 0, buf, &size)) return {};
    return std::wstring(buf, size);
}

// UWP apps live in an ApplicationFrameHost frame; the real app owns a child CoreWindow.
struct ChildSearch {
    DWORD framePid;
    DWORD found;
};

BOOL CALLBACK OnChildWindow(HWND child, LPARAM lp) {
    auto* s = reinterpret_cast<ChildSearch*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(child, &pid);
    if (pid != s->framePid) {
        s->found = pid;
        return FALSE;
    }
    return TRUE;
}

DWORD HostedAppProcess(HWND frame, DWORD framePid) {
    ChildSearch search{framePid, 0};
    EnumChildWindows(frame, OnChildWindow, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

struct EnumState {
    DWORD ownPid;
    std::vector<WindowInfo>* out;
    std::unordered_map<DWORD, std::wstring> paths;
};

BOOL CALLBACK OnWindow(HWND hwnd, LPARAM lp) {
    auto* st = reinterpret_cast<EnumState*>(lp);
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((ex & WS_EX_APPWINDOW) == 0 && (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) != 0) return TRUE;
    if (IsCloaked(hwnd)) return TRUE;  // other virtual desktops, suspended UWP frames
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == st->ownPid) return TRUE;
    std::wstring title = WindowText(hwnd);
    if (Trim(title).empty()) return TRUE;
    const std::wstring cls = ClassName(hwnd);
    if (cls == L"Progman" || cls == L"WorkerW" || cls == L"Shell_TrayWnd" || cls == L"Shell_SecondaryTrayWnd")
        return TRUE;
    if (cls == L"ApplicationFrameWindow") {
        if (const DWORD hosted = HostedAppProcess(hwnd, pid)) pid = hosted;
    }
    auto it = st->paths.find(pid);
    if (it == st->paths.end()) it = st->paths.emplace(pid, ProcessPath(pid)).first;
    WindowInfo info;
    info.hwnd = hwnd;
    info.title = std::move(title);
    info.exePath = it->second;
    info.processName = StemOf(info.exePath);
    st->out->push_back(std::move(info));
    return TRUE;
}

}  // namespace

std::vector<WindowInfo> EnumerateWindows() {
    std::vector<WindowInfo> windows;
    EnumState state{GetCurrentProcessId(), &windows, {}};
    EnumWindows(OnWindow, reinterpret_cast<LPARAM>(&state));
    return windows;
}

bool ActivateWindow(HWND hwnd) {
    if (!IsWindow(hwnd)) return false;
    if (IsIconic(hwnd)) ShowWindowAsync(hwnd, SW_RESTORE);
    if (SetForegroundWindow(hwnd)) return true;
    // We are normally the foreground process here; if Windows still refuses,
    // borrow the foreground thread's input state for a moment.
    const HWND fg = GetForegroundWindow();
    const DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const DWORD self = GetCurrentThreadId();
    const bool attached = fgThread && fgThread != self && AttachThreadInput(self, fgThread, TRUE);
    BringWindowToTop(hwnd);
    const bool ok = SetForegroundWindow(hwnd) != FALSE;
    if (attached) AttachThreadInput(self, fgThread, FALSE);
    return ok;
}

void CloseWindowGracefully(HWND hwnd) {
    if (IsWindow(hwnd)) PostMessageW(hwnd, WM_CLOSE, 0, 0);
}

}  // namespace beam
