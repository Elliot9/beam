// Application controller on Linux: a single-instance GtkApplication that owns the
// settings, the app index, the plugins, the GNOME shortcuts and the launcher window.
//
// Global hotkeys are GNOME custom shortcuts running `beam --toggle`; a second `beam`
// process only forwards its command line (and the compositor's activation token, so
// the launcher may take focus) to the running instance and exits.
#pragma once

#include <gtk/gtk.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/favorites.hpp"
#include "core/settings.hpp"
#include "core/usage.hpp"
#include "linux/apps.hpp"
#include "linux/results.hpp"
#include "linux/windows.hpp"
#include "plugin/plugin_manager.hpp"

namespace beam {

class LauncherWindow;
class SearchService;

class App {
public:
    App();
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    int Run(int argc, char** argv);

    const Settings& settings() const { return settings_; }
    UsageStore& usage() { return *usage_; }
    FavoritesStore& favorites() { return favorites_; }
    PluginManager& plugins() { return plugins_; }
    WindowService& windowService() { return windows_; }
    const std::vector<AppEntry>& apps() const { return apps_; }
    bool indexing() const { return indexing_; }
    const SearchService& search() const { return *search_; }
    const std::wstring& configDirectory() const { return configDir_; }
    std::wstring settingsPath() const { return configDir_ + L"/settings.json"; }
    bool darkTheme() const { return dark_; }

    void ShowLauncher(Mode mode);
    void ToggleLauncher(Mode mode);
    void HideLauncher();
    void OpenSettingsFile();
    void Reindex();
    void ReloadPlugins();  // explicit reload; the caller reports the outcome
    void Quit();
    void Notify(const std::wstring& title, const std::wstring& text, bool warning = false);
    void RecordUsage(const std::wstring& key, const std::wstring& query);
    void Defer(std::function<void()> fn);  // runs after the current event is handled

private:
    void Startup();
    int CommandLine(GApplicationCommandLine* commandLine);
    void ApplySettings(const Settings& settings);
    void RegisterHotkeys(bool announce, const std::wstring& extraNote);
    // Looks for a refusal of Beam's shortcut that gsd-media-keys (`gsd`) logged after the `base`
    // refusals counted before asking it to grab; asks again (kGrabAttempts in all) if so.
    void VerifyHotkeyGrab(unsigned generation, long gsd, const std::wstring& windowsHotkey, int base, int attempt);
    void CheckSettingsFile();
    void CheckPlugins();  // picks up edited / added / removed plugin.json files
    std::wstring PluginProblemsText() const;
    int64_t SettingsFileTime() const;
    void OnIndexDone(std::vector<AppEntry> apps);
    void ScheduleReindex();
    void WatchTheme();

    GtkApplication* gtkApp_ = nullptr;
    bool started_ = false;
    bool firstRun_ = false;
    std::wstring configDir_;
    Settings settings_;
    std::unique_ptr<UsageStore> usage_;
    FavoritesStore favorites_;
    PluginManager plugins_;
    std::wstring pluginProblems_;
    WindowService windows_;
    std::vector<AppEntry> apps_;
    std::atomic<bool> indexing_{false};
    std::thread indexThread_;
    bool reindexQueued_ = false;
    int64_t lastIndexMs_ = 0;
    guint reindexTimer_ = 0;
    guint watchTimer_ = 0;
    unsigned hotkeyGeneration_ = 0;  // RegisterHotkeys calls; a newer one cancels pending steps
    int64_t settingsTime_ = 0;
    bool dark_ = true;
    GSettings* interfaceSettings_ = nullptr;
    std::shared_ptr<App*> alive_;  // lets callbacks from worker threads find out whether we still exist
    std::unique_ptr<SearchService> search_;
    std::unique_ptr<LauncherWindow> launcher_;
};

}  // namespace beam
