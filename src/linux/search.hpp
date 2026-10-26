// Linux search: favorites, desktop apps, open windows (GNOME extension), built-in
// commands, plugins, quicklinks, calculator, URLs, paths and web search.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/settings.hpp"
#include "linux/apps.hpp"
#include "linux/results.hpp"
#include "linux/windows.hpp"
#include "plugin/plugin_manager.hpp"

namespace beam {

class App;

struct WindowState {
    std::vector<WindowInfo> windows;
    ExtensionState extension = ExtensionState::Unsupported;
};

class SearchService {
public:
    explicit SearchService(App& app);

    // Root and Windows modes (plugin lists are filled by the launcher).
    std::vector<ResultItem> Search(const std::wstring& query, Mode mode, const WindowState& windows) const;
    void OnIndexChanged();

    // Plugins
    std::vector<ResultItem> PluginItems(const PluginCommand& cmd, const PluginResponse& response) const;
    ResultItem PluginErrorItem(const PluginCommand& cmd, const std::wstring& failure) const;
    // `item` is the raw item the action belongs to (sent back with callbacks); null for top-level directives.
    Action PluginActionFor(const PluginCommand& cmd, const Json* item, const PluginAction& action) const;
    ItemIcon PluginIcon(const PluginCommand& cmd, const std::wstring& spec) const;

private:
    struct Command {
        std::wstring key;
        std::wstring title;
        std::wstring subtitle;
        std::vector<std::wstring> aliases;
        const char* icon;
        uint32_t tint;
        bool dangerous;
        std::function<Outcome(LauncherHost&)> run;
    };

    void BuildCommands();
    std::vector<ResultItem> EmptyState(const WindowState& windows) const;
    std::vector<ResultItem> WindowResults(const std::wstring& q, const WindowState& windows) const;
    std::vector<ResultItem> PathResults(const std::wstring& q) const;
    std::vector<ResultItem> NewPluginItems(const std::wstring& name) const;
    std::optional<ResultItem> ResolveUsageKey(const std::wstring& key) const;
    void AddFavoriteActions(ResultItem& item, bool inFavoritesSection) const;

    ResultItem AppItem(const AppEntry& app) const;
    ResultItem CommandItem(const Command& cmd) const;
    ResultItem PluginCommandItem(const PluginCommand& cmd, const std::wstring& argument) const;
    ResultItem WindowItem(const WindowInfo& w) const;
    ResultItem ExtensionItem(ExtensionState state) const;
    ResultItem QuicklinkItem(const Quicklink& link, const std::wstring& argument) const;
    ResultItem CalculatorItem(const std::wstring& expression, double value) const;
    ResultItem UrlItem(const std::wstring& url) const;
    ResultItem WebSearchItem(const std::wstring& q) const;
    ResultItem ShellCommandItem(const std::wstring& command) const;
    ResultItem PathItem(const std::wstring& path, bool directory) const;

    App& app_;
    std::vector<Command> commands_;
    std::unordered_map<std::wstring, size_t> appsByKey_;
    std::unordered_set<std::wstring> appNames_;
};

}  // namespace beam
