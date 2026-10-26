// Beam - a Raycast-style launcher for Windows 10/11.
//
//   Beam.exe               start (or bring up the already running instance)
//   Beam.exe --background  start hidden in the notification area (used for "launch at startup")
#include "win/common.hpp"

#include <cwchar>

#include "win/app.hpp"

namespace {

void EnablePerMonitorDpiAwareness() {
    // The manifest already asks for PerMonitorV2; this covers builds without it.
    // Looked up dynamically: SetProcessDpiAwarenessContext needs Windows 10 1703+.
    using Fn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        if (auto fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"))))
            fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
    EnablePerMonitorDpiAwareness();
    const bool background = commandLine && std::wcsstr(commandLine, L"--background") != nullptr;

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\Beam.Launcher.SingleInstance.5f1c");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!background) {
            if (HWND existing = FindWindowW(beam::kHostWindowClass, nullptr)) {
                DWORD pid = 0;
                GetWindowThreadProcessId(existing, &pid);
                AllowSetForegroundWindow(pid);  // let the running instance take focus
                PostMessageW(existing, beam::WM_APP_SHOW, 0, 0);
            }
        }
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int code = 0;
    {
        beam::App app;
        code = app.Run(instance, background);
    }
    CoUninitialize();
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return code;
}
