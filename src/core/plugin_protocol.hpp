// Beam plugin protocol (portable part).
//
// A plugin is a folder with plugin.json and any executable/script. Beam runs
// the "run" command line, writes one JSON request to its stdin and reads one
// JSON response from its stdout. See PLUGINS.md for the full description.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/json.hpp"

namespace beam {

enum class PluginMode { List, Command };
enum class PluginFiltering { Plugin, Beam };  // who filters list results as you type

struct PluginCommandSpec {
    std::wstring id;  // "folder" or "folder/command-id"
    std::wstring name;
    std::wstring description;
    std::wstring keyword;
    std::wstring placeholder;
    std::wstring run;
    std::wstring icon;
    PluginMode mode = PluginMode::List;
    PluginFiltering filtering = PluginFiltering::Plugin;
    int timeoutMs = 0;  // 0 = DefaultTimeoutMs(mode)
    int debounceMs = 120;
};

struct PluginManifest {
    std::wstring name;
    std::wstring description;
    std::wstring icon;
    std::vector<PluginCommandSpec> commands;
};

// nullopt when the JSON is invalid or no command is usable; `problems` lists what was wrong.
std::optional<PluginManifest> ParsePluginManifest(std::string_view text, const std::wstring& folderName,
                                                  std::vector<std::string>* problems = nullptr);
int DefaultTimeoutMs(PluginMode mode);

enum class PluginActionKind { None, Open, Copy, Run, SetQuery, Callback };

struct PluginAction {
    PluginActionKind kind = PluginActionKind::None;
    std::wstring title;
    std::wstring value;      // URL/path, text, command line, query text, or callback id
    std::wstring cwd;        // run: working directory
    bool terminal = false;   // run: in a visible console instead of hidden
    std::optional<bool> keepOpen;
    std::wstring shortcut;   // e.g. "Ctrl+Shift+C"
    Json raw;                // sent back to the plugin with callbacks
};

struct PluginItem {
    std::wstring id;
    std::wstring title;
    std::wstring subtitle;
    std::wstring accessory;
    std::wstring icon;
    std::wstring autocomplete;
    std::wstring section;  // optional group header
    std::vector<PluginAction> actions;
    Json raw;
};

struct PluginResponse {
    bool json = false;      // stdout parsed as JSON
    bool hasItems = false;  // an "items" list was present (possibly empty)
    std::vector<PluginItem> items;
    std::wstring message;
    std::wstring error;
    std::wstring output;     // long text for the detail view (or the raw stdout when not JSON)
    std::wstring placeholder;
    PluginAction directive;  // top-level open / copy / run / setQuery
    std::optional<bool> hide;
};

PluginResponse ParsePluginResponse(std::string_view stdoutUtf8);
std::wstring DefaultActionTitle(PluginActionKind kind);

// {"version":1,"type":...,"command":...,"query":...,"pluginDir":...,"dataDir":...,"item":...,"action":...}
std::string BuildPluginRequest(std::wstring_view type, std::wstring_view commandId, std::wstring_view query,
                               std::wstring_view pluginDir, std::wstring_view dataDir, const Json* item = nullptr,
                               const Json* action = nullptr);

// Command-line helpers (Windows quoting rules, implemented portably).
std::wstring QuoteArgument(std::wstring_view arg);  // CommandLineToArgvW-compatible
std::wstring ExpandRunTemplate(std::wstring_view run, std::wstring_view query);  // {query} -> quoted argument
std::pair<std::wstring, std::wstring> SplitCommandLine(std::wstring_view commandLine);  // first token, rest

struct GlyphSpec {
    std::wstring glyph;  // one Segoe MDL2 Assets code point
    uint32_t color = 0;  // ARGB, 0 = default
};
std::optional<GlyphSpec> ParseGlyphSpec(std::wstring_view spec);  // "glyph:E721" / "glyph:E721:#3B82F6"
std::optional<uint32_t> ParseColor(std::wstring_view text);       // "#RRGGBB" / "#AARRGGBB"

bool IsValidUtf8(std::string_view s);

}  // namespace beam
