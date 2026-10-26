#include "linux/app.hpp"

#include <glib-unix.h>
#include <sys/stat.h>

#include <csignal>
#include <ctime>
#include <filesystem>

#include "core/text.hpp"
#include "linux/hotkey.hpp"
#include "linux/launcher.hpp"
#include "linux/search.hpp"
#include "linux/system.hpp"

#ifndef BEAM_VERSION
#define BEAM_VERSION "dev"
#endif

namespace beam {
namespace {

constexpr char kAppId[] = "local.beam.Launcher";
constexpr int64_t kIndexMaxAgeMs = 10 * 60 * 1000;

int64_t NowMs() { return g_get_monotonic_time() / 1000; }

void Log(const std::wstring& text) { g_printerr("beam: %s\n", WideToUtf8(text).c_str()); }

std::filesystem::path Fs(const std::wstring& path) { return std::filesystem::path(WideToUtf8(path)); }

// Hotkey registration timing (see App::RegisterHotkeys).
constexpr guint kSettleMs = 1500;     // GNOME Shell applies a released shortcut when it is idle
constexpr guint kGrabCheckMs = 2000;  // gsd-media-keys grabs (or logs a refusal) well within this
constexpr int kGrabAttempts = 3;

void After(guint ms, std::function<void()> fn) {
    g_timeout_add_full(
        G_PRIORITY_DEFAULT, ms,
        [](gpointer data) -> gboolean {
            (*static_cast<std::function<void()>*>(data))();
            return G_SOURCE_REMOVE;
        },
        new std::function<void()>(std::move(fn)), [](gpointer data) { delete static_cast<std::function<void()>*>(data); });
}

const GOptionEntry kOptions[] = {
    {"toggle", 't', 0, G_OPTION_ARG_NONE, nullptr, "顯示或隱藏 Beam（預設，GNOME 快捷鍵執行的就是這個）", nullptr},
    {"show", 's', 0, G_OPTION_ARG_NONE, nullptr, "顯示 Beam", nullptr},
    {"windows", 'w', 0, G_OPTION_ARG_NONE, nullptr, "顯示或隱藏視窗切換清單", nullptr},
    {"background", 'b', 0, G_OPTION_ARG_NONE, nullptr, "在背景啟動，不顯示畫面（開機啟動用）", nullptr},
    {"quit", 'q', 0, G_OPTION_ARG_NONE, nullptr, "結束執行中的 Beam", nullptr},
    {"unregister", 0, 0, G_OPTION_ARG_NONE, nullptr,
     "移除 Beam 在 GNOME 的設定（快捷鍵、開機啟動、視窗切換擴充功能）並結束；解除安裝前使用", nullptr},
    {"doctor", 0, 0, G_OPTION_ARG_NONE, nullptr, "檢查快捷鍵為什麼沒有反應，並修正 Beam 自己的 GNOME 快捷鍵", nullptr},
    {"version", 'v', 0, G_OPTION_ARG_NONE, nullptr, "顯示版本", nullptr},
    {"dump-state", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, nullptr, "Print the launcher state (tests)", nullptr},
    {nullptr, 0, 0, G_OPTION_ARG_NONE, nullptr, nullptr, nullptr},
};

// beam --unregister: undo what Beam set up in the desktop (the settings folder stays).
const char* UnregisterDesktop() {
    UnregisterGnomeShortcuts();
    SetAutostart(false);
    WindowService::DisableExtension();
    return "已移除 Beam 的 GNOME 快捷鍵、開機啟動與視窗切換擴充功能（Alt+Space 還給 GNOME 的視窗選單）。\n"
           "設定與外掛仍在 ~/.config/beam。\n";
}

template <typename Fn>
GCallback Callback(Fn* fn) {  // G_CALLBACK for lambdas
    return reinterpret_cast<GCallback>(fn);
}

// Whether a D-Bus name has an owner (without starting a service for it).
bool NameHasOwner(const char* name) {
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!bus) return false;
    GVariant* reply = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                                  "org.freedesktop.DBus", "NameHasOwner", g_variant_new("(s)", name),
                                                  G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr);
    gboolean owned = FALSE;
    if (reply) {
        g_variant_get(reply, "(b)", &owned);
        g_variant_unref(reply);
    }
    g_object_unref(bus);
    return owned;
}

