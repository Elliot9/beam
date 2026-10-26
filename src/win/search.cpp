#include "win/search.hpp"

#include <algorithm>
#include <ctime>

#include "core/calc.hpp"
#include "core/favorites.hpp"
#include "core/fuzzy.hpp"
#include "core/hotkey.hpp"
#include "core/query.hpp"
#include "core/text.hpp"
#include "win/app.hpp"
#include "win/shell.hpp"

namespace beam {

std::vector<std::wstring> Shortcut::Parts() const {
    std::vector<std::wstring> parts;
    if (ctrl) parts.emplace_back(L"Ctrl");
    if (shift) parts.emplace_back(L"Shift");
    if (alt) parts.emplace_back(L"Alt");
    switch (vk) {
        case VK_UP: parts.emplace_back(L"\u2191"); break;
        case VK_DOWN: parts.emplace_back(L"\u2193"); break;
        case VK_LEFT: parts.emplace_back(L"\u2190"); break;
        case VK_RIGHT: parts.emplace_back(L"\u2192"); break;
        default:
            if (const wchar_t* name = VirtualKeyName(vk)) parts.emplace_back(name);
            break;
    }
    return parts;
}

namespace {

constexpr wchar_t kSectionResults[] = L"結果";
constexpr wchar_t kSectionFavorites[] = L"最愛";
constexpr wchar_t kSectionNewPlugin[] = L"建立新外掛";
constexpr wchar_t kSectionRecent[] = L"最近使用";
constexpr wchar_t kSectionSuggestions[] = L"建議";
constexpr wchar_t kSectionWindows[] = L"已開啟的視窗";
constexpr wchar_t kSectionWeb[] = L"網路搜尋";

int64_t Now() { return static_cast<int64_t>(std::time(nullptr)); }

ItemIcon GlyphIcon(const wchar_t* g, uint32_t color) { return ItemIcon{IconType::Glyph, g, color}; }
ItemIcon ShellIcon(const std::wstring& path) { return ItemIcon{IconType::Shell, path, 0}; }

Outcome OpenOrReport(LauncherHost& host, const std::wstring& target, const LaunchOptions& options = {}) {
    std::wstring error;
    if (ShellOpen(target, options, &error) || error.empty()) return Outcome::Hide;
    host.ShowStatus(L"無法開啟：" + error, true);
    return Outcome::Stay;
}

Action CopyAction(std::wstring title, std::wstring text, Shortcut shortcut = Keys('C', true, true)) {
    return Action{std::move(title), glyph::kCopy, shortcut, false, [text = std::move(text)](LauncherHost& host) {
                      CopyToClipboard(host.Window(), text);
                      return Outcome::Hide;
                  }};
}

Action RevealAction(std::wstring path) {
    return Action{L"在檔案總管中顯示", glyph::kFolderOpen, Keys('E', true, true), false,
                  [path = std::move(path)](LauncherHost&) {
                      RevealInExplorer(path);
                      return Outcome::Hide;
                  }};
}

bool IsExecutableExtension(const std::wstring& ext) {
    return ext == L".exe" || ext == L".bat" || ext == L".cmd" || ext == L".msi" || ext == L".lnk" || ext == L".msc";
}

std::wstring HostOf(const std::wstring& url) {
    size_t start = url.find(L"://");
    start = start == std::wstring::npos ? 0 : start + 3;
    size_t end = url.find_first_of(L"/?#", start);
    std::wstring host = url.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
    if (StartsWithIgnoreCase(host, L"www.")) host = host.substr(4);
    return host;
}

bool CanBeFavorite(const std::wstring& key) {
    return key.rfind(L"app:", 0) == 0 || key.rfind(L"cmd:", 0) == 0 || key.rfind(L"ql:", 0) == 0 ||
           key.rfind(L"plugin:", 0) == 0;
}

bool IsAbsolute(const std::wstring& p) {
    return p.size() > 2 && ((p[1] == L':' && p[2] == L'\\') || (p[0] == L'\\' && p[1] == L'\\'));
}

// Quicklink arguments as on Linux: split first, then {query} goes into each argument, so the typed
// text stays inside its argument even with spaces or quotes ("D:\src\{query}" -> "D:\src\my app").
std::wstring FillArguments(const std::wstring& arguments, const std::wstring& query) {
    if (Trim(arguments).empty()) return arguments;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW((L"x " + arguments).c_str(), &argc);  // argv[0] has its own rules
    if (!argv) return FillTemplate(arguments, query, false);
    std::wstring out;
    for (int i = 1; i < argc; ++i) {
        if (!out.empty()) out.push_back(L' ');
        out += QuoteArgument(FillTemplate(argv[i], query, false));
    }
    LocalFree(argv);
    return out;
}

// Moves a favorite one visible step: until it passes `neighbor` (hidden entries, e.g.
// apps not indexed yet, are skipped over so every key press visibly moves the row).
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

void SortByScore(std::vector<ResultItem>& items) {
    std::stable_sort(items.begin(), items.end(), [](const ResultItem& a, const ResultItem& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.title.size() < b.title.size();
    });
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
                      const wchar_t* g, uint32_t color, bool dangerous, std::function<Outcome(LauncherHost&)> run) {
        commands_.push_back(Command{L"cmd:" + id, std::move(title), std::move(subtitle), std::move(aliases), g, color,
                                    dangerous, std::move(run)});
    };
    auto open = [](std::wstring target, std::wstring args = L"") {
        return [target = std::move(target), args = std::move(args)](LauncherHost& host) {
            return OpenOrReport(host, target, {args});
        };
    };

