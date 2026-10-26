#include "win/app.hpp"

#include <commctrl.h>

#include <ctime>
#include <thread>

#include "core/hotkey.hpp"
#include "core/text.hpp"
#include "win/launcher.hpp"
#include "win/log.hpp"
#include "win/search.hpp"
#include "win/settings_dialog.hpp"
#include "win/shell.hpp"
#include "win/tray.hpp"

namespace beam {
namespace {
constexpr UINT_PTR kSettingsWatchTimer = 1;
constexpr ULONGLONG kIndexMaxAgeMs = 10 * 60 * 1000;
}  // namespace

App::App() = default;

App::~App() {
    cancelIndex_ = true;
    if (indexThread_.joinable()) indexThread_.join();
}

int App::Run(HINSTANCE instance, bool startHidden) {
    instance_ = instance;
    configDir_ = ConfigDirectory();
    log::Init(configDir_);
    log::Info(L"Beam starting");

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    const SettingsLoadResult loaded = LoadSettings(settingsPath());
    settings_ = loaded.settings;
    settingsTime_ = SettingsFileTime();
    usage_ = std::make_unique<UsageStore>(std::filesystem::path(configDir_) / L"usage.json");
    usage_->Load();
    favorites_ = FavoritesStore(std::filesystem::path(configDir_) / L"favorites.json");
    favorites_.Load();
    plugins_.Initialize(configDir_);
    plugins_.SetLog([](const std::wstring& text) { log::Info(text); });
    plugins_.Reload();
    plugins_.EnsureDocumentation();
    pluginProblems_ = PluginProblemsText();
    RefreshPalette();

    if (!CreateHostWindow()) return 1;
    taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");
    search_ = std::make_unique<SearchService>(*this);
    launcher_ = std::make_unique<LauncherWindow>(*this);
    if (!launcher_->Create(instance)) {
        MessageBoxW(nullptr, L"無法初始化 Direct2D / DirectWrite。", kAppName, MB_ICONERROR);
        return 1;
    }
    icons_.Start(launcher_->hwnd(), 48);
    RegisterHotkeyBoxClass(instance);
    tray_ = std::make_unique<TrayIcon>(*this, host_, instance);
    tray_->Add();

    const std::wstring hotkeyError = RegisterHotkeys(settings_);
    if (settings_.launchAtStartup) SetLaunchAtStartup(true);  // refresh the path if the exe moved
    Reindex();
    SetTimer(host_, kSettingsWatchTimer, 1500, nullptr);

    if (!loaded.error.empty())
        Notify(L"settings.json 格式錯誤（暫時使用預設值）", Utf8ToWide(loaded.error), true);
    if (!hotkeyError.empty()) Notify(L"快捷鍵無法使用", hotkeyError + L"\n請在設定中更換。", true);
    else if (!pluginProblems_.empty()) Notify(L"外掛設定有問題", pluginProblems_, true);
    if (loaded.created) ShowSettings(true);
    else if (!startHidden) ShowLauncher(Mode::Root);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (settingsDialog_ && settingsDialog_->hwnd() && IsDialogMessageW(settingsDialog_->hwnd(), &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    cancelIndex_ = true;
    if (indexThread_.joinable()) indexThread_.join();
    plugins_.CancelAll();
    icons_.Stop();
    usage_->Save();
    settingsDialog_.reset();
    launcher_.reset();
    tray_.reset();
    if (host_) DestroyWindow(host_);
    log::Info(L"Beam stopped");
    return static_cast<int>(msg.wParam);
}

bool App::CreateHostWindow() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HostProc;
    wc.hInstance = instance_;
    wc.lpszClassName = kHostWindowClass;
    RegisterClassExW(&wc);
    // A hidden top-level window (not message-only) so it also receives TaskbarCreated.
    host_ = CreateWindowExW(WS_EX_TOOLWINDOW, kHostWindowClass, kAppName, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                            instance_, this);
    return host_ != nullptr;
}

LRESULT CALLBACK App::HostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->host_ = hwnd;
    }
    if (msg == WM_NCDESTROY && self) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        self->host_ = nullptr;
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return self ? self->HandleHostMessage(hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT App::HandleHostMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (taskbarCreatedMessage_ != 0 && msg == taskbarCreatedMessage_) {
        if (tray_) tray_->Add();
        return 0;
    }
    switch (msg) {
        case WM_HOTKEY:
            if (wp == kHotkeyLauncher) ToggleLauncher(Mode::Root);
            else if (wp == kHotkeyWindows) ToggleLauncher(Mode::Windows);
            return 0;
        case WM_APP_SHOW:
            ShowLauncher(Mode::Root);
            return 0;
        case WM_APP_TRAY:
            if (tray_) tray_->OnCallback(wp, lp);
            return 0;
        case WM_APP_INDEX_DONE: {
            std::unique_ptr<std::vector<AppEntry>> apps(reinterpret_cast<std::vector<AppEntry>*>(lp));
            if (indexThread_.joinable()) indexThread_.join();  // it posted this as its last act
            apps_ = std::move(*apps);
            indexing_ = false;
            search_->OnIndexChanged();
            std::vector<std::wstring> iconPaths;
            iconPaths.reserve(apps_.size());
            for (const auto& a : apps_) iconPaths.push_back(a.iconPath);
            icons_.Prefetch(iconPaths);
            if (launcher_) launcher_->OnIndexChanged();
            if (reindexQueued_) {
                reindexQueued_ = false;
                Reindex();
            }
            return 0;
        }
        case WM_APP_DEFERRED: {
            auto pending = std::move(deferred_);
            deferred_.clear();
            for (auto& fn : pending) fn();
            return 0;
        }
        case WM_TIMER:
            if (wp == kSettingsWatchTimer) {
                CheckSettingsFile();
                CheckPlugins();
            }
            return 0;
        case WM_SETTINGCHANGE:
            if (lp && lstrcmpiW(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0) {
                RefreshPalette();
                if (launcher_) launcher_->OnThemeChanged();
            }
            return 0;
        case WM_QUERYENDSESSION:
            usage_->Save();
            return TRUE;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void App::RefreshPalette() { palette_ = ResolvePalette(settings_.theme); }

void App::ShowLauncher(Mode mode) {
    if (!launcher_) return;
    if (!launcher_->IsShown()) {
        RefreshPalette();
        if (GetTickCount64() - lastIndexTick_ > kIndexMaxAgeMs) Reindex();
    }
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

void App::ShowSettings(bool welcome) {
    if (settingsDialog_ && settingsDialog_->hwnd()) {
        settingsDialog_->Activate();
        return;
    }
    HideLauncher();
    settingsDialog_ = std::make_unique<SettingsDialog>(*this, welcome);
    if (!settingsDialog_->Create()) settingsDialog_.reset();
}

void App::OnSettingsDialogClosed() {
    SetHotkeysSuspended(false);
    Defer([this] {
        if (settingsDialog_ && !settingsDialog_->hwnd()) settingsDialog_.reset();
    });
}

void App::OpenSettingsFile() {
    const std::wstring path = settingsPath();
    if (!FileExists(path)) SaveSettings(path, settings_);
    ShellOpen(L"notepad.exe", {L"\"" + path + L"\""});
}

void App::Reindex() {
    if (indexing_) {
        reindexQueued_ = true;
        return;
    }
    if (indexThread_.joinable()) indexThread_.join();
    indexing_ = true;
    cancelIndex_ = false;
    lastIndexTick_ = GetTickCount64();
    const HWND host = host_;
    indexThread_ = std::thread([this, host, folders = settings_.extraFolders] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        std::vector<AppEntry>* apps = nullptr;
        try {
            apps = new std::vector<AppEntry>(ScanApplications(folders, &cancelIndex_));
        } catch (...) {
            log::Error(L"App scan failed");
            apps = new std::vector<AppEntry>();
        }
        CoUninitialize();
        if (!PostMessageW(host, WM_APP_INDEX_DONE, 0, reinterpret_cast<LPARAM>(apps))) delete apps;
    });
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
    KillTimer(host_, kSettingsWatchTimer);
    UnregisterHotkeys();
    if (settingsDialog_ && settingsDialog_->hwnd()) DestroyWindow(settingsDialog_->hwnd());
    if (launcher_) launcher_->Hide();
    if (tray_) tray_->Remove();
    PostQuitMessage(0);
}

void App::Notify(const std::wstring& title, const std::wstring& text, bool warning) {
    log::Write(warning ? L"WARN" : L"INFO", title + L": " + text);
    if (tray_) tray_->Balloon(title, text, warning);
}

void App::RecordUsage(const std::wstring& key, const std::wstring& query) {
    usage_->Record(key, query, static_cast<int64_t>(std::time(nullptr)));
    usage_->Save();
}

void App::Defer(std::function<void()> fn) {
    deferred_.push_back(std::move(fn));
    PostMessageW(host_, WM_APP_DEFERRED, 0, 0);
}

std::wstring App::RegisterHotkeys(const Settings& s) {
    UnregisterHotkeys();
    std::wstring errors;
    auto addError = [&errors](const std::wstring& e) {
        if (!errors.empty()) errors += L"\n";
        errors += e;
    };
    const auto mainHotkey = ParseHotkey(s.hotkey);
    if (!mainHotkey) {
        addError(L"無法解析快捷鍵「" + s.hotkey + L"」");
    } else if (!RegisterHotKey(host_, kHotkeyLauncher, mainHotkey->mods | MOD_NOREPEAT, mainHotkey->vk)) {
        addError(L"「" + FormatHotkey(*mainHotkey) + L"」已被其他程式使用");
    }
    if (!Trim(s.windowSwitcherHotkey).empty()) {
        const auto windowsHotkey = ParseHotkey(s.windowSwitcherHotkey);
        if (!windowsHotkey) {
            addError(L"無法解析快捷鍵「" + s.windowSwitcherHotkey + L"」");
        } else if (mainHotkey && *windowsHotkey == *mainHotkey) {
            addError(L"兩個快捷鍵不能相同");
        } else if (!RegisterHotKey(host_, kHotkeyWindows, windowsHotkey->mods | MOD_NOREPEAT, windowsHotkey->vk)) {
            addError(L"「" + FormatHotkey(*windowsHotkey) + L"」已被其他程式使用");
        }
    }
    hotkeysRegistered_ = true;
    return errors;
}

void App::UnregisterHotkeys() {
    if (!host_) return;
    UnregisterHotKey(host_, kHotkeyLauncher);
    UnregisterHotKey(host_, kHotkeyWindows);
    hotkeysRegistered_ = false;
}

void App::SetHotkeysSuspended(bool suspended) {
    if (suspended == hotkeysSuspended_) return;
    hotkeysSuspended_ = suspended;
    if (suspended) UnregisterHotkeys();
    else RegisterHotkeys(settings_);
}

std::wstring App::ApplySettings(const Settings& s, bool persist) {
    hotkeysSuspended_ = false;
    const std::wstring error = RegisterHotkeys(s);
    if (!error.empty()) {
        RegisterHotkeys(settings_);  // keep the previous, working hotkeys
        return error;
    }
    const bool foldersChanged = s.extraFolders != settings_.extraFolders;
    const bool startupChanged = s.launchAtStartup != settings_.launchAtStartup;
    settings_ = s;
    if (startupChanged && !SetLaunchAtStartup(s.launchAtStartup))
        Notify(L"無法設定開機啟動", L"寫入登錄機碼失敗", true);
    RefreshPalette();
    if (launcher_) launcher_->OnThemeChanged();
    if (tray_) tray_->Add();  // refreshes the tooltip
    if (foldersChanged) Reindex();
    if (persist) {
        if (!SaveSettings(settingsPath(), settings_)) Notify(L"無法儲存設定", settingsPath(), true);
        settingsTime_ = SettingsFileTime();
    }
    return L"";
}

FILETIME App::SettingsFileTime() const {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(settingsPath().c_str(), GetFileExInfoStandard, &data)) return FILETIME{};
    return data.ftLastWriteTime;
}

void App::CheckSettingsFile() {
    const FILETIME now = SettingsFileTime();
    if (CompareFileTime(&now, &settingsTime_) == 0) return;
    settingsTime_ = now;
    const SettingsLoadResult r = LoadSettings(settingsPath());
    if (!r.error.empty()) {
        Notify(L"settings.json 格式錯誤，未套用", Utf8ToWide(r.error), true);
        return;
    }
    if (r.settings == settings_) return;
    const std::wstring error = ApplySettings(r.settings, false);
    if (!error.empty()) Notify(L"新的快捷鍵無法使用，未套用", error, true);
    else Notify(L"已套用新的設定", L"settings.json 已重新載入", false);
}

}  // namespace beam