// Whether a Beam instance is running.
bool InstanceRunning() { return NameHasOwner(kAppId); }

// Windows Subsystem for Linux: no GNOME desktop there, Windows itself gets Alt+Space.
bool InsideWsl() {
    return g_getenv("WSL_DISTRO_NAME") || g_file_test("/proc/sys/fs/binfmt_misc/WSLInterop", G_FILE_TEST_EXISTS);
}

// Whether GNOME runs this session's custom shortcuts: GNOME's schemas alone are not enough (they
// come with GNOME apps on any desktop, even in WSL); gsd-media-keys must be there to run them.
bool GnomeRunsShortcuts() {
    return GnomeShortcutsSupported() && (IsGnomeSession() || NameHasOwner("org.gnome.SettingsDaemon.MediaKeys"));
}

// Why Beam cannot set up its hotkey here (no GNOME desktop in this session).
std::wstring NoGnomeHotkeyText() {
    if (InsideWsl())
        return L"這是 WSL：裡面沒有 GNOME 桌面，Alt+Space 是 Windows 在接收。請在 Windows 使用 Windows 版 Beam（Beam.exe）";
    const char* type = g_getenv("XDG_SESSION_TYPE");
    if (!g_getenv("WAYLAND_DISPLAY") && !g_getenv("DISPLAY") && (!type || !*type || g_str_equal(type, "tty")))
        return L"這裡是文字模式（SSH 或 tty），不是 Ubuntu 的圖形桌面：請在圖形桌面裡開啟「終端機」再執行";
    return L"這不是 GNOME 桌面：Beam 只會在 GNOME 自動設定快捷鍵。請在系統的鍵盤設定新增一組快捷鍵，指令填 beam --toggle";
}