    add(L"switch-windows", L"切換視窗", L"搜尋並切換到已開啟的視窗",
        {L"switch windows", L"windows", L"alt tab", L"window switcher"}, glyph::kTaskView, tint::kWindow, false,
        [](LauncherHost& host) {
            host.EnterMode(Mode::Windows);
            return Outcome::Stay;
        });
    add(L"settings", L"Beam 設定", L"快捷鍵、開機啟動、主題",
        {L"beam settings", L"preferences", L"hotkey", L"快捷鍵", L"config"}, glyph::kSettings, tint::kBeam, false,
        [app](LauncherHost&) {
            app->Defer([app] { app->ShowSettings(); });
            return Outcome::Hide;
        });
    add(L"edit-config", L"編輯 Beam 設定檔", L"settings.json：快速連結、工具資料夾",
        {L"settings.json", L"quicklinks", L"extra folders", L"edit config"}, glyph::kEdit, tint::kBeam, false,
        [app](LauncherHost&) {
            app->OpenSettingsFile();
            return Outcome::Hide;
        });
    add(L"reindex", L"重新建立應用程式索引", L"重新掃描開始功能表與工具資料夾",
        {L"reload", L"reindex", L"refresh apps", L"rescan"}, glyph::kRefresh, tint::kBeam, false,
        [app](LauncherHost& host) {
            app->Reindex();
            host.ShowStatus(L"正在重新建立索引…", false);
            return Outcome::Stay;
        });
    add(L"quit", L"結束 Beam", L"關閉 Beam（快捷鍵將停用）", {L"quit beam", L"exit beam"}, glyph::kClose,
        tint::kBeam, false, [app](LauncherHost&) {
            app->Defer([app] { app->Quit(); });
            return Outcome::Hide;
        });

    add(L"new-plugin", L"建立新外掛", L"從 Python / Node.js / PowerShell 範本開始",
        {L"new plugin", L"create plugin", L"newplugin", L"plugin template", L"extension", L"建立外掛"}, glyph::kPuzzle,
        tint::kPlugin, false, [](LauncherHost& host) {
            host.SetQuery(L"newplugin ");
            return Outcome::Stay;
        });
    add(L"reload-plugins", L"重新載入外掛", L"重新讀取外掛資料夾裡的 plugin.json",
        {L"reload plugins", L"refresh plugins", L"rescan plugins"}, glyph::kRefresh, tint::kPlugin, false,
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
        {L"plugin log", L"plugin debug", L"plugin output", L"debug plugins", L"外掛輸出"}, glyph::kDiagnostic,
        tint::kPlugin, false, [app](LauncherHost& host) {
            host.ShowDetail(L"外掛偵錯紀錄", app->plugins().DescribeRecords());
            return Outcome::Stay;
        });
    add(L"plugin-folder", L"開啟外掛資料夾", L"%APPDATA%\\Beam\\plugins",
        {L"plugins folder", L"open plugins", L"plugin folder"}, glyph::kFolderOpen, tint::kPlugin, false,
        [app](LauncherHost& host) { return OpenOrReport(host, app->plugins().Directory()); });

