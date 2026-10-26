#include "core/settings.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <sstream>

#include "core/text.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace beam {
namespace fs = std::filesystem;

namespace {

std::wstring GetString(const Json& obj, const char* key, const std::wstring& fallback) {
    const Json* v = obj.Find(key);
    return (v && v->IsString()) ? Utf8ToWide(v->AsString()) : fallback;
}

bool GetBool(const Json& obj, const char* key, bool fallback) {
    const Json* v = obj.Find(key);
    return (v && v->IsBool()) ? v->AsBool() : fallback;
}

Json Str(const std::wstring& s) { return Json(WideToUtf8(s)); }

}  // namespace

std::vector<Quicklink> DefaultQuicklinks() {
    return {
        {L"GitHub 搜尋", L"gh", L"https://github.com/search?q={query}&type=repositories", L"", false},
        {L"YouTube 搜尋", L"yt", L"https://www.youtube.com/results?search_query={query}", L"", false},
        {L"Google 翻譯", L"tr", L"https://translate.google.com/?sl=auto&tl=zh-TW&text={query}&op=translate", L"",
         false},
        {L"Google 地圖", L"map", L"https://www.google.com/maps/search/{query}", L"", false},
#ifdef _WIN32
        {L"編輯 hosts 檔案", L"", L"notepad.exe", L"C:\\Windows\\System32\\drivers\\etc\\hosts", true},
#endif
    };
}

Settings SettingsFromJson(const Json& json) {
    Settings s;
    if (!json.IsObject()) return s;
    s.hotkey = GetString(json, "hotkey", s.hotkey);
    s.windowSwitcherHotkey = GetString(json, "windowSwitcherHotkey", s.windowSwitcherHotkey);
    s.launchAtStartup = GetBool(json, "launchAtStartup", s.launchAtStartup);
    s.includeWindowsInSearch = GetBool(json, "includeWindowsInSearch", s.includeWindowsInSearch);
    s.theme = Fold(GetString(json, "theme", s.theme));
    if (s.theme != L"dark" && s.theme != L"light") s.theme = L"auto";
    s.searchEngineUrl = GetString(json, "searchEngineUrl", s.searchEngineUrl);
    if (const Json* v = json.Find("maxResults"); v && v->IsNumber())
        s.maxResults = std::clamp(static_cast<int>(v->AsNumber()), 5, 200);

    if (const Json* v = json.Find("extraFolders"); v && v->IsArray()) {
        s.extraFolders.clear();
        for (const auto& item : v->Items())
            if (item.IsString() && !item.AsString().empty()) s.extraFolders.push_back(Utf8ToWide(item.AsString()));
    }
    if (const Json* v = json.Find("quicklinks"); v && v->IsArray()) {
        s.quicklinks.clear();
        for (const auto& item : v->Items()) {
            if (!item.IsObject()) continue;
            Quicklink q;
            q.name = Trim(GetString(item, "name", L""));
            q.keyword = Trim(GetString(item, "keyword", L""));
            q.target = Trim(GetString(item, "target", L""));
            q.arguments = GetString(item, "arguments", L"");
            q.runAsAdmin = GetBool(item, "runAsAdmin", false);
            if (q.name.empty() || q.target.empty()) continue;
            s.quicklinks.push_back(std::move(q));
        }
    }
    return s;
}

Json SettingsToJson(const Settings& s) {
    Json j = Json::MakeObject();
    j.Set("hotkey", Str(s.hotkey));
    j.Set("windowSwitcherHotkey", Str(s.windowSwitcherHotkey));
    j.Set("launchAtStartup", s.launchAtStartup);
    j.Set("includeWindowsInSearch", s.includeWindowsInSearch);
    j.Set("theme", Str(s.theme));
    j.Set("searchEngineUrl", Str(s.searchEngineUrl));
    j.Set("maxResults", s.maxResults);
    Json folders = Json::MakeArray();
    for (const auto& f : s.extraFolders) folders.Push(Str(f));
    j.Set("extraFolders", std::move(folders));
    Json links = Json::MakeArray();
    for (const auto& q : s.quicklinks) {
        Json o = Json::MakeObject();
        o.Set("name", Str(q.name));
        if (!q.keyword.empty()) o.Set("keyword", Str(q.keyword));
        o.Set("target", Str(q.target));
        if (!q.arguments.empty()) o.Set("arguments", Str(q.arguments));
        if (q.runAsAdmin) o.Set("runAsAdmin", true);
        links.Push(std::move(o));
    }
    j.Set("quicklinks", std::move(links));
    return j;
}

bool ReadTextFile(const fs::path& file, std::string& out) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
}

bool WriteTextFileAtomic(const fs::path& file, const std::string& data) {
    std::error_code ec;
    if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
    fs::path tmp = file;
    tmp += L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out) return false;
    }
#ifdef _WIN32
    if (!MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
#else
    fs::rename(tmp, file, ec);
    return !ec;
#endif
}

SettingsLoadResult LoadSettings(const fs::path& file) {
    SettingsLoadResult result;
    std::error_code ec;
    if (!fs::exists(file, ec)) {
        result.created = true;
        SaveSettings(file, result.settings);
        return result;
    }
    std::string text;
    if (!ReadTextFile(file, text)) {
        result.error = "cannot read settings.json";
        return result;
    }
    std::string error;
    const auto json = Json::Parse(text, &error);
    if (!json) {
        result.error = error;
        return result;
    }
    if (!json->IsObject()) {
        result.error = "settings.json must contain a JSON object";
        return result;
    }
    result.settings = SettingsFromJson(*json);
    return result;
}

bool SaveSettings(const fs::path& file, const Settings& settings) {
    return WriteTextFileAtomic(file, SettingsToJson(settings).Dump());
}

}  // namespace beam