// beam --doctor: checks what the hotkey depends on, repairs Beam's own GNOME shortcut (like every
// start of Beam does) and says what the user can do about the rest. Exit status 1 on a problem.
int Doctor() {
    int problems = 0;
    auto say = [](const char* mark, const std::wstring& text) {
        std::string body = WideToUtf8(text);
        for (size_t at = body.find('\n'); at != std::string::npos; at = body.find('\n', at + 3)) body.insert(at + 1, "  ");
        g_print("%s %s\n", mark, body.c_str());
    };
    auto good = [&](const std::wstring& text) { say("✓", text); };
    auto bad = [&](const std::wstring& text) {
        say("✗", text);
        ++problems;
    };
    auto info = [&](const std::wstring& text) { say("•", text); };
    auto finish = [&](const std::wstring& hotkey) {
        g_print("\n");
        if (problems == 0)
            g_print("看起來都正常：按 %s 應該就會出現 Beam。\n"
                    "還是沒反應的話，請把這份結果和 journalctl -b | grep -iE \"beam|grab accel\" 的輸出一起回報。\n",
                    WideToUtf8(hotkey).c_str());
        return problems ? 1 : 0;
    };

    gtk_init_check();  // the keyboard map (KeyOwnerProbe); fails harmlessly without a display
    g_print("Beam %s：檢查快捷鍵\n\n", BEAM_VERSION);
    const char* desktop = g_getenv("XDG_CURRENT_DESKTOP");
    const char* session = g_getenv("XDG_SESSION_TYPE");
    info(L"桌面：" + Utf8ToWide(desktop && *desktop ? desktop : "不明") + L"（" + Utf8ToWide(session && *session ? session : "?") + L"）");
    if (!GnomeRunsShortcuts()) {
        bad(NoGnomeHotkeyText());
        return finish(L"Alt+Space");
    }
    if (InstanceRunning()) good(L"Beam 正在執行");
    else info(L"Beam 現在沒有執行（按快捷鍵時會自動啟動）");

    const std::wstring settingsFile = PlatformConfigDirectory() + L"/settings.json";
    if (!FileExists(settingsFile)) {
        bad(L"還沒開過 Beam：快捷鍵是 Beam 第一次啟動時才設定的。請從應用程式清單開啟「Beam」（或執行 beam）");
        return finish(L"Alt+Space");
    }
    const SettingsLoadResult loaded = LoadSettings(Fs(settingsFile));
    if (!loaded.error.empty()) bad(L"settings.json 格式錯誤（Beam 改用預設值）：" + Utf8ToWide(loaded.error));
    const Settings& s = loaded.settings;
    std::wstring windowsHotkey = s.windowSwitcherHotkey;
    if (!Trim(windowsHotkey).empty() && GnomeAccelerator(windowsHotkey) == GnomeAccelerator(s.hotkey)) {
        bad(L"hotkey 和 windowSwitcherHotkey 不能是同一組按鍵");
        windowsHotkey.clear();
    }

    // Beam's custom shortcut: repaired the same way every start of Beam does it (App::RegisterHotkeys).
    const std::string exe = ExecutablePath();
    const std::string accel = GnomeAccelerator(s.hotkey);
    const std::string command = BeamShortcutCommand(exe, false);
    const std::wstring shown = s.hotkey + L" → " + Utf8ToWide(command);
    const long gsd = MediaKeysPid();
    const GnomeShortcut before = ReadBeamShortcut(false);
    const HotkeySetup released = ReleaseHotkeyConflicts(s.hotkey, windowsHotkey);
    if (released.changed) g_usleep(kSettleMs * 1000);
    std::string failedAt;
    const int failures = CountShortcutGrabFailures(gsd, &failedAt);
    const ShortcutWrite wrote = WriteGnomeShortcuts(s.hotkey, windowsHotkey, exe, failures > 0);
    const GnomeShortcut after = ReadBeamShortcut(false);
    if (accel.empty()) {
        bad(L"settings.json 的 hotkey「" + s.hotkey + L"」不是能用的快捷鍵");
    } else if (!wrote.changed && before.listed && before.binding == accel && before.command == command) {
        good(L"GNOME 自訂快捷鍵「Beam」：" + shown);
    } else if (after.listed && after.binding == accel && after.command == command) {
        good(L"已修正 GNOME 自訂快捷鍵「Beam」：" + shown +
             (before.listed ? L"（原本是 " + Utf8ToWide(before.binding) + L" → " + Utf8ToWide(before.command) + L"）"
                            : std::wstring(L"（原本沒有這組快捷鍵）")));
    } else {
        bad(L"無法寫入 GNOME 的自訂快捷鍵設定");
    }
    if (!released.note.empty()) good(released.note);
    if (!released.problem.empty()) bad(released.problem);
    else if (!accel.empty()) good(L"沒有其他快捷鍵用 " + s.hotkey);

    // gsd-media-keys runs custom shortcuts: it asks GNOME Shell to grab their keys, and if the
    // shell refuses, it only logs that and never asks again.
    if (gsd <= 0) {
        bad(L"GNOME 的快捷鍵服務（gsd-media-keys）沒有在執行，所有自訂快捷鍵都不會有反應：請登出再登入");
    } else {
        good(L"GNOME 的快捷鍵服務（gsd-media-keys）正在執行");
        if (wrote.asked && failures >= 0) {
            if (failures > 0) info(L"GNOME 在 " + Utf8ToWide(failedAt) + L" 拒絕過註冊 " + s.hotkey + L"，正在請它重新註冊…");
            int base = failures;
            int attempts = 0;
            bool accepted = false;
            while (!accepted && attempts < kGrabAttempts) {
                ++attempts;
                g_usleep(kGrabCheckMs * 1000);
                const int now = CountShortcutGrabFailures(gsd, nullptr);
                accepted = now <= base;
                base = std::max(base, now);
                if (!accepted && attempts < kGrabAttempts) WriteGnomeShortcuts(s.hotkey, windowsHotkey, exe, true);
            }
            if (!accepted) {
                bad(L"GNOME 拒絕註冊 " + s.hotkey + L"（試了 " + std::to_wstring(attempts) + L" 次），按鍵被別的東西占用了"
                    L"\n登出再登入一次；還是不行就在 settings.json 換一組快捷鍵");
                // Nothing in GNOME's settings explains it: only GNOME Shell knows who holds the keys.
                const std::string probe = released.problem.empty() ? KeyOwnerProbe(s.hotkey) : std::string();
                if (!probe.empty()) {
                    info(L"GNOME 的設定裡找不到是誰占用了 " + s.hotkey + L"，但可以直接問 GNOME：\n"
                         L"按 Alt+F2，輸入 lg 按 Enter（Looking Glass），在下方 >>> 輸入列貼上下面這一行並按 Enter，"
                         L"再把回答的那一行回報");
                    g_print("\n%s\n\n", probe.c_str());
                }
            } else if (failures > 0)
                good(L"GNOME 這次接受了 " + s.hotkey + L"：現在按 " + s.hotkey + L" 試試");
        }
    }

    WindowService windows;
    switch (windows.State()) {
        case ExtensionState::Running: good(L"視窗切換擴充功能正在執行"); break;
        case ExtensionState::NeedsRelogin: info(L"視窗切換擴充功能已啟用，登出再登入後才會載入（不影響快捷鍵）"); break;
        default: info(L"視窗切換擴充功能沒有啟用（不影響快捷鍵；在 Beam 輸入「設定視窗切換」可以開啟）"); break;
    }
    const std::string autostart = std::string(g_get_user_config_dir()) + "/autostart/" + kAppId + ".desktop";
    if (g_file_test(autostart.c_str(), G_FILE_TEST_EXISTS)) good(L"登入時自動啟動");
    else info(L"沒有設定登入時自動啟動（不影響快捷鍵）");
    return finish(s.hotkey);
}

}  // namespace

