// Settings window: hotkeys (recorded by pressing them), startup, theme,
// search engine. Everything else lives in settings.json.
#pragma once

#include "win/common.hpp"

#include <optional>
#include <string>

#include "core/hotkey.hpp"

namespace beam {

class App;

// "BeamHotkeyBox" control: focus it and press a key combination.
// Notifies the parent with WM_COMMAND, HIWORD = one of these codes.
constexpr WORD kHotkeyBoxChanged = 1;
constexpr WORD kHotkeyBoxRecordingStarted = 2;
constexpr WORD kHotkeyBoxRecordingStopped = 3;

void RegisterHotkeyBoxClass(HINSTANCE instance);
void SetHotkeyBoxValue(HWND box, std::optional<Hotkey> value, bool allowEmpty);
std::optional<Hotkey> GetHotkeyBoxValue(HWND box);

class SettingsDialog {
public:
    SettingsDialog(App& app, bool welcome) : app_(app), welcome_(welcome) {}
    ~SettingsDialog();
    bool Create();
    HWND hwnd() const { return hwnd_; }
    void Activate();

private:
    static INT_PTR CALLBACK DialogProc(HWND, UINT, WPARAM, LPARAM);
    INT_PTR Handle(UINT msg, WPARAM wp, LPARAM lp);
    void Initialize();
    void Save();
    void SetStatus(const std::wstring& text, bool error);

    App& app_;
    bool welcome_;
    HWND hwnd_ = nullptr;
    bool statusIsError_ = false;
    HICON bigIcon_ = nullptr;
    HICON smallIcon_ = nullptr;
};

}  // namespace beam
