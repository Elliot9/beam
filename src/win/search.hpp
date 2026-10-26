// Turns the typed query into ranked result items: favorites, apps, open
// windows, built-in commands, plugins, quicklinks, calculator, URLs, paths
// and web search. Also converts plugin responses into result items.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/settings.hpp"
#include "win/apps.hpp"
#include "plugin/plugin_manager.hpp"
#include "win/results.hpp"
#include "win/winlist.hpp"

namespace beam {

class App;

class SearchService {
public:
    explicit SearchService(App& app);

    // Root and Windows modes (plugin lists are filled by the launcher).
    std::vector<ResultItem> Search(const std::wstring& query, Mode mode,
                                   const std::vector<WindowInfo>& windows) const;
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
        const wchar_t* glyph;
        uint32_t tint;
        bool dangerous;
        std::function<Outcome(LauncherHost&)> run;
    };

    void BuildCommands();
    std::vector<ResultItem> EmptyState(const std::vector<WindowInfo>& windows) const;
    std::vector<ResultItem> WindowResults(const std::wstring& q, const std::vector<WindowInfo>& windows) const;
    std::vector<ResultItem> PathResults(const std::wstring& q) const;
    std::vector<ResultItem> NewPluginItems(const std::wstring& name) const;
    std::optional<ResultItem> ResolveUsageKey(const std::wstring& key) const;
    void AddFavoriteActions(ResultItem& item, bool inFavoritesSection) const;

    ResultItem AppItem(const AppEntry& app) const;
    ResultItem CommandItem(const Command& cmd) const;
    ResultItem PluginCommandItem(const PluginCommand& cmd, const std::wstring& argument) const;
    ResultItem WindowItem(const WindowInfo& w) const;
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