App::App() = default;

App::~App() {
    alive_.reset();
    if (indexThread_.joinable()) indexThread_.join();
}

int App::Run(int argc, char** argv) {
    gtkApp_ = gtk_application_new(kAppId, G_APPLICATION_HANDLES_COMMAND_LINE);
    g_application_add_main_option_entries(G_APPLICATION(gtkApp_), kOptions);
    g_application_set_option_context_summary(G_APPLICATION(gtkApp_),
                                             "Beam：搜尋應用程式、視窗、指令與外掛（預設快捷鍵 Alt+Space）。");
    g_signal_connect(gtkApp_, "handle-local-options", Callback(+[](GApplication*, GVariantDict* options, gpointer) -> gint {
                         if (g_variant_dict_contains(options, "version")) {
                             g_print("Beam %s\n", BEAM_VERSION);
                             return 0;
                         }
                         if (g_variant_dict_contains(options, "doctor")) return Doctor();
                         if (g_variant_dict_contains(options, "unregister") && !InstanceRunning()) {
                             g_print("%s", UnregisterDesktop());
                             return 0;
                         }
                         // Nothing to quit or inspect: do not start an instance just for that.
                         if ((g_variant_dict_contains(options, "quit") || g_variant_dict_contains(options, "dump-state")) &&
                             !InstanceRunning()) {
                             g_printerr("Beam is not running\n");
                             return g_variant_dict_contains(options, "quit") ? 0 : 1;
                         }
                         return -1;
                     }),
                     this);
    g_signal_connect(gtkApp_, "startup", Callback(+[](GApplication*, gpointer self) { static_cast<App*>(self)->Startup(); }),
                     this);
    g_signal_connect(gtkApp_, "command-line",
                     Callback(+[](GApplication*, GApplicationCommandLine* commandLine, gpointer self) -> int {
                         return static_cast<App*>(self)->CommandLine(commandLine);
                     }),
                     this);
    const int status = g_application_run(G_APPLICATION(gtkApp_), argc, argv);

    for (guint* id : {&watchTimer_, &reindexTimer_}) {
        if (*id) g_source_remove(*id);
        *id = 0;
    }
    alive_.reset();
    if (indexThread_.joinable()) indexThread_.join();
    plugins_.CancelAll();
    if (usage_) usage_->Save();
    launcher_.reset();
    search_.reset();
    if (interfaceSettings_) g_object_unref(interfaceSettings_);
    interfaceSettings_ = nullptr;
    g_object_unref(gtkApp_);
    gtkApp_ = nullptr;
    return status;
}