    add(L"lock", L"鎖定電腦", L"", {L"lock screen", L"lock"}, glyph::kLock, tint::kSystem, false,
        [app](LauncherHost&) {
            app->Defer(LockComputer);
            return Outcome::Hide;
        });
    add(L"sleep", L"睡眠", L"", {L"sleep", L"suspend"}, glyph::kSleep, tint::kSystem, false, [app](LauncherHost&) {
        app->Defer(SleepComputer);
        return Outcome::Hide;
    });
    add(L"shutdown", L"關機", L"", {L"shut down", L"shutdown", L"power off"}, glyph::kPower, tint::kDanger, true,
        [app](LauncherHost&) {
            app->Defer([] { ShutdownComputer(false); });
            return Outcome::Hide;
        });
    add(L"restart", L"重新啟動", L"", {L"restart", L"reboot"}, glyph::kRestart, tint::kDanger, true,
        [app](LauncherHost&) {
            app->Defer([] { ShutdownComputer(true); });
            return Outcome::Hide;
        });
    add(L"signout", L"登出", L"", {L"sign out", L"log off", L"logout"}, glyph::kSignOut, tint::kDanger, true,
        [app](LauncherHost&) {
            app->Defer(SignOut);
            return Outcome::Hide;
        });
    add(L"empty-bin", L"清空資源回收筒", L"", {L"empty recycle bin", L"empty trash"}, glyph::kDelete, tint::kDanger,
        true, [app](LauncherHost&) {
            app->Defer(EmptyRecycleBin);
            return Outcome::Hide;
        });
    add(L"recycle-bin", L"資源回收筒", L"", {L"recycle bin", L"trash"}, glyph::kDelete, tint::kSystem, false,
        open(L"shell:RecycleBinFolder"));
    add(L"windows-settings", L"Windows 設定", L"", {L"settings", L"windows settings", L"ms-settings"},
        glyph::kSettings, tint::kSettings, false, open(L"ms-settings:"));
    add(L"task-manager", L"工作管理員", L"", {L"task manager", L"taskmgr"}, glyph::kDiagnostic, tint::kSettings,
        false, open(L"taskmgr.exe"));
    add(L"control-panel", L"控制台", L"", {L"control panel"}, glyph::kSettings, tint::kSettings, false,
        open(L"control.exe"));
    add(L"env-vars", L"編輯環境變數", L"使用者環境變數 / PATH", {L"environment variables", L"env", L"path"},
        glyph::kCode, tint::kShell, false, open(L"rundll32.exe", L"sysdm.cpl,EditEnvironmentVariables"));
    add(L"device-manager", L"裝置管理員", L"", {L"device manager", L"devmgmt"}, glyph::kDevices, tint::kSettings,
        false, open(L"devmgmt.msc"));
    add(L"network-connections", L"網路連線", L"網路介面卡", {L"network connections", L"ncpa", L"adapters"},
        glyph::kNetwork, tint::kSettings, false, open(L"ncpa.cpl"));
    add(L"programs", L"程式和功能", L"解除安裝或變更程式", {L"programs and features", L"appwiz", L"uninstall"},
        glyph::kApps, tint::kSettings, false, open(L"appwiz.cpl"));
}

std::vector<ResultItem> SearchService::Search(const std::wstring& raw, Mode mode,
                                              const std::vector<WindowInfo>& windows) const {
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
    if (LooksLikePath(q)) {
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
        if (appNames_.count(Fold(cmd.title))) continue;  // the Start Menu already has it
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
        for (const auto& w : windows) {
            const int s = BestScore(q, w.title, {w.processName}, 100);
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

std::vector<ResultItem> SearchService::EmptyState(const std::vector<WindowInfo>& windows) const {
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
        // Ctrl+Alt+Up/Down; Ctrl+Shift+Up/Down also works where graphics drivers grab Ctrl+Alt+arrows.
        if (i > 0)
            items[i].actions.push_back(Action{L"上移", glyph::kUp, Keys(VK_UP, true, false, true), false,
                                              move(items[i - 1].usageKey, -1), Keys(VK_UP, true, true), false});
        if (i + 1 < pinned)
            items[i].actions.push_back(Action{L"下移", glyph::kDown, Keys(VK_DOWN, true, false, true), false,
                                              move(items[i + 1].usageKey, 1), Keys(VK_DOWN, true, true), false});
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
            if (cmd.key == L"cmd:switch-windows" || cmd.key == L"cmd:settings" || cmd.key == L"cmd:edit-config" ||
                cmd.key == L"cmd:new-plugin") {
                ResultItem item = CommandItem(cmd);
                item.section = kSectionSuggestions;
                AddFavoriteActions(item, false);
                items.push_back(std::move(item));
            }
        }
    }
    size_t shown = 0;
    for (const auto& w : windows) {
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
    if (pinned && !inFavoritesSection) item.accessory = L"\u2605 " + item.accessory;
    App* app = &app_;
    item.actions.push_back(Action{pinned ? L"從最愛移除" : L"加入最愛", pinned ? glyph::kStarFill : glyph::kStar,
                                  Keys('F', true, true), false, [app, key, pinned](LauncherHost& host) {
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

std::vector<ResultItem> SearchService::WindowResults(const std::wstring& q,
                                                     const std::vector<WindowInfo>& windows) const {
    std::vector<ResultItem> items;
    for (size_t i = 0; i < windows.size(); ++i) {
        int s = 0;
        if (!q.empty()) {
            s = BestScore(q, windows[i].title, {windows[i].processName}, 60);
            if (s == kNoMatch) continue;
        }
        ResultItem item = WindowItem(windows[i]);
        item.section = L"視窗";
        item.score = q.empty() ? -static_cast<double>(i) : s;  // keep Z-order when not filtering
        items.push_back(std::move(item));
    }
    SortByScore(items);
    return items;
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
    item.icon = ShellIcon(app.iconPath);
    const std::wstring dir = (app.kind == AppKind::Program || app.kind == AppKind::Script) && !app.target.empty()
                                 ? ParentOf(app.target)
                                 : std::wstring();
    item.actions.push_back(Action{L"開啟", glyph::kOpen, Keys(VK_RETURN), false,
                                  [launch = app.launch, args = app.arguments, dir](LauncherHost& host) {
                                      return OpenOrReport(host, launch, {args, dir});
                                  }});
    const bool canElevate = app.kind != AppKind::Packaged && app.kind != AppKind::Protocol;
    if (canElevate) {
        item.actions.push_back(Action{L"以系統管理員身分執行", glyph::kAdmin, Keys(VK_RETURN, true, true), false,
                                      [launch = app.launch, args = app.arguments, dir](LauncherHost& host) {
                                          return OpenOrReport(host, launch, {args, dir, true});
                                      }});
    }
    const std::wstring onDisk = !app.target.empty() ? app.target
                                : app.launch.rfind(L"shell:", 0) == 0 ? std::wstring()
                                                                       : app.launch;
    if (!onDisk.empty()) {
        item.actions.push_back(RevealAction(onDisk));
        item.actions.push_back(CopyAction(L"複製路徑", onDisk));
    }
    return item;
}

ResultItem SearchService::CommandItem(const Command& cmd) const {
    ResultItem item;
    item.title = cmd.title;
    item.subtitle = cmd.subtitle;
    item.accessory = L"指令";
    item.usageKey = cmd.key;
    item.icon = GlyphIcon(cmd.glyph, cmd.tint);
    item.actions.push_back(Action{cmd.dangerous ? cmd.title : L"執行", glyph::kOpen, Keys(VK_RETURN), cmd.dangerous,
                                  cmd.run});
    return item;
}

ResultItem SearchService::WindowItem(const WindowInfo& w) const {
    ResultItem item;
    item.title = w.title;
    item.subtitle = w.processName;
    item.accessory = L"視窗";
    item.icon = w.exePath.empty() ? GlyphIcon(glyph::kTaskView, tint::kWindow) : ShellIcon(w.exePath);
    const HWND hwnd = w.hwnd;
    item.actions.push_back(Action{L"切換到此視窗", glyph::kTaskView, Keys(VK_RETURN), false, [hwnd](LauncherHost& host) {
                                      if (ActivateWindow(hwnd)) return Outcome::Hide;
                                      host.ShowStatus(L"無法切換到這個視窗（可能已關閉）", true);
                                      host.RefreshWindows();
                                      return Outcome::Stay;
                                  }});
    item.actions.push_back(Action{L"關閉視窗", glyph::kClose, Keys('W', true), false, [hwnd](LauncherHost& host) {
                                      CloseWindowGracefully(hwnd);
                                      host.RefreshWindows();
                                      return Outcome::Stay;
                                  }});
    if (!w.exePath.empty()) item.actions.push_back(RevealAction(w.exePath));
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
    if (isUrl) {
        item.icon = GlyphIcon(glyph::kGlobe, tint::kQuicklink);
    } else {
        // Only absolute paths get a shell icon; the icon worker deals with missing files.
        const std::wstring path = ExpandUserPath(link.target);
        const bool absolute = path.size() > 2 && (path[1] == L':' || (path[0] == L'\\' && path[1] == L'\\'));
        item.icon = absolute ? ShellIcon(path) : GlyphIcon(glyph::kLink, tint::kQuicklink);
    }
    const std::wstring target = FillTemplate(link.target, argument, isUrl);
    const std::wstring args = FillArguments(link.arguments, argument);
    const std::wstring keyword = link.keyword;
    const bool admin = link.runAsAdmin;
    item.actions.push_back(Action{L"開啟", glyph::kOpen, Keys(VK_RETURN), false,
                                  [=](LauncherHost& host) {
                                      if (needsQuery && Trim(argument).empty()) {
                                          if (!keyword.empty()) {
                                              host.SetQuery(keyword + L" ");
                                              return Outcome::Stay;
                                          }
                                          host.ShowStatus(L"這個快速連結需要 keyword 才能輸入搜尋內容", true);
                                          return Outcome::Stay;
                                      }
                                      return OpenOrReport(host, isUrl ? target : ExpandUserPath(target),
                                                          {args, L"", admin});
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
    item.icon = GlyphIcon(glyph::kCalculator, tint::kCalc);
    item.actions.push_back(CopyAction(L"複製結果", plain, Keys(VK_RETURN)));
    item.actions.push_back(CopyAction(L"複製算式與結果", expression + L" = " + plain));
    if (!programmer.empty()) item.actions.push_back(CopyAction(L"複製十六進位", programmer.substr(0, programmer.find(L' ')), Shortcut{}));
    return item;
}

ResultItem SearchService::UrlItem(const std::wstring& url) const {
    ResultItem item;
    item.title = L"開啟 " + url;
    item.accessory = L"網址";
    item.icon = GlyphIcon(glyph::kGlobe, tint::kWeb);
    item.actions.push_back(Action{L"在瀏覽器開啟", glyph::kOpen, Keys(VK_RETURN), false,
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
    item.icon = GlyphIcon(glyph::kSearch, tint::kWeb);
    item.actions.push_back(Action{L"在瀏覽器搜尋", glyph::kSearch, Keys(VK_RETURN), false,
                                  [url](LauncherHost& host) { return OpenOrReport(host, url); }});
    item.actions.push_back(CopyAction(L"複製搜尋網址", url));
    return item;
}

ResultItem SearchService::ShellCommandItem(const std::wstring& command) const {
    ResultItem item;
    item.accessory = L"Shell";
    item.icon = GlyphIcon(glyph::kTerminal, tint::kShell);
    if (command.empty()) {
        item.title = L"輸入要執行的指令，例如 > ipconfig /all";
        item.subtitle = L"Enter：cmd　Ctrl+Enter：PowerShell";
        item.actions.push_back(Action{L"執行", glyph::kTerminal, Keys(VK_RETURN), false,
                                      [](LauncherHost&) { return Outcome::Stay; }});
        return item;
    }
    item.title = L"執行：" + command;
    item.subtitle = L"命令提示字元";
    item.actions.push_back(Action{L"在命令提示字元執行", glyph::kTerminal, Keys(VK_RETURN), false,
                                  [command](LauncherHost&) {
                                      RunCommandLine(command, false, false);
                                      return Outcome::Hide;
                                  }});
    item.actions.push_back(Action{L"在 PowerShell 執行", glyph::kTerminal, Keys(VK_RETURN, true), false,
                                  [command](LauncherHost&) {
                                      RunCommandLine(command, true, false);
                                      return Outcome::Hide;
                                  }});
    item.actions.push_back(Action{L"以系統管理員身分執行", glyph::kAdmin, Keys(VK_RETURN, true, true), false,
                                  [command](LauncherHost&) {
                                      RunCommandLine(command, false, true);
                                      return Outcome::Hide;
                                  }});
    item.actions.push_back(CopyAction(L"複製指令", command));
    return item;
}

ResultItem SearchService::PathItem(const std::wstring& path, bool directory) const {
    ResultItem item;
    const std::wstring name = FileNameOf(path);
    item.title = name.empty() ? path : name;
    item.subtitle = name.empty() ? L"" : ParentOf(path);
    item.accessory = directory ? L"資料夾" : L"檔案";
    item.icon = ShellIcon(path);
    if (directory) {
        item.autocomplete = (!path.empty() && path.back() == L'\\') ? path : path + L"\\";
        item.actions.push_back(Action{L"開啟資料夾", glyph::kFolderOpen, Keys(VK_RETURN), false,
                                      [path](LauncherHost& host) { return OpenOrReport(host, path); }});
        item.actions.push_back(Action{L"在終端機開啟", glyph::kTerminal, Keys(VK_RETURN, true), false,
                                      [path](LauncherHost&) {
                                          OpenTerminalAt(path);
                                          return Outcome::Hide;
                                      }});
    } else {
        item.actions.push_back(Action{L"開啟", glyph::kOpen, Keys(VK_RETURN), false, [path](LauncherHost& host) {
                                          return OpenOrReport(host, path, {L"", ParentOf(path)});
                                      }});
        if (IsExecutableExtension(ExtensionOf(path))) {
            item.actions.push_back(Action{L"以系統管理員身分執行", glyph::kAdmin, Keys(VK_RETURN, true, true), false,
                                          [path](LauncherHost& host) {
                                              return OpenOrReport(host, path, {L"", ParentOf(path), true});
                                          }});
        }
    }
    item.actions.push_back(RevealAction(path));
    item.actions.push_back(CopyAction(L"複製路徑", path));
    return item;
}

std::vector<ResultItem> SearchService::PathResults(const std::wstring& q) const {
    std::wstring expanded = ExpandUserPath(q);
    if (expanded.size() == 2 && expanded[1] == L':') expanded += L'\\';
    std::vector<ResultItem> items;
    if (IsNetworkPath(expanded)) {  // UNC or mapped drive: never block the UI on a network round trip
        items.push_back(PathItem(expanded, true));
        return items;
    }
    const bool endsWithSeparator = !expanded.empty() && expanded.back() == L'\\';
    std::wstring dir = endsWithSeparator ? expanded : ParentOf(expanded);
    const std::wstring prefix = endsWithSeparator ? L"" : FileNameOf(expanded);

    const DWORD attr = GetFileAttributesW(expanded.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && !endsWithSeparator)
        items.push_back(PathItem(expanded, (attr & FILE_ATTRIBUTE_DIRECTORY) != 0));

    if (dir.empty() || !DirectoryExists(dir)) return items;
    struct Child {
        std::wstring name;
        bool directory;
        int rank;
    };
    std::vector<Child> children;
    WIN32_FIND_DATAW fd{};
    const std::wstring base = dir.back() == L'\\' ? dir : dir + L"\\";
    HANDLE h = FindFirstFileExW((base + L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h != INVALID_HANDLE_VALUE) {
        int scanned = 0;
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L".." || (fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM)) continue;
            int rank = 0;
            if (!prefix.empty()) {
                if (StartsWithIgnoreCase(name, prefix)) rank = 0;
                else if (FindIgnoreCase(name, prefix) != std::wstring::npos) rank = 1;
                else continue;
                if (EqualsIgnoreCase(name, prefix)) continue;  // already shown as the exact item
            }
            children.push_back({name, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, rank});
        } while (++scanned < 2000 && FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(children.begin(), children.end(), [](const Child& a, const Child& b) {
        if (a.rank != b.rank) return a.rank < b.rank;
        if (a.directory != b.directory) return a.directory;
        return Fold(a.name) < Fold(b.name);
    });
    if (children.size() > 30) children.resize(30);
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
        item.actions.push_back(Action{L"開啟", glyph::kOpen, Keys(VK_RETURN), false, [id, argument](LauncherHost& host) {
                                          host.EnterPlugin(id, argument);
                                          return Outcome::Stay;
                                      }});
    } else {
        item.actions.push_back(Action{L"執行", glyph::kPlay, Keys(VK_RETURN), false, [id, argument](LauncherHost& host) {
                                          host.RunPluginCommand(id, argument);
                                          return Outcome::Stay;
                                      }});
    }
    App* app = &app_;
    const std::wstring manifest = pc.manifestPath;
    item.actions.push_back(Action{L"編輯 plugin.json", glyph::kEdit, Shortcut{}, false, [manifest](LauncherHost& host) {
                                      return OpenOrReport(host, L"notepad.exe", {QuoteArgument(manifest)});
                                  }});
    item.actions.push_back(RevealAction(manifest));
    item.actions.push_back(Action{L"外掛偵錯紀錄", glyph::kDiagnostic, Shortcut{}, false, [app](LauncherHost& host) {
                                      host.ShowDetail(L"外掛偵錯紀錄", app->plugins().DescribeRecords());
                                      return Outcome::Stay;
                                  }});
    return item;
}

std::vector<ResultItem> SearchService::NewPluginItems(const std::wstring& name) const {
    struct Language {
        PluginManager::Template language;
        const wchar_t* label;
        const wchar_t* note;
    };
    static const Language kLanguages[] = {
        {PluginManager::Template::Python, L"Python", L"需要安裝 Python 3"},
        {PluginManager::Template::Node, L"Node.js", L"需要安裝 Node.js"},
        {PluginManager::Template::PowerShell, L"PowerShell", L"Windows 內建，不用另外安裝"},
    };
    App* app = &app_;
    std::vector<ResultItem> items;
    for (const auto& lang : kLanguages) {
        ResultItem item;
        item.title = std::wstring(L"建立 ") + lang.label + L" 外掛" + (name.empty() ? L"" : L"「" + name + L"」");
        item.subtitle = name.empty() ? L"接著輸入外掛名稱，例如：newplugin 匯率查詢" : lang.note;
        item.accessory = L"範本";
        item.section = kSectionNewPlugin;
        item.icon = GlyphIcon(glyph::kPuzzle, tint::kPlugin);
        const PluginManager::Template language = lang.language;
        item.actions.push_back(Action{L"建立外掛", glyph::kAdd, Keys(VK_RETURN), false, [app, name, language](LauncherHost& host) {
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
            RevealInExplorer(folder + L"\\" + script);
            app->Notify(L"外掛已建立：" + FileNameOf(folder),
                        L"在 Beam 輸入「" + keyword + L" 」（加一個空白）試試看。修改 " + script + L" 存檔後立即生效。");
            return Outcome::Hide;
        }});
        items.push_back(std::move(item));
    }
    return items;
}

ItemIcon SearchService::PluginIcon(const PluginCommand& cmd, const std::wstring& spec) const {
    if (spec.empty()) return GlyphIcon(glyph::kPuzzle, tint::kPlugin);
    if (const auto g = ParseGlyphSpec(spec)) return ItemIcon{IconType::Glyph, g->glyph, g->color ? g->color : tint::kPlugin};
    if (StartsWithIgnoreCase(spec, L"icon:")) return GlyphIcon(glyph::kPuzzle, tint::kPlugin);  // Linux icon-theme names
    std::wstring path = ExpandUserPath(spec);
    for (auto& c : path)
        if (c == L'/') c = L'\\';
    if (!IsAbsolute(path)) path = cmd.folder + L"\\" + path;
    return ShellIcon(path);
}

Action SearchService::PluginActionFor(const PluginCommand& cmd, const Json* item, const PluginAction& a) const {
    Action action;
    action.title = a.title.empty() ? DefaultActionTitle(a.kind) : a.title;
    if (const auto chord = ParseKeyChord(a.shortcut); chord && !(chord->mods & kModWin)) {
        action.shortcut = Keys(chord->vk, (chord->mods & kModControl) != 0, (chord->mods & kModShift) != 0,
                               (chord->mods & kModAlt) != 0);
    }
    const std::wstring folder = cmd.folder;
    const std::wstring value = a.value;
    const bool keepOpen = a.keepOpen.value_or(false);
    switch (a.kind) {
        case PluginActionKind::Open:
            action.glyph = glyph::kOpen;
            action.run = [folder, value, keepOpen](LauncherHost& host) {
                std::wstring target = value;
                if (target.find(L':') == std::wstring::npos && target.rfind(L"\\\\", 0) != 0) {
                    // A path relative to the plugin folder ("docs\\help.html"), or "%VAR%" / "~" paths.
                    const std::wstring expanded = ExpandUserPath(target);
                    if (expanded != target) target = expanded;
                    else if (FileExists(folder + L"\\" + target) || DirectoryExists(folder + L"\\" + target))
                        target = folder + L"\\" + target;
                }
                const Outcome outcome = OpenOrReport(host, target);
                return outcome == Outcome::Hide && keepOpen ? Outcome::Stay : outcome;
            };
            break;
        case PluginActionKind::Copy:
            action.glyph = glyph::kCopy;
            action.run = [value, keepOpen](LauncherHost& host) {
                CopyToClipboard(host.Window(), value);
                if (!keepOpen) return Outcome::Hide;
                host.ShowStatus(L"已複製到剪貼簿", false);
                return Outcome::Stay;
            };
            break;
        case PluginActionKind::Run: {
            action.glyph = glyph::kTerminal;
            const std::wstring cwd = a.cwd.empty() ? folder : ExpandUserPath(a.cwd);
            const bool terminal = a.terminal;
            action.run = [value, cwd, terminal, keepOpen](LauncherHost& host) {
                std::wstring error;
                if (!SpawnDetached(value, cwd, terminal, &error)) {
                    host.ShowStatus(L"無法執行：" + error, true);
                    return Outcome::Stay;
                }
                return keepOpen ? Outcome::Stay : Outcome::Hide;
            };
            break;
        }
        case PluginActionKind::SetQuery:
            action.glyph = glyph::kSearch;
            action.run = [value](LauncherHost& host) {
                host.SetQuery(value);
                return Outcome::Stay;
            };
            break;
        case PluginActionKind::Callback: {
            action.glyph = glyph::kPlay;
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
            const auto chord = ParseKeyChord(a.shortcut);
            if (chord && chord->vk == VK_RETURN && chord->mods == kModControl) ctrlEnterTaken = true;
        }
        for (size_t i = 0; i < it.actions.size(); ++i) {
            Action action = PluginActionFor(cmd, &it.raw, it.actions[i]);
            if (!action.run) continue;
            if (item.actions.empty()) {
                action.alternate = action.shortcut;  // Enter always runs the first action
                action.shortcut = Keys(VK_RETURN);
            } else if (item.actions.size() == 1 && action.shortcut.Empty() && !ctrlEnterTaken) {
                action.shortcut = Keys(VK_RETURN, true);
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
    item.title = newline == std::wstring::npos ? failure : failure.substr(0, newline);
    item.subtitle = cmd.spec.name;
    item.accessory = L"錯誤";
    item.section = L"外掛發生錯誤";
    item.icon = GlyphIcon(glyph::kWarning, tint::kDanger);
    App* app = &app_;
    item.actions.push_back(Action{L"檢視偵錯紀錄", glyph::kDiagnostic, Keys(VK_RETURN), false, [app](LauncherHost& host) {
                                      host.ShowDetail(L"外掛偵錯紀錄", app->plugins().DescribeRecords());
                                      return Outcome::Stay;
                                  }});
    const std::wstring manifest = cmd.manifestPath;
    item.actions.push_back(Action{L"編輯 plugin.json", glyph::kEdit, Shortcut{}, false, [manifest](LauncherHost& host) {
                                      return OpenOrReport(host, L"notepad.exe", {QuoteArgument(manifest)});
                                  }});
    item.actions.push_back(RevealAction(manifest));
    return item;
}

}  // namespace beam
