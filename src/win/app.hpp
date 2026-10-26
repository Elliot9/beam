// Application controller: owns settings, the index, hotkeys, the tray icon
// and the launcher window, and runs the message loop.
#pragma once

#include "win/common.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/favorites.hpp"
#include "core/settings.hpp"
#include "core/usage.hpp"
#include "win/apps.hpp"
#include "win/icons.hpp"
#include "plugin/plugin_manager.hpp"
#include "win/renderer.hpp"
#include "win/results.hpp"

namespace beam {

class LauncherWindow;
class SearchService;
class SettingsDialog;
class TrayIcon;

class App {
public:
    App();
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    int Run(HINSTANCE instance, bool startHidden);

    HINSTANCE instance() const { return instance_; }
    HWND hostWindow() const { return host_; }
    const Settings& settings() const { return settings_; }
    const Palette& palette() const { return palette_; }
    UsageStore& usage() { return *usage_; }
    FavoritesStore& favorites() { return favorites_; }
    PluginManager& plugins() { return plugins_; }
    IconCache& icons() { return icons_; }
    const std::vector<AppEntry>& apps() const { return apps_; }
    bool indexing() const { return indexing_; }
    const SearchService& search() const { return *search_; }
    const std::wstring& configDirectory() const { return configDir_; }
    std::wstring settingsPath() const { return configDir_ + L"\\settings.json"; }

    void ShowLauncher(Mode mode);
    void ToggleLauncher(Mode mode);
    void HideLauncher();
    void ShowSettings(bool welcome = false);
    void OpenSettingsFile();
    void Reindex();
    void ReloadPlugins();  // explicit reload; the caller reports the outcome
    void Quit();
    void Notify(const std::wstring& title, const std::wstring& text, bool warning = false);
    void RecordUsage(const std::wstring& key, const std::wstring& query);
    void Defer(std::function<void()> fn);  // runs after the current message is handled

    // Validates hotkeys by registering them; returns an error message (settings unchanged) or "".
    std::wstring ApplySettings(const Settings& settings, bool persist);
    void SetHotkeysSuspended(bool suspended);
    void OnSettingsDialogClosed();

private:
    static LRESULT CALLBACK HostProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleHostMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    bool CreateHostWindow();
    std::wstring RegisterHotkeys(const Settings& settings);
    void UnregisterHotkeys();
    void CheckSettingsFile();
    void CheckPlugins();  // picks up edited / added / removed plugin.json files
    std::wstring PluginProblemsText() const;
    FILETIME SettingsFileTime() const;
    void RefreshPalette();

    HINSTANCE instance_ = nullptr;
    HWND host_ = nullptr;
    std::wstring configDir_;
    Settings settings_;
    Palette palette_ = MakePalette(true);
    std::unique_ptr<UsageStore> usage_;
    FavoritesStore favorites_;
    PluginManager plugins_;
    std::wstring pluginProblems_;
    IconCache icons_;
    std::vector<AppEntry> apps_;
    std::atomic<bool> indexing_{false};
    std::atomic<bool> cancelIndex_{false};
    std::thread indexThread_;
    bool reindexQueued_ = false;
    ULONGLONG lastIndexTick_ = 0;
    std::unique_ptr<SearchService> search_;
    std::unique_ptr<LauncherWindow> launcher_;
    std::unique_ptr<TrayIcon> tray_;
    std::unique_ptr<SettingsDialog> settingsDialog_;
    bool hotkeysRegistered_ = false;
    bool hotkeysSuspended_ = false;
    FILETIME settingsTime_{};
    UINT taskbarCreatedMessage_ = 0;
    std::vector<std::function<void()>> deferred_;
};

constexpr int kHotkeyLauncher = 1;
constexpr int kHotkeyWindows = 2;

}  // namespace beam