void App::Startup() {
    alive_ = std::make_shared<App*>(this);
    configDir_ = PlatformConfigDirectory();
    g_mkdir_with_parents(WideToUtf8(configDir_).c_str(), 0700);
    Log(L"starting " + Utf8ToWide(BEAM_VERSION) + L", config " + configDir_);

    const SettingsLoadResult loaded = LoadSettings(Fs(settingsPath()));
    settings_ = loaded.settings;
    firstRun_ = loaded.created;
    if (firstRun_) {
        // The GNOME shortcut starts Beam on demand anyway; autostart only makes the first press instant.
        settings_.launchAtStartup = true;
        SaveSettings(Fs(settingsPath()), settings_);
    }
    settingsTime_ = SettingsFileTime();
    usage_ = std::make_unique<UsageStore>(Fs(configDir_) / "usage.json");
    usage_->Load();
    favorites_ = FavoritesStore(Fs(configDir_) / "favorites.json");
    favorites_.Load();
    plugins_.Initialize(configDir_);
    plugins_.SetLog([](const std::wstring& text) { Log(text); });
    plugins_.Reload();
    plugins_.EnsureDocumentation();
    pluginProblems_ = PluginProblemsText();
    WatchTheme();

    search_ = std::make_unique<SearchService>(*this);
    launcher_ = std::make_unique<LauncherWindow>(*this);
    launcher_->Create(gtkApp_);
    g_application_hold(G_APPLICATION(gtkApp_));  // stay alive while the launcher is hidden

    std::wstring extensionNote;
    if (firstRun_ && IsGnomeSession() && windows_.State() == ExtensionState::NotInstalled) {
        // Window switching (and a reliable focus) need Beam's GNOME Shell extension: turn it on
        // now so it is there after the next login.
        std::wstring message;
        if (WindowService::InstallExtension(&message) && windows_.State() != ExtensionState::Running)
            extensionNote = L"切換視窗功能會在下次登入後啟用";
    }
    RegisterHotkeys(firstRun_, extensionNote);
    if (!SetAutostart(settings_.launchAtStartup) && settings_.launchAtStartup)
        Notify(L"無法設定開機啟動", L"無法寫入 ~/.config/autostart", true);
    Reindex();
    g_signal_connect(g_app_info_monitor_get(), "changed", Callback(+[](GAppInfoMonitor*, gpointer self) {
                         static_cast<App*>(self)->ScheduleReindex();  // apps installed / removed
                     }),
                     this);
    watchTimer_ = g_timeout_add(1500, +[](gpointer self) -> gboolean {
        auto* app = static_cast<App*>(self);
        app->CheckSettingsFile();
        app->CheckPlugins();
        return G_SOURCE_CONTINUE;
    }, this);
    for (int signal : {SIGTERM, SIGINT, SIGHUP}) {
        g_unix_signal_add(signal, +[](gpointer self) -> gboolean {
            static_cast<App*>(self)->Quit();
            return G_SOURCE_REMOVE;
        }, this);
    }

    if (!loaded.error.empty())
        Notify(L"settings.json 格式錯誤（暫時使用預設值）", Utf8ToWide(loaded.error), true);
    else if (!pluginProblems_.empty())
        Notify(L"外掛設定有問題", pluginProblems_, true);
}

int App::CommandLine(GApplicationCommandLine* commandLine) {
    GVariantDict* options = g_application_command_line_get_options_dict(commandLine);
    if (g_getenv("BEAM_DEBUG")) {
        GVariant* platform = g_application_command_line_get_platform_data(commandLine);
        gchar* text = platform ? g_variant_print(platform, FALSE) : g_strdup("-");
        g_printerr("beam: command line, remote=%d, platform data %s\n", g_application_command_line_get_is_remote(commandLine), text);
        g_free(text);
        if (platform) g_variant_unref(platform);
    }
    const bool first = !started_;
    started_ = true;
    if (g_variant_dict_contains(options, "quit")) {
        Quit();
        return 0;
    }
    if (g_variant_dict_contains(options, "unregister")) {
        g_application_command_line_print(commandLine, "%s", UnregisterDesktop());
        Quit();
        return 0;
    }
    if (g_variant_dict_contains(options, "dump-state")) {
        const std::string state = launcher_ ? WideToUtf8(launcher_->DescribeState()) : std::string("no launcher\n");
        g_application_command_line_print(commandLine, "%s", state.c_str());
        return 0;
    }
    if (g_variant_dict_contains(options, "background")) {
        if (first && firstRun_) ShowLauncher(Mode::Root);  // the very first start shows what Beam is
        return 0;
    }
    if (g_variant_dict_contains(options, "windows")) ToggleLauncher(Mode::Windows);
    else if (g_variant_dict_contains(options, "show")) ShowLauncher(Mode::Root);
    else ToggleLauncher(Mode::Root);
    return 0;
}

