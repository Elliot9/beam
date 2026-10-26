// Top-level window enumeration (Alt+Tab style) and activation.
#pragma once

#include "win/common.hpp"

#include <string>
#include <vector>

namespace beam {

struct WindowInfo {
    HWND hwnd = nullptr;
    std::wstring title;
    std::wstring processName;  // "chrome" (no extension)
    std::wstring exePath;      // used for the icon; real app for UWP frames
};

// Z-order, most recently active first. Excludes Beam's own windows.
std::vector<WindowInfo> EnumerateWindows();
bool ActivateWindow(HWND hwnd);
void CloseWindowGracefully(HWND hwnd);

}  // namespace beam
