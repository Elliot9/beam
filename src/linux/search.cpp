#include "linux/search.hpp"

#include <gio/gio.h>

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <system_error>

#include "core/calc.hpp"
#include "core/favorites.hpp"
#include "core/fuzzy.hpp"
#include "core/query.hpp"
#include "core/text.hpp"
#include "linux/app.hpp"
#include "linux/system.hpp"
#include "plugin/plugin_process.hpp"

namespace beam {
namespace {

constexpr wchar_t kSectionResults[] = L"結果";
constexpr wchar_t kSectionFavorites[] = L"最愛";
constexpr wchar_t kSectionNewPlugin[] = L"建立新外掛";
constexpr wchar_t kSectionRecent[] = L"最近使用";
constexpr wchar_t kSectionSuggestions[] = L"建議";
constexpr wchar_t kSectionWindows[] = L"已開啟的視窗";
constexpr wchar_t kSectionWeb[] = L"網路搜尋";

int64_t Now() { return static_cast<int64_t>(std::time(nullptr)); }

Outcome OpenOrReport(LauncherHost& host, const std::wstring& target) {
    std::wstring error;
    if (OpenTarget(target, &error)) return Outcome::Hide;
    host.ShowStatus(L"無法開啟：" + error, true);
    return Outcome::Stay;
}

Outcome Report(LauncherHost& host, bool ok, const std::wstring& error, Outcome success = Outcome::Hide) {
    if (ok) return success;
    host.ShowStatus(L"無法執行：" + error, true);
    return Outcome::Stay;
}

Action CopyAction(std::wstring title, std::wstring text, Shortcut shortcut = Keys(GDK_KEY_c, true, true)) {
    return Action{std::move(title), icon::kCopy, shortcut, false, [text = std::move(text)](LauncherHost&) {
                      CopyText(text);
                      return Outcome::Hide;
                  }};
}

Action RevealAction(std::wstring path) {
    return Action{L"在檔案管理員中顯示", icon::kFolderOpen, Keys(GDK_KEY_e, true, true), false,
                  [path = std::move(path)](LauncherHost&) {
                      RevealInFileManager(path);
                      return Outcome::Hide;
                  }};
}

std::wstring HostOf(const std::wstring& url) {
    size_t start = url.find(L"://");
    start = start == std::wstring::npos ? 0 : start + 3;
    size_t end = url.find_first_of(L"/?#", start);
    std::wstring host = url.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
    if (StartsWithIgnoreCase(host, L"www.")) host = host.substr(4);
    return host;
}

void SortByScore(std::vector<ResultItem>& items) {
    std::stable_sort(items.begin(), items.end(), [](const ResultItem& a, const ResultItem& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.title.size() < b.title.size();
    });
}

bool CanBeFavorite(const std::wstring& key) {
    return key.rfind(L"app:", 0) == 0 || key.rfind(L"cmd:", 0) == 0 || key.rfind(L"ql:", 0) == 0 ||
           key.rfind(L"plugin:", 0) == 0;
}

// Moves a favorite one visible step: until it passes `neighbor` (hidden entries, e.g. apps
// that are not installed any more, are skipped so every key press visibly moves the row).
bool MoveFavoritePast(FavoritesStore& favorites, const std::wstring& key, const std::wstring& neighbor, int direction) {
    for (int guard = 0; guard < 4096; ++guard) {
        const int a = favorites.IndexOf(key);
        const int b = favorites.IndexOf(neighbor);
        if (a < 0 || b < 0) return false;
        if (direction < 0 ? a < b : a > b) return true;
        if (!favorites.Move(key, direction)) return false;
    }
    return false;
}

bool LooksLikePosixPath(const std::wstring& q) {
    if (q.empty()) return false;
    if (q[0] == L'/') return true;
    if (q == L"~" || q.rfind(L"~/", 0) == 0) return true;
    if (q[0] == L'$' && q.size() > 1 && (std::iswupper(q[1]) || q[1] == L'{')) return q.find(L'/') != std::wstring::npos || q.size() > 2;
    return false;
}

// File-type icon without touching the file (name based).
ItemIcon FileIcon(const std::wstring& path, bool directory) {
    GIcon* gicon = nullptr;
    if (directory) {
        gicon = g_themed_icon_new("folder");
    } else {
        gboolean uncertain = FALSE;
        gchar* type = g_content_type_guess(U8(FileNameOf(path)).c_str(), nullptr, 0, &uncertain);
        gicon = g_content_type_get_icon(type);
        g_free(type);
    }
    ItemIcon icon = GIconOf(gicon);
    g_object_unref(gicon);
    return icon;
}

// Segoe MDL2 code points used by Windows plugins -> freedesktop symbolic icons.
const char* GlyphToIcon(wchar_t glyph) {
    switch (glyph) {
        case 0xE721: return icon::kSearch;
        case 0xE946: return icon::kInfo;
        case 0xEA86: return icon::kPuzzle;
        case 0xE774: case 0xE909: case 0xE12B: return icon::kGlobe;
        case 0xE8A7: return "window-new-symbolic";
        case 0xE8BD: case 0xE715: return "mail-unread-symbolic";
        case 0xE724: return "mail-send-symbolic";
        case 0xE713: case 0xE115: return icon::kSettings;
        case 0xE8C8: case 0xE16F: return icon::kCopy;
        case 0xE8B7: return icon::kFolder;
        case 0xE838: return icon::kFolderOpen;
        case 0xE756: return icon::kTerminal;
        case 0xE72C: return icon::kRefresh;
        case 0xE768: return icon::kPlay;
        case 0xE7BA: return icon::kWarning;
        case 0xE71B: return icon::kLink;
        case 0xE787: case 0xE163: return "x-office-calendar-symbolic";
        case 0xE8D6: return "audio-x-generic-symbolic";
        case 0xE7C3: case 0xE8A5: return icon::kFile;
        case 0xE943: return icon::kCode;
        case 0xE734: return icon::kStar;
        case 0xE735: return icon::kStarFill;
        case 0xE710: return icon::kAdd;
        case 0xE74D: return "edit-delete-symbolic";
        case 0xE72E: return icon::kLock;
        case 0xE7E8: return icon::kPower;
        case 0xE80F: return "go-home-symbolic";
        case 0xE77B: return "avatar-default-symbolic";
        case 0xE896: return "folder-download-symbolic";
        case 0xE722: return "camera-photo-symbolic";
        case 0xE753: return "weather-few-clouds-symbolic";
        case 0xE823: case 0xE121: return "preferences-system-time-symbolic";
        case 0xE74E: return "document-save-symbolic";
        case 0xEB9F: case 0xE91B: return "image-x-generic-symbolic";
        case 0xE714: return "video-x-generic-symbolic";
        case 0xE7B8: return "package-x-generic-symbolic";
        case 0xE968: return "network-server-symbolic";
        case 0xE70F: return icon::kEdit;
        case 0xE9D9: return icon::kMonitor;
        case 0xE8FD: return icon::kList;
        case 0xE7F4: return icon::kWindows;
        case 0xE897: return "help-browser-symbolic";
        default: return icon::kPuzzle;
    }
}

std::wstring Shorten(const std::wstring& text, size_t max) {
    return text.size() > max ? text.substr(0, max - 1) + L"…" : text;
}

}  // namespace

SearchService::SearchService(App& app) : app_(app) { BuildCommands(); }

void SearchService::OnIndexChanged() {
    appsByKey_.clear();
    appNames_.clear();
    const auto& apps = app_.apps();
    for (size_t i = 0; i < apps.size(); ++i) {
        appsByKey_.emplace(apps[i].key, i);
        appNames_.insert(Fold(apps[i].name));
    }
}

void SearchService::BuildCommands() {
    App* app = &app_;  // captured by value in the command lambdas
    auto add = [this](std::wstring id, std::wstring title, std::wstring subtitle, std::vector<std::wstring> aliases,
                      const char* icon, uint32_t color, bool dangerous, std::function<Outcome(LauncherHost&)> run) {
        commands_.push_back(Command{L"cmd:" + id, std::move(title), std::move(subtitle), std::move(aliases), icon, color,
                                    dangerous, std::move(run)});
    };
    auto deferred = [app](void (*fn)()) {
        return [app, fn](LauncherHost&) {
            app->Defer(fn);
            return Outcome::Hide;
        };
    };

    add(L"switch-windows", L"切換視窗", L"搜尋並切換到已開啟的視窗",
        {L"switch windows", L"windows", L"alt tab", L"window switcher"}, icon::kWindows, tint::kWindow, false,
        [](LauncherHost& host) {
            host.EnterMode(Mode::Windows);
            return Outcome::Stay;
        });
    add(L"settings", L"Beam 設定", L"settings.json：快捷鍵、快速連結、主題、工具資料夾",
        {L"beam settings", L"preferences", L"hotkey", L"快捷鍵", L"config", L"settings.json", L"quicklinks"},
        icon::kSettings, tint::kBeam, false, [app](LauncherHost&) {
            app->OpenSettingsFile();
            return Outcome::Hide;
        });
    add(L"reindex", L"重新建立應用程式索引", L"重新讀取已安裝的應用程式與工具資料夾",
        {L"reload", L"reindex", L"refresh apps", L"rescan"}, icon::kRefresh, tint::kBeam, false,
        [app](LauncherHost& host) {
            app->Reindex();
            host.ShowStatus(L"已重新建立索引：" + std::to_wstring(app->apps().size()) + L" 個應用程式", false);
            return Outcome::Stay;
        });
    add(L"window-extension", L"設定視窗切換（GNOME 擴充功能）", L"安裝並啟用 Beam 的 GNOME Shell 擴充功能",
        {L"window extension", L"gnome extension", L"enable window switching"}, icon::kPuzzle, tint::kWindow, false,
        [](LauncherHost& host) {
            std::wstring message;
            const bool ok = WindowService::InstallExtension(&message);
            host.ShowStatus(message, !ok);
            return Outcome::Stay;
        });
    add(L"quit", L"結束 Beam", L"關閉 Beam（快捷鍵會重新啟動它）", {L"quit beam", L"exit beam"}, icon::kExit,
        tint::kBeam, false, [app](LauncherHost&) {
            app->Defer([app] { app->Quit(); });
            return Outcome::Hide;
        });

    add(L"new-plugin", L"建立新外掛", L"從 Python / Node.js / Shell 範本開始",
        {L"new plugin", L"create plugin", L"newplugin", L"plugin template", L"extension", L"建立外掛"}, icon::kPuzzle,
        tint::kPlugin, false, [](LauncherHost& host) {
            host.SetQuery(L"newplugin ");
            return Outcome::Stay;
        });
    add(L"reload-plugins", L"重新載入外掛", L"重新讀取外掛資料夾裡的 plugin.json",
        {L"reload plugins", L"refresh plugins", L"rescan plugins"}, icon::kRefresh, tint::kPlugin, false,
        [app](LauncherHost& host) {
            app->ReloadPlugins();
            const PluginManager& plugins = app->plugins();
            std::wstring message = L"已載入 " + std::to_wstring(plugins.Commands().size()) + L" 個外掛指令";
            if (!plugins.Problems().empty())
                message += L"，" + std::to_wstring(plugins.Problems().size()) + L" 個問題（見「外掛偵錯紀錄」）";
            host.ShowStatus(message, !plugins.Problems().empty());
            return Outcome::Stay;
        });
    add(L"plugin-log", L"外掛偵錯紀錄", L"最近 30 次外掛執行的輸入、輸出與錯誤",
        {L"plugin log", L"plugin debug", L"plugin output", L"debug plugins", L"外掛輸出"}, icon::kMonitor,
        tint::kPlugin, false, [app](LauncherHost& host) {
            host.ShowDetail(L"外掛偵錯紀錄", app->plugins().DescribeRecords());
            return Outcome::Stay;
        });
    add(L"plugin-folder", L"開啟外掛資料夾", L"~/.config/beam/plugins", {L"plugins folder", L"open plugins", L"plugin folder"},
        icon::kFolderOpen, tint::kPlugin, false, [app](LauncherHost& host) { return OpenOrReport(host, app->plugins().Directory()); });

    add(L"lock", L"鎖定螢幕", L"", {L"lock screen", L"lock"}, icon::kLock, tint::kSystem, false, deferred(LockScreen));
    add(L"sleep", L"睡眠", L"", {L"sleep", L"suspend"}, icon::kSleep, tint::kSystem, false, deferred(Suspend));
    add(L"shutdown", L"關機", L"", {L"shut down", L"shutdown", L"power off"}, icon::kPower, tint::kDanger, true,
        deferred(PowerOff));
    add(L"restart", L"重新啟動", L"", {L"restart", L"reboot"}, icon::kRestart, tint::kDanger, true, deferred(Reboot));
    add(L"signout", L"登出", L"", {L"sign out", L"log out", L"logout"}, icon::kSignOut, tint::kDanger, true,
        deferred(LogOut));
    add(L"empty-trash", L"清空垃圾桶", L"", {L"empty trash", L"empty recycle bin"}, icon::kTrashFull, tint::kDanger,
        true, deferred(EmptyTrash));
    add(L"trash", L"垃圾桶", L"", {L"trash", L"recycle bin"}, icon::kTrash, tint::kSystem, false,
        [](LauncherHost& host) { return OpenOrReport(host, L"trash:///"); });
    add(L"terminal", L"開啟終端機", L"在家目錄開新的終端機視窗", {L"terminal", L"shell", L"console"}, icon::kTerminal,
        tint::kShell, false, [](LauncherHost& host) {
            std::wstring error;
            return Report(host, OpenTerminalAt(HomeDir(), &error), error);
        });
}

std::vector<ResultItem> SearchService::Search(const std::wstring& raw, Mode mode, const WindowState& windows) const {
    const std::wstring q = Trim(raw);
    if (mode == Mode::Windows) return WindowResults(q, windows);
    if (q.empty()) return EmptyState(windows);

    const Settings& settings = app_.settings();
    const PluginManager& plugins = app_.plugins();
    std::vector<ResultItem> top;

    if (q[0] == L'>') {
        top.push_back(ShellCommandItem(Trim(std::wstring_view(q).substr(1))));
        return top;
    }
    // "newplugin <name>" lists the templates ("newplugin " from the 建立新外掛 command).
    if (const KeywordSplit typed = SplitKeyword(raw); typed.hasSpace && EqualsIgnoreCase(typed.keyword, L"newplugin"))
        return NewPluginItems(Trim(typed.rest));

    const KeywordSplit split = SplitKeyword(q);
    if (split.hasSpace) {
        for (const auto& link : settings.quicklinks) {
            if (!link.keyword.empty() && EqualsIgnoreCase(link.keyword, split.keyword))
                top.push_back(QuicklinkItem(link, split.rest));
        }
        // "keyword argument" runs a command plugin with that argument (list plugins are
        // entered by the launcher as soon as the space is typed).
        if (const PluginCommand* pc = plugins.FindByKeyword(split.keyword); pc && pc->spec.mode == PluginMode::Command)
            top.push_back(PluginCommandItem(*pc, split.rest));
    }
    if (const auto value = TryCalculate(q)) top.push_back(CalculatorItem(q, *value));
    if (const auto url = TryGetUrl(q)) top.push_back(UrlItem(*url));
    if (LooksLikePosixPath(q)) {
        auto paths = PathResults(q);
        top.insert(top.end(), std::make_move_iterator(paths.begin()), std::make_move_iterator(paths.end()));
        for (auto& item : top) item.section = kSectionResults;
        return top;
    }

    const int64_t now = Now();
    UsageStore& usage = app_.usage();
    const FavoritesStore& favorites = app_.favorites();
    std::vector<ResultItem> scored;
    auto push = [&](ResultItem item, int score, double bias) {
        double boost = 0;
        if (!item.usageKey.empty()) {
            boost = usage.Boost(item.usageKey, q, now);
            if (favorites.Contains(item.usageKey)) boost += 60;  // pinned items win close calls
        }
        item.score = score + bias + boost;
        scored.push_back(std::move(item));
    };

    for (const auto& app : app_.apps()) {
        const int s = BestScore(q, app.name, app.aliases);
        if (s != kNoMatch) push(AppItem(app), s, 0);
    }
    for (const auto& cmd : commands_) {
        if (appNames_.count(Fold(cmd.title))) continue;  // an installed app already has this name
        const int s = BestScore(q, cmd.title, cmd.aliases);
        if (s != kNoMatch) push(CommandItem(cmd), s, 0);
    }
    for (const auto& pc : plugins.Commands()) {
        std::vector<std::wstring> aliases;
        if (!pc.spec.keyword.empty()) aliases.push_back(pc.spec.keyword);
        if (pc.pluginName != pc.spec.name) aliases.push_back(pc.pluginName);
        int s = BestScore(q, pc.spec.name, aliases);
        if (!pc.spec.keyword.empty() && EqualsIgnoreCase(pc.spec.keyword, q)) s = std::max(s, 950);
        if (s != kNoMatch) push(PluginCommandItem(pc, L""), s, 0);
    }
    for (const auto& link : settings.quicklinks) {
        int s = FuzzyScore(q, link.name);
        if (!link.keyword.empty() && EqualsIgnoreCase(link.keyword, q)) s = std::max(s, 950);
        if (s != kNoMatch) push(QuicklinkItem(link, L""), s, 0);
    }
    if (settings.includeWindowsInSearch) {
        for (const auto& w : windows.windows) {
            const int s = BestScore(q, w.title, {w.appName}, 100);
            if (s != kNoMatch) push(WindowItem(w), s, -20);
        }
    }
    SortByScore(scored);
    if (scored.size() > static_cast<size_t>(settings.maxResults)) scored.resize(settings.maxResults);

    std::vector<ResultItem> results = std::move(top);
    results.insert(results.end(), std::make_move_iterator(scored.begin()), std::make_move_iterator(scored.end()));
    for (auto& item : results) {
        item.section = kSectionResults;
        AddFavoriteActions(item, false);
    }
    ResultItem web = WebSearchItem(q);
    web.section = kSectionWeb;
    results.push_back(std::move(web));
    return results;
}

std::vector<ResultItem> SearchService::EmptyState(const WindowState& windows) const {
    std::vector<ResultItem> items;
    const FavoritesStore& favorites = app_.favorites();
    for (const auto& key : favorites.Keys()) {
        if (auto item = ResolveUsageKey(key)) {
            item->section = kSectionFavorites;
            items.push_back(std::move(*item));
            if (items.size() == 30) break;
        }
    }
    const size_t pinned = items.size();
    App* app = &app_;
    for (size_t i = 0; i < pinned; ++i) {
        const std::wstring key = items[i].usageKey;
        auto move = [app, key](const std::wstring& neighbor, int direction) {
            return [app, key, neighbor, direction](LauncherHost& host) {
                if (MoveFavoritePast(app->favorites(), key, neighbor, direction)) {
                    if (!app->favorites().Save()) host.ShowStatus(L"無法儲存 favorites.json", true);
                    host.Refresh(key);
                }
                return Outcome::Stay;
            };
        };
        // Ctrl+Shift+Up/Down; Ctrl+Alt+Up/Down also works where GNOME does not use it for workspaces.
        if (i > 0)
            items[i].actions.push_back(Action{L"上移", icon::kUp, Keys(GDK_KEY_Up, true, true), false,
                                              move(items[i - 1].usageKey, -1), Keys(GDK_KEY_Up, true, false, true),
                                              false});
        if (i + 1 < pinned)
            items[i].actions.push_back(Action{L"下移", icon::kDown, Keys(GDK_KEY_Down, true, true), false,
                                              move(items[i + 1].usageKey, 1), Keys(GDK_KEY_Down, true, false, true),
                                              false});
        AddFavoriteActions(items[i], true);
    }

    size_t recent = 0;
    for (const auto& key : app_.usage().RecentKeys(24)) {
        if (favorites.Contains(key)) continue;
        if (auto item = ResolveUsageKey(key)) {
            item->section = kSectionRecent;
            AddFavoriteActions(*item, false);
            items.push_back(std::move(*item));
            if (++recent == 6) break;
        }
    }
    if (items.empty()) {
        for (const auto& cmd : commands_) {
            if (cmd.key == L"cmd:switch-windows" || cmd.key == L"cmd:settings" || cmd.key == L"cmd:new-plugin") {
                ResultItem item = CommandItem(cmd);
                item.section = kSectionSuggestions;
                AddFavoriteActions(item, false);
                items.push_back(std::move(item));
            }
        }
    }
    size_t shown = 0;
    for (const auto& w : windows.windows) {
        if (shown++ == 12) break;
        ResultItem item = WindowItem(w);
        item.section = kSectionWindows;
        items.push_back(std::move(item));
    }
    return items;
}

void SearchService::AddFavoriteActions(ResultItem& item, bool inFavoritesSection) const {
    const std::wstring key = item.usageKey;
    if (key.empty() || !CanBeFavorite(key)) return;
    const bool pinned = app_.favorites().Contains(key);
    if (pinned && !inFavoritesSection) item.accessory = L"★ " + item.accessory;
    App* app = &app_;
    item.actions.push_back(Action{pinned ? L"從最愛移除" : L"加入最愛", pinned ? icon::kStarFill : icon::kStar,
                                  Keys(GDK_KEY_f, true, true), false,
                                  [app, key, pinned](LauncherHost& host) {
                                      FavoritesStore& favorites = app->favorites();
                                      if (pinned) favorites.Remove(key);
                                      else favorites.Add(key);
                                      if (!favorites.Save()) host.ShowStatus(L"無法儲存 favorites.json", true);
                                      else host.ShowStatus(pinned ? L"已從最愛移除" : L"已加入最愛（清空搜尋框就會看到）", false);
                                      host.Refresh(key);
                                      return Outcome::Stay;
                                  },
                                  Shortcut{}, false});
}

std::vector<ResultItem> SearchService::WindowResults(const std::wstring& q, const WindowState& state) const {
    std::vector<ResultItem> items;
    if (state.extension != ExtensionState::Running) {
        items.push_back(ExtensionItem(state.extension));
        return items;
    }
    for (size_t i = 0; i < state.windows.size(); ++i) {
        const WindowInfo& w = state.windows[i];
        int s = 0;
        if (!q.empty()) {
            s = BestScore(q, w.title, {w.appName}, 60);
            if (s == kNoMatch) continue;
        }
        ResultItem item = WindowItem(w);
        item.section = L"視窗";
        item.score = q.empty() ? -static_cast<double>(i) : s;  // keep the most-recently-used order when not filtering
        items.push_back(std::move(item));
    }
    SortByScore(items);
    return items;
}

ResultItem SearchService::ExtensionItem(ExtensionState state) const {
    ResultItem item;
    item.section = L"視窗";
    item.icon = SymbolicIcon(icon::kPuzzle, tint::kWindow);
    if (state == ExtensionState::NeedsRelogin) {
        item.title = L"視窗切換擴充功能已啟用，登出再登入一次就能使用";
        item.subtitle = L"GNOME 只在登入時載入新的擴充功能";
        item.actions.push_back(Action{L"知道了", icon::kInfo, Keys(GDK_KEY_Return), false,
                                      [](LauncherHost&) { return Outcome::Stay; }});
        return item;
    }
    if (state == ExtensionState::Unsupported) {
        item.title = L"視窗切換需要 GNOME 桌面";
        item.subtitle = L"在 Wayland 上只有 GNOME Shell 能列出其他程式的視窗";
        item.actions.push_back(Action{L"知道了", icon::kInfo, Keys(GDK_KEY_Return), false,
                                      [](LauncherHost&) { return Outcome::Stay; }});
        return item;
    }
    item.title = L"啟用視窗切換";
    item.subtitle = L"安裝 Beam 的 GNOME Shell 擴充功能（Wayland 上只有它能看到其他視窗）";
    item.actions.push_back(Action{L"安裝並啟用", icon::kAdd, Keys(GDK_KEY_Return), false, [](LauncherHost& host) {
                                      std::wstring message;
                                      const bool ok = WindowService::InstallExtension(&message);
                                      host.ShowStatus(message, !ok);
                                      host.RefreshWindows();
                                      return Outcome::Stay;
                                  }});
    return item;
}

std::optional<ResultItem> SearchService::ResolveUsageKey(const std::wstring& key) const {
    if (key.rfind(L"app:", 0) == 0) {
        const auto it = appsByKey_.find(key);
        if (it != appsByKey_.end()) return AppItem(app_.apps()[it->second]);
        return std::nullopt;
    }
    if (key.rfind(L"cmd:", 0) == 0) {
        for (const auto& cmd : commands_)
            if (cmd.key == key) return CommandItem(cmd);
        return std::nullopt;
    }
    if (key.rfind(L"ql:", 0) == 0) {
        for (const auto& link : app_.settings().quicklinks)
            if (L"ql:" + link.name == key) return QuicklinkItem(link, L"");
        return std::nullopt;
    }
    if (key.rfind(L"plugin:", 0) == 0) {
        if (const PluginCommand* pc = app_.plugins().Find(key.substr(7))) return PluginCommandItem(*pc, L"");
    }
    return std::nullopt;
}

ResultItem SearchService::AppItem(const AppEntry& app) const {
    ResultItem item;
    item.title = app.name;
    item.subtitle = app.subtitle;
    item.accessory = L"應用程式";
    item.usageKey = app.key;
    item.icon = app.icon ? GIconOf(app.icon.get()) : SymbolicIcon(icon::kApp, tint::kSystem);
    if (!app.desktopId.empty()) {
        const std::string id = app.desktopId;
        item.actions.push_back(Action{L"開啟", icon::kOpen, Keys(GDK_KEY_Return), false, [id](LauncherHost& host) {
                                          std::wstring error;
                                          return Report(host, LaunchApp(id, "", &error), error);
                                      }});
        // Desktop actions ("開新視窗", "新增無痕視窗"...), like a jump list.
        for (const auto& [actionId, label] : app.actions) {
            const std::string action = actionId;
            Action a{label, icon::kWindows, Shortcut{}, false, [id, action](LauncherHost& host) {
                         std::wstring error;
                         return Report(host, LaunchApp(id, action, &error), error);
                     }};
            if (item.actions.size() == 1) a.shortcut = Keys(GDK_KEY_Return, true);
            item.actions.push_back(std::move(a));
        }
        if (!app.commandLine.empty()) item.actions.push_back(CopyAction(L"複製指令", app.commandLine));
        if (!app.desktopFile.empty()) item.actions.push_back(RevealAction(Utf8ToWide(app.desktopFile)));
    } else {
        const std::wstring program = app.program;
        item.actions.push_back(Action{L"執行", icon::kRun, Keys(GDK_KEY_Return), false, [program](LauncherHost& host) {
                                          std::wstring error;
                                          return Report(host, SpawnWithActivation({U8(program)}, ParentOf(program), &error),
                                                        error);
                                      }});
        item.actions.push_back(Action{L"在終端機執行", icon::kTerminal, Keys(GDK_KEY_Return, true), false,
                                      [program](LauncherHost& host) {
                                          std::wstring error;
                                          gchar* quoted = g_shell_quote(U8(program).c_str());
                                          const bool ok = RunInTerminal(Utf8ToWide(quoted), false, ParentOf(program), &error);
                                          g_free(quoted);
                                          return Report(host, ok, error);
                                      }});
        item.actions.push_back(RevealAction(program));
        item.actions.push_back(CopyAction(L"複製路徑", program));
    }
    return item;
}

ResultItem SearchService::CommandItem(const Command& cmd) const {
    ResultItem item;
    item.title = cmd.title;
    item.subtitle = cmd.subtitle;
    item.accessory = L"指令";
    item.usageKey = cmd.key;
    item.icon = SymbolicIcon(cmd.icon, cmd.tint);
    item.actions.push_back(Action{cmd.dangerous ? cmd.title : L"執行", icon::kOpen, Keys(GDK_KEY_Return), cmd.dangerous,
                                  cmd.run});
    return item;
}

ResultItem SearchService::WindowItem(const WindowInfo& w) const {
    ResultItem item;
    item.title = w.title;
    item.subtitle = w.appName;
    item.accessory = L"視窗";
    item.icon = w.icon ? GIconOf(w.icon.get()) : SymbolicIcon(icon::kWindows, tint::kWindow);
    const std::string id = w.id;
    App* app = &app_;
    item.actions.push_back(Action{L"切換到此視窗", icon::kWindows, Keys(GDK_KEY_Return), false,
                                  [app, id](LauncherHost& host) {
                                      if (app->windowService().Activate(id)) return Outcome::Hide;
                                      host.ShowStatus(L"無法切換到這個視窗（可能已關閉）", true);
                                      host.RefreshWindows();
                                      return Outcome::Stay;
                                  }});
    item.actions.push_back(Action{L"關閉視窗", icon::kClose, Keys(GDK_KEY_w, true), false, [app, id](LauncherHost& host) {
                                      app->windowService().Close(id);
                                      host.RefreshWindows();
                                      return Outcome::Stay;
                                  }});
    item.actions.push_back(CopyAction(L"複製視窗標題", w.title));
    return item;
}

ResultItem SearchService::QuicklinkItem(const Quicklink& link, const std::wstring& argument) const {
    const bool isUrl = link.target.find(L"://") != std::wstring::npos || StartsWithIgnoreCase(link.target, L"mailto:");
    const bool needsQuery = HasQueryPlaceholder(link.target) || HasQueryPlaceholder(link.arguments);
    ResultItem item;
    item.title = argument.empty() ? link.name : link.name + L"：" + argument;
    if (!link.keyword.empty()) item.subtitle = needsQuery ? link.keyword + L" <搜尋內容>" : link.keyword;
    else item.subtitle = isUrl ? HostOf(link.target) : link.target;
    item.accessory = L"快速連結";
    item.usageKey = L"ql:" + link.name;
    if (!link.keyword.empty()) item.autocomplete = link.keyword + L" ";
    const std::wstring target = FillTemplate(link.target, argument, isUrl);
    // ~ and $VAR are expanded in what the user configured, never in the typed query.
    const std::wstring expanded = isUrl ? std::wstring() : FillTemplate(ExpandUserPath(link.target), argument, false);
    // Network mounts are never touched on the UI thread (a dead NFS server would freeze Beam).
    const bool remote = !expanded.empty() && expanded[0] == L'/' && IsRemotePath(expanded);
    // A program (absolute executable or a name on PATH) runs with the arguments; anything else opens.
    const bool program = !isUrl && !remote &&
                         ((expanded.find(L'/') != std::wstring::npos && IsExecutableFile(expanded)) ||
                          (expanded.find(L'/') == std::wstring::npos && !FindProgram(expanded).empty()));
    const std::wstring argumentsTemplate = link.arguments;
    if (isUrl) item.icon = SymbolicIcon(icon::kGlobe, tint::kQuicklink);
    else if (!program && !remote && !expanded.empty() && expanded[0] == L'/')
        item.icon = FileIcon(expanded, DirectoryExists(expanded));
    else item.icon = SymbolicIcon(program ? icon::kRun : icon::kLink, tint::kQuicklink);
    const std::wstring keyword = link.keyword;
    item.actions.push_back(Action{L"開啟", icon::kOpen, Keys(GDK_KEY_Return), false, [=](LauncherHost& host) {
                                      if (needsQuery && Trim(argument).empty()) {
                                          if (!keyword.empty()) {
                                              host.SetQuery(keyword + L" ");
                                              return Outcome::Stay;
                                          }
                                          host.ShowStatus(L"這個快速連結需要 keyword 才能輸入搜尋內容", true);
                                          return Outcome::Stay;
                                      }
                                      if (program) {
                                          std::wstring error;
                                          return Report(host, RunProgram(expanded, argumentsTemplate, argument, &error), error);
                                      }
                                      return OpenOrReport(host, isUrl ? target : expanded);
                                  }});
    if (!needsQuery || !argument.empty()) item.actions.push_back(CopyAction(isUrl ? L"複製網址" : L"複製路徑", target));
    return item;
}

ResultItem SearchService::CalculatorItem(const std::wstring& expression, double value) const {
    ResultItem item;
    const std::wstring plain = FormatNumber(value);
    const std::wstring programmer = FormatProgrammer(value);
    item.title = L"= " + FormatNumberGrouped(value);
    item.subtitle = programmer.empty() ? expression : expression + L"   " + programmer;
    item.accessory = L"計算機";
    item.icon = SymbolicIcon(icon::kCalculator, tint::kCalc);
    item.actions.push_back(CopyAction(L"複製結果", plain, Keys(GDK_KEY_Return)));
    item.actions.push_back(CopyAction(L"複製算式與結果", expression + L" = " + plain));
    if (!programmer.empty())
        item.actions.push_back(CopyAction(L"複製十六進位", programmer.substr(0, programmer.find(L' ')), Shortcut{}));
    return item;
}

ResultItem SearchService::UrlItem(const std::wstring& url) const {
    ResultItem item;
    item.title = L"開啟 " + url;
    item.accessory = L"網址";
    item.icon = SymbolicIcon(icon::kGlobe, tint::kWeb);
    item.actions.push_back(Action{L"在瀏覽器開啟", icon::kOpen, Keys(GDK_KEY_Return), false,
                                  [url](LauncherHost& host) { return OpenOrReport(host, url); }});
    item.actions.push_back(CopyAction(L"複製網址", url));
    return item;
}

ResultItem SearchService::WebSearchItem(const std::wstring& q) const {
    const std::wstring engine = app_.settings().searchEngineUrl;
    const std::wstring url = FillTemplate(engine, q, true);
    ResultItem item;
    item.title = L"搜尋「" + q + L"」";
    item.subtitle = HostOf(engine);
    item.accessory = L"網路搜尋";
    item.icon = SymbolicIcon(icon::kSearch, tint::kWeb);
    item.actions.push_back(Action{L"在瀏覽器搜尋", icon::kSearch, Keys(GDK_KEY_Return), false,
                                  [url](LauncherHost& host) { return OpenOrReport(host, url); }});
    item.actions.push_back(CopyAction(L"複製搜尋網址", url));
    return item;
}

ResultItem SearchService::ShellCommandItem(const std::wstring& command) const {
    ResultItem item;
    item.accessory = L"Shell";
    item.icon = SymbolicIcon(icon::kTerminal, tint::kShell);
    if (command.empty()) {
        item.title = L"輸入要執行的指令，例如 > htop";
        item.subtitle = L"Enter：在終端機執行　Ctrl+Enter：在背景執行";
        item.actions.push_back(Action{L"執行", icon::kTerminal, Keys(GDK_KEY_Return), false,
                                      [](LauncherHost&) { return Outcome::Stay; }});
        return item;
    }
    item.title = L"執行：" + command;
    item.subtitle = L"終端機（bash）";
    item.actions.push_back(Action{L"在終端機執行", icon::kTerminal, Keys(GDK_KEY_Return), false,
                                  [command](LauncherHost& host) {
                                      std::wstring error;
                                      return Report(host, RunInTerminal(command, false, HomeDir(), &error), error);
                                  }});
    item.actions.push_back(Action{L"在背景執行", icon::kRun, Keys(GDK_KEY_Return, true), false,
                                  [command](LauncherHost& host) {
                                      std::wstring error;
                                      return Report(host, RunInBackground(command, &error), error);
                                  }});
    item.actions.push_back(Action{L"以 sudo 在終端機執行", icon::kAdmin, Keys(GDK_KEY_Return, true, true), false,
                                  [command](LauncherHost& host) {
                                      std::wstring error;
                                      return Report(host, RunInTerminal(command, true, HomeDir(), &error), error);
                                  }});
    item.actions.push_back(CopyAction(L"複製指令", command));
    return item;
}

ResultItem SearchService::PathItem(const std::wstring& path, bool directory) const {
    ResultItem item;
    const std::wstring name = FileNameOf(path);
    item.title = name.empty() || path == L"/" ? path : name;
    item.subtitle = path == L"/" ? L"" : ParentOf(path);
    item.accessory = directory ? L"資料夾" : L"檔案";
    item.icon = FileIcon(path, directory);
    if (directory) {
        item.autocomplete = (!path.empty() && path.back() == L'/') ? path : path + L"/";
        item.actions.push_back(Action{L"開啟資料夾", icon::kFolderOpen, Keys(GDK_KEY_Return), false,
                                      [path](LauncherHost& host) { return OpenOrReport(host, path); }});
        item.actions.push_back(Action{L"在終端機開啟", icon::kTerminal, Keys(GDK_KEY_Return, true), false,
                                      [path](LauncherHost& host) {
                                          std::wstring error;
                                          return Report(host, OpenTerminalAt(path, &error), error);
                                      }});
    } else {
        item.actions.push_back(Action{L"開啟", icon::kOpen, Keys(GDK_KEY_Return), false,
                                      [path](LauncherHost& host) { return OpenOrReport(host, path); }});
        if (!IsRemotePath(path) && IsExecutableFile(path)) {
            item.actions.push_back(Action{L"執行", icon::kRun, Keys(GDK_KEY_Return, true), false,
                                          [path](LauncherHost& host) {
                                              std::wstring error;
                                              return Report(host, SpawnWithActivation({U8(path)}, ParentOf(path), &error),
                                                            error);
                                          }});
        }
    }
    if (path != L"/") item.actions.push_back(RevealAction(path));
    item.actions.push_back(CopyAction(L"複製路徑", path));
    return item;
}

std::vector<ResultItem> SearchService::PathResults(const std::wstring& q) const {
    std::wstring expanded = ExpandUserPath(q);
    std::vector<ResultItem> items;
    if (IsRemotePath(expanded)) {  // never block the UI on a network round trip
        items.push_back(PathItem(expanded, true));
        return items;
    }
    const bool endsWithSeparator = !expanded.empty() && expanded.back() == L'/';
    const std::wstring dir = endsWithSeparator ? expanded : ParentOf(expanded);
    const std::wstring prefix = endsWithSeparator ? L"" : FileNameOf(expanded);
    if (!endsWithSeparator && FileExists(expanded)) items.push_back(PathItem(expanded, DirectoryExists(expanded)));
    if (dir.empty() || !DirectoryExists(dir)) return items;

    struct Child {
        std::wstring name;
        bool directory;
        int rank;
    };
    std::vector<Child> children;
    std::error_code ec;
    int scanned = 0;
    for (std::filesystem::directory_iterator it(U8(dir), ec), end; !ec && it != end && scanned < 2000;
         it.increment(ec), ++scanned) {
        const std::wstring name = Utf8ToWide(it->path().filename().string());
        if (name.empty() || (name[0] == L'.' && (prefix.empty() || prefix[0] != L'.'))) continue;  // hidden files
        int rank = 0;
        if (!prefix.empty()) {
            if (StartsWithIgnoreCase(name, prefix)) rank = 0;
            else if (FindIgnoreCase(name, prefix) != std::wstring::npos) rank = 1;
            else continue;
            if (name == prefix) continue;  // already shown as the exact item
        }
        std::error_code e2;
        children.push_back({name, it->is_directory(e2), rank});
    }
    std::sort(children.begin(), children.end(), [](const Child& a, const Child& b) {
        if (a.rank != b.rank) return a.rank < b.rank;
        if (a.directory != b.directory) return a.directory;
        return Fold(a.name) < Fold(b.name);
    });
    if (children.size() > 30) children.resize(30);
    const std::wstring base = dir.back() == L'/' ? dir : dir + L"/";
    for (const auto& c : children) items.push_back(PathItem(base + c.name, c.directory));
    return items;
}

// ---------------------------------------------------------------------------
// Plugins

ResultItem SearchService::PluginCommandItem(const PluginCommand& pc, const std::wstring& argument) const {
    const bool list = pc.spec.mode == PluginMode::List;
    ResultItem item;
    item.title = argument.empty() ? pc.spec.name : pc.spec.name + L"：" + argument;
    item.subtitle = pc.spec.description;
    if (item.subtitle.empty() && !pc.spec.keyword.empty())
        item.subtitle = list ? pc.spec.keyword + L" <搜尋內容>" : pc.spec.keyword;
    item.accessory = L"外掛";
    item.usageKey = L"plugin:" + pc.spec.id;
    if (!pc.spec.keyword.empty()) item.autocomplete = pc.spec.keyword + L" ";
    item.icon = PluginIcon(pc, pc.spec.icon);
    const std::wstring id = pc.spec.id;
    if (list) {
        item.actions.push_back(Action{L"開啟", icon::kOpen, Keys(GDK_KEY_Return), false, [id, argument](LauncherHost& host) {
                                          host.EnterPlugin(id, argument);
                                          return Outcome::Stay;
                                      }});
    } else {
        item.actions.push_back(Action{L"執行", icon::kPlay, Keys(GDK_KEY_Return), false, [id, argument](LauncherHost& host) {
                                          host.RunPluginCommand(id, argument);
                                          return Outcome::Stay;
                                      }});
    }
    App* app = &app_;
    const std::wstring manifest = pc.manifestPath;
    item.actions.push_back(Action{L"編輯 plugin.json", icon::kEdit, Shortcut{}, false,
                                  [manifest](LauncherHost& host) {
                                      std::wstring error;
                                      return Report(host, OpenInTextEditor(manifest, &error), error);
                                  }});
    item.actions.push_back(RevealAction(manifest));
    item.actions.push_back(Action{L"外掛偵錯紀錄", icon::kMonitor, Shortcut{}, false, [app](LauncherHost& host) {
                                      host.ShowDetail(L"外掛偵錯紀錄", app->plugins().DescribeRecords());
                                      return Outcome::Stay;
                                  }});
    return item;
}

std::vector<ResultItem> SearchService::NewPluginItems(const std::wstring& name) const {
    App* app = &app_;
    std::vector<ResultItem> items;
    for (const PluginTemplate language : PlatformPluginTemplates()) {
        const wchar_t* note = language == PluginTemplate::Python ? L"需要 python3（Ubuntu 內建）"
                              : language == PluginTemplate::Node ? L"需要安裝 Node.js（sudo apt install nodejs）"
                                                                 : L"bash，不用另外安裝";
        ResultItem item;
        item.title = std::wstring(L"建立 ") + PluginTemplateLabel(language) + L" 外掛" + (name.empty() ? L"" : L"「" + name + L"」");
        item.subtitle = name.empty() ? L"接著輸入外掛名稱，例如：newplugin 匯率查詢" : note;
        item.accessory = L"範本";
        item.section = kSectionNewPlugin;
        item.icon = SymbolicIcon(icon::kPuzzle, tint::kPlugin);
        item.actions.push_back(Action{L"建立外掛", icon::kAdd, Keys(GDK_KEY_Return), false, [app, name, language](LauncherHost& host) {
            if (name.empty()) {
                host.ShowStatus(L"請先輸入外掛名稱，例如：newplugin 匯率查詢", true);
                return Outcome::Stay;
            }
            std::wstring folder, keyword, error;
            if (!app->plugins().CreateFromTemplate(name, language, &folder, &keyword, &error)) {
                host.ShowStatus(L"無法建立外掛：" + error, true);
                return Outcome::Stay;
            }
            app->ReloadPlugins();
            const std::wstring script = PluginScriptFileName(language);
            RevealInFileManager(folder + L"/" + script);
            app->Notify(L"外掛已建立：" + FileNameOf(folder),
                        L"在 Beam 輸入「" + keyword + L" 」（加一個空白）試試看。修改 " + script + L" 存檔後立即生效。");
            return Outcome::Hide;
        }});
        items.push_back(std::move(item));
    }
    return items;
}

ItemIcon SearchService::PluginIcon(const PluginCommand& cmd, const std::wstring& spec) const {
    if (spec.empty()) return SymbolicIcon(icon::kPuzzle, tint::kPlugin);
    if (const auto g = ParseGlyphSpec(spec)) return SymbolicIcon(GlyphToIcon(g->glyph[0]), g->color ? g->color : tint::kPlugin);
    if (StartsWithIgnoreCase(spec, L"icon:")) {  // "icon:<freedesktop name>[:#RRGGBB]"
        std::wstring name = spec.substr(5);
        uint32_t color = tint::kPlugin;
        if (const size_t colon = name.find(L':'); colon != std::wstring::npos) {
            if (const auto c = ParseColor(name.substr(colon + 1))) color = *c;
            name.resize(colon);
        }
        if (EndsWithIgnoreCase(name, L"-symbolic")) {
            ItemIcon i;
            i.type = IconType::Symbolic;
            i.name = U8(name);
            i.tint = color;
            return i;
        }
        GIcon* themed = g_themed_icon_new(U8(name).c_str());
        ItemIcon i = GIconOf(themed);
        g_object_unref(themed);
        return i;
    }
    std::wstring path = ExpandUserPath(spec);
    if (path.empty() || path[0] != L'/') path = cmd.folder + L"/" + path;
    ItemIcon i;
    i.type = IconType::File;
    i.name = U8(path);
    return i;
}

Action SearchService::PluginActionFor(const PluginCommand& cmd, const Json* item, const PluginAction& a) const {
    Action action;
    action.title = a.title.empty() ? DefaultActionTitle(a.kind) : a.title;
    action.shortcut = ParseShortcut(a.shortcut);
    const std::wstring folder = cmd.folder;
    const std::wstring value = a.value;
    const bool keepOpen = a.keepOpen.value_or(false);
    switch (a.kind) {
        case PluginActionKind::Open:
            action.icon = icon::kOpen;
            action.run = [folder, value, keepOpen](LauncherHost& host) {
                std::wstring target = value;
                // A path relative to the plugin folder ("docs/help.html"); URLs, ~ and $VAR paths as they are.
                if (target.find(L':') == std::wstring::npos && !target.empty() && target[0] != L'/' && target[0] != L'~' &&
                    target[0] != L'$' && FileExists(folder + L"/" + target))
                    target = folder + L"/" + target;
                const Outcome outcome = OpenOrReport(host, target);
                return outcome == Outcome::Hide && keepOpen ? Outcome::Stay : outcome;
            };
            break;
        case PluginActionKind::Copy:
            action.icon = icon::kCopy;
            action.run = [value, keepOpen](LauncherHost& host) {
                CopyText(value);
                if (!keepOpen) return Outcome::Hide;
                host.ShowStatus(L"已複製到剪貼簿", false);
                return Outcome::Stay;
            };
            break;
        case PluginActionKind::Run: {
            action.icon = icon::kTerminal;
            const std::wstring cwd = a.cwd.empty() ? folder : ExpandUserPath(a.cwd);
            const bool terminal = a.terminal;
            action.run = [value, cwd, terminal, keepOpen](LauncherHost& host) {
                std::wstring error;
                bool ok = false;
                if (terminal) {
                    ok = RunInTerminal(value, false, cwd, &error);
                } else {
                    std::vector<std::string> argv;
                    ok = ResolvePluginArgv(value, cwd, &argv, &error) && SpawnWithActivation(argv, cwd, &error);
                }
                if (!ok) {
                    host.ShowStatus(L"無法執行：" + error, true);
                    return Outcome::Stay;
                }
                return keepOpen ? Outcome::Stay : Outcome::Hide;
            };
            break;
        }
        case PluginActionKind::SetQuery:
            action.icon = icon::kSearch;
            action.run = [value](LauncherHost& host) {
                host.SetQuery(value);
                return Outcome::Stay;
            };
            break;
        case PluginActionKind::Callback: {
            action.icon = icon::kPlay;
            const std::wstring id = cmd.spec.id;
            const Json itemJson = item ? *item : Json();
            const Json actionJson = a.raw;
            action.run = [id, itemJson, actionJson](LauncherHost& host) {
                host.PluginCallback(id, itemJson, actionJson);
                return Outcome::Stay;
            };
            break;
        }
        default:
            break;
    }
    return action;
}

std::vector<ResultItem> SearchService::PluginItems(const PluginCommand& cmd, const PluginResponse& response) const {
    std::vector<ResultItem> items;
    items.reserve(response.items.size());
    for (const auto& it : response.items) {
        ResultItem item;
        item.title = it.title;
        item.subtitle = it.subtitle;
        item.accessory = it.accessory;
        item.autocomplete = it.autocomplete;
        item.section = it.section.empty() ? std::wstring(kSectionResults) : it.section;
        item.icon = PluginIcon(cmd, it.icon.empty() ? cmd.spec.icon : it.icon);
        // Like Raycast: the second action gets Ctrl+Enter unless the plugin assigned it elsewhere.
        bool ctrlEnterTaken = false;
        for (const auto& a : it.actions) {
            const Shortcut s = ParseShortcut(a.shortcut);
            if (s.keyval == GDK_KEY_Return && s.ctrl && !s.shift && !s.alt) ctrlEnterTaken = true;
        }
        for (size_t i = 0; i < it.actions.size(); ++i) {
            Action action = PluginActionFor(cmd, &it.raw, it.actions[i]);
            if (!action.run) continue;
            if (item.actions.empty()) {
                action.alternate = action.shortcut;  // Enter always runs the first action
                action.shortcut = Keys(GDK_KEY_Return);
            } else if (item.actions.size() == 1 && action.shortcut.Empty() && !ctrlEnterTaken) {
                action.shortcut = Keys(GDK_KEY_Return, true);
            }
            item.actions.push_back(std::move(action));
        }
        items.push_back(std::move(item));
    }
    return items;
}

ResultItem SearchService::PluginErrorItem(const PluginCommand& cmd, const std::wstring& failure) const {
    ResultItem item;
    const size_t newline = failure.find_first_of(L"\r\n");
    item.title = Shorten(newline == std::wstring::npos ? failure : failure.substr(0, newline), 200);
    item.subtitle = cmd.spec.name;
    item.accessory = L"錯誤";
    item.section = L"外掛發生錯誤";
    item.icon = SymbolicIcon(icon::kWarning, tint::kDanger);
    App* app = &app_;
    item.actions.push_back(Action{L"檢視偵錯紀錄", icon::kMonitor, Keys(GDK_KEY_Return), false, [app](LauncherHost& host) {
                                      host.ShowDetail(L"外掛偵錯紀錄", app->plugins().DescribeRecords());
                                      return Outcome::Stay;
                                  }});
    const std::wstring manifest = cmd.manifestPath;
    item.actions.push_back(Action{L"編輯 plugin.json", icon::kEdit, Shortcut{}, false,
                                  [manifest](LauncherHost& host) {
                                      std::wstring error;
                                      return Report(host, OpenInTextEditor(manifest, &error), error);
                                  }});
    item.actions.push_back(RevealAction(manifest));
    return item;
}

}  // namespace beam