void App::WatchTheme() {
    dark_ = PrefersDarkTheme();
    GSettingsSchemaSource* source = g_settings_schema_source_get_default();
    GSettingsSchema* schema = source ? g_settings_schema_source_lookup(source, "org.gnome.desktop.interface", TRUE) : nullptr;
    if (!schema) return;
    const bool hasKey = g_settings_schema_has_key(schema, "color-scheme");
    g_settings_schema_unref(schema);
    if (!hasKey) return;
    interfaceSettings_ = g_settings_new("org.gnome.desktop.interface");
    g_signal_connect(interfaceSettings_, "changed::color-scheme", Callback(+[](GSettings*, char*, gpointer self) {
                         auto* app = static_cast<App*>(self);
                         const bool dark = PrefersDarkTheme();
                         if (dark == app->dark_) return;
                         app->dark_ = dark;
                         if (app->launcher_ && app->settings_.theme == L"auto") app->launcher_->OnThemeChanged();
                     }),
                     this);
    g_free(g_settings_get_string(interfaceSettings_, "color-scheme"));  // GSettings only reports keys that were read
}

void App::ShowLauncher(Mode mode) {
    if (!launcher_) return;
    if (!launcher_->IsShown() && NowMs() - lastIndexMs_ > kIndexMaxAgeMs) Reindex();
    launcher_->Show(mode);
}

void App::ToggleLauncher(Mode mode) {
    if (!launcher_) return;
    if (!launcher_->IsShown()) {
        ShowLauncher(mode);
        return;
    }
    launcher_->Toggle(mode);
}

void App::HideLauncher() {
    if (launcher_) launcher_->Hide();
}

void App::OpenSettingsFile() {
    const std::wstring path = settingsPath();
    if (!FileExists(path)) SaveSettings(Fs(path), settings_);
    std::wstring error;
    if (!OpenInTextEditor(path, &error)) Notify(L"無法開啟 settings.json", error, true);
}

void App::ScheduleReindex() {
    if (reindexTimer_) g_source_remove(reindexTimer_);
    reindexTimer_ = g_timeout_add(1500, +[](gpointer self) -> gboolean {
        auto* app = static_cast<App*>(self);
        app->reindexTimer_ = 0;
        app->Reindex();
        return G_SOURCE_REMOVE;
    }, this);
}

void App::Reindex() {
    if (indexing_) {
        reindexQueued_ = true;
        return;
    }
    if (indexThread_.joinable()) indexThread_.join();
    indexing_ = true;
    lastIndexMs_ = NowMs();
    std::weak_ptr<App*> weak = alive_;
    indexThread_ = std::thread([weak, folders = settings_.extraFolders] {
        auto* apps = new std::vector<AppEntry>();
        try {
            *apps = ScanApplications(folders);
        } catch (...) {
            Log(L"app scan failed");
        }
        struct Done {
            std::weak_ptr<App*> weak;
            std::vector<AppEntry>* apps;
        };
        g_idle_add_full(
            G_PRIORITY_DEFAULT,
            [](gpointer p) -> gboolean {
                std::unique_ptr<Done> done(static_cast<Done*>(p));
                std::unique_ptr<std::vector<AppEntry>> list(done->apps);
                if (auto app = done->weak.lock()) (*app)->OnIndexDone(std::move(*list));
                return G_SOURCE_REMOVE;
            },
            new Done{weak, apps}, nullptr);
    });
}

void App::OnIndexDone(std::vector<AppEntry> apps) {
    if (indexThread_.joinable()) indexThread_.join();  // it queued this as its last act
    apps_ = std::move(apps);
    indexing_ = false;
    search_->OnIndexChanged();
    if (launcher_) launcher_->OnIndexChanged();
    if (reindexQueued_) {
        reindexQueued_ = false;
        Reindex();
    }
}

