// Notification-area icon with a context menu and balloon notifications.
#pragma once

#include "win/common.hpp"

#include <string>

namespace beam {

class App;

class TrayIcon {
public:
    TrayIcon(App& app, HWND host, HINSTANCE instance);
    ~TrayIcon();

    void Add();  // also after Explorer restarts (TaskbarCreated)
    void Remove();
    void OnCallback(WPARAM wp, LPARAM lp);
    void Balloon(const std::wstring& title, const std::wstring& text, bool warning);

private:
    NOTIFYICONDATAW Base() const;
    void ShowMenu();

    App& app_;
    HWND host_;
    HICON icon_ = nullptr;
    bool added_ = false;
};

}  // namespace beam
