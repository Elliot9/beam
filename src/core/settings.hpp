// User settings persisted as %APPDATA%\Beam\settings.json.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/json.hpp"

namespace beam {

struct Quicklink {
    std::wstring name;
    std::wstring keyword;    // optional: "gh" lets you type "gh react"
    std::wstring target;     // URL, file, folder or program; may contain {query}
    std::wstring arguments;  // optional program arguments; may contain {query}
    bool runAsAdmin = false;
    bool operator==(const Quicklink&) const = default;
};

std::vector<Quicklink> DefaultQuicklinks();

struct Settings {
    std::wstring hotkey = L"Alt+Space";
    std::wstring windowSwitcherHotkey;  // empty = disabled
    bool launchAtStartup = false;
    bool includeWindowsInSearch = true;
    std::wstring theme = L"auto";  // auto | dark | light
    std::wstring searchEngineUrl = L"https://www.google.com/search?q={query}";
    int maxResults = 40;
    std::vector<std::wstring> extraFolders;
    std::vector<Quicklink> quicklinks = DefaultQuicklinks();
    bool operator==(const Settings&) const = default;
};

Settings SettingsFromJson(const Json& json);
Json SettingsToJson(const Settings& settings);

struct SettingsLoadResult {
    Settings settings;
    bool created = false;  // file did not exist; defaults were written
    std::string error;     // parse error (defaults returned, file left untouched)
};

SettingsLoadResult LoadSettings(const std::filesystem::path& file);
bool SaveSettings(const std::filesystem::path& file, const Settings& settings);

bool ReadTextFile(const std::filesystem::path& file, std::string& out);
bool WriteTextFileAtomic(const std::filesystem::path& file, const std::string& data);

}  // namespace beam