void App::ReloadPlugins() {
    plugins_.Reload();
    pluginProblems_ = PluginProblemsText();
    if (launcher_) launcher_->OnPluginsChanged();
}

void App::CheckPlugins() {
    if (!plugins_.ReloadIfChanged()) return;
    if (launcher_) launcher_->OnPluginsChanged();
    const std::wstring problems = PluginProblemsText();
    if (!problems.empty() && problems != pluginProblems_) Notify(L"外掛設定有問題", problems, true);
    pluginProblems_ = problems;
}

std::wstring App::PluginProblemsText() const {
    std::wstring text;
    const auto& problems = plugins_.Problems();
    for (size_t i = 0; i < problems.size() && i < 3; ++i) text += (i ? L"\n" : L"") + problems[i];
    if (problems.size() > 3) text += L"\n…還有 " + std::to_wstring(problems.size() - 3) + L" 個（見「外掛偵錯紀錄」）";
    return text;
}

void App::Quit() {
    plugins_.CancelAll();
    for (guint* id : {&watchTimer_, &reindexTimer_}) {
        if (*id) g_source_remove(*id);
        *id = 0;
    }
    if (launcher_) launcher_->Hide();
    if (gtkApp_) g_application_quit(G_APPLICATION(gtkApp_));
}

void App::Notify(const std::wstring& title, const std::wstring& text, bool warning) {
    Log((warning ? L"[warn] " : L"") + title + L": " + text);
    ::beam::Notify(title, text, warning);
}

void App::RecordUsage(const std::wstring& key, const std::wstring& query) {
    usage_->Record(key, query, static_cast<int64_t>(std::time(nullptr)));
    usage_->Save();
}

void App::Defer(std::function<void()> fn) {
    auto* p = new std::function<void()>(std::move(fn));
    g_idle_add_full(
        G_PRIORITY_DEFAULT,
        [](gpointer data) -> gboolean {
            (*static_cast<std::function<void()>*>(data))();
            return G_SOURCE_REMOVE;
        },
        p, [](gpointer data) { delete static_cast<std::function<void()>*>(data); });
}

// A GNOME custom shortcut can fail without a trace on screen: gsd-media-keys asks GNOME Shell once
// to grab the keys, and when the shell refuses (one of its own shortcuts still has them: the window
// menu lets go of Alt+Space only a moment after Beam changed that setting), gsd-media-keys logs
// "Failed to grab accelerator" and never asks again. So Beam (1) gives GNOME a moment between
// releasing a conflicting shortcut and adding its own, and (2) then looks for such a refusal in
// the journal and makes gsd-media-keys ask again (also for a refusal from earlier in this login).
void App::RegisterHotkeys(bool announce, const std::wstring& extraNote) {
    std::wstring windowsHotkey = settings_.windowSwitcherHotkey;
    std::wstring problem;
    if (!Trim(windowsHotkey).empty() && GnomeAccelerator(windowsHotkey) == GnomeAccelerator(settings_.hotkey)) {
        problem = L"兩個快捷鍵不能相同";
        windowsHotkey.clear();
    }
    const unsigned generation = ++hotkeyGeneration_;
    if (!GnomeRunsShortcuts()) {
        if (announce) Notify(L"無法自動設定快捷鍵", NoGnomeHotkeyText(), true);
        else if (!extraNote.empty())
            Notify(L"Beam 已就緒", extraNote);
        return;
    }
    const HotkeySetup released = ReleaseHotkeyConflicts(settings_.hotkey, windowsHotkey);
    if (!released.problem.empty()) problem += (problem.empty() ? L"" : L"\n") + released.problem;
    std::weak_ptr<App*> weak = alive_;
    After(released.changed ? kSettleMs : 0, [this, weak, generation, announce, extraNote, windowsHotkey, released, problem] {
        if (!weak.lock() || generation != hotkeyGeneration_) return;
        const long gsd = MediaKeysPid();
        CountShortcutGrabFailuresAsync(gsd, [this, weak, generation, announce, extraNote, windowsHotkey, released, problem,
                                             gsd](int failures) {
            if (!weak.lock() || generation != hotkeyGeneration_) return;
            const ShortcutWrite wrote = WriteGnomeShortcuts(settings_.hotkey, windowsHotkey, ExecutablePath(), failures > 0);
            if (failures > 0) Log(L"GNOME refused Beam's shortcut earlier in this login; asking gsd-media-keys again");
            if (wrote.asked && failures >= 0) VerifyHotkeyGrab(generation, gsd, windowsHotkey, failures, 1);

            if (!problem.empty()) {
                Notify(L"快捷鍵可能無法使用", problem, true);
                return;
            }
            if (!announce && !released.changed && !wrote.changed) return;
            std::wstring body = L"按 " + settings_.hotkey + L" 隨時開啟 Beam";
            if (!Trim(windowsHotkey).empty()) body += L"，" + windowsHotkey + L" 切換視窗";
            if (!released.note.empty()) body += L"\n" + released.note;
            if (!extraNote.empty()) body += L"\n" + extraNote;
            Notify(announce ? L"Beam 已就緒" : L"已更新快捷鍵", body);
        });
    });
}

void App::VerifyHotkeyGrab(unsigned generation, long gsd, const std::wstring& windowsHotkey, int base, int attempt) {
    std::weak_ptr<App*> weak = alive_;
    After(kGrabCheckMs, [this, weak, generation, gsd, windowsHotkey, base, attempt] {
        if (!weak.lock() || generation != hotkeyGeneration_) return;
        CountShortcutGrabFailuresAsync(gsd, [this, weak, generation, gsd, windowsHotkey, base, attempt](int now) {
            if (!weak.lock() || generation != hotkeyGeneration_ || now < 0) return;
            if (now <= base) {
                if (attempt > 1) Log(L"GNOME accepted Beam's shortcut on attempt " + std::to_wstring(attempt));
                return;
            }
            if (attempt >= kGrabAttempts) {
                Notify(L"快捷鍵沒有生效",
                       L"GNOME 拒絕註冊 " + settings_.hotkey + L"，按鍵可能被別的程式占用。請登出再登入；還是不行就在 "
                       L"settings.json 換一組快捷鍵（beam --doctor 可以看原因）",
                       true);
                return;
            }
            Log(L"GNOME refused Beam's shortcut; asking gsd-media-keys again");
            WriteGnomeShortcuts(settings_.hotkey, windowsHotkey, ExecutablePath(), true);
            VerifyHotkeyGrab(generation, gsd, windowsHotkey, now, attempt + 1);
        });
    });
}

void App::ApplySettings(const Settings& s) {
    const Settings old = settings_;
    settings_ = s;
    if (s.hotkey != old.hotkey || s.windowSwitcherHotkey != old.windowSwitcherHotkey) RegisterHotkeys(false, L"");
    if (s.launchAtStartup != old.launchAtStartup && !SetAutostart(s.launchAtStartup))
        Notify(L"無法設定開機啟動", L"無法寫入 ~/.config/autostart", true);
    if (s.theme != old.theme && launcher_) launcher_->OnThemeChanged();
    if (s.extraFolders != old.extraFolders) Reindex();
    if (launcher_) launcher_->OnIndexChanged();  // quicklinks, result count... (when visible)
}

int64_t App::SettingsFileTime() const {
    struct stat st {};
    if (stat(WideToUtf8(settingsPath()).c_str(), &st) != 0) return 0;
    return static_cast<int64_t>(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec + st.st_size;
}

void App::CheckSettingsFile() {
    const int64_t now = SettingsFileTime();
    if (now == settingsTime_) return;
    settingsTime_ = now;
    const SettingsLoadResult r = LoadSettings(Fs(settingsPath()));
    settingsTime_ = SettingsFileTime();  // LoadSettings re-creates a deleted file
    if (!r.error.empty()) {
        Notify(L"settings.json 格式錯誤，未套用", Utf8ToWide(r.error), true);
        return;
    }
    if (r.settings == settings_) return;
    ApplySettings(r.settings);
    Notify(L"已套用新的設定", L"settings.json 已重新載入");
}

}  // namespace beam
