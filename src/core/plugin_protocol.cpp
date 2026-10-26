#include "core/plugin_protocol.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/text.hpp"

namespace beam {
namespace {

std::wstring Str(const Json& obj, const char* key, const std::wstring& fallback = {}) {
    const Json* v = obj.Find(key);
    return (v && v->IsString()) ? Utf8ToWide(v->AsString()) : fallback;
}

std::optional<bool> OptBool(const Json& obj, const char* key) {
    const Json* v = obj.Find(key);
    if (v && v->IsBool()) return v->AsBool();
    return std::nullopt;
}

std::optional<int> OptInt(const Json& obj, const char* key) {
    const Json* v = obj.Find(key);
    if (v && v->IsNumber()) return static_cast<int>(v->AsNumber());
    return std::nullopt;
}

std::wstring NumberToString(double n) {
    char buf[64];
    if (std::floor(n) == n && std::fabs(n) < 1e15) std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(n));
    else std::snprintf(buf, sizeof buf, "%.17g", n);
    return Utf8ToWide(buf);
}

bool ParseMode(const std::wstring& text, PluginMode& mode) {
    const std::wstring m = Fold(Trim(text));
    if (m.empty()) return true;
    if (m == L"list" || m == L"filter" || m == L"search") {
        mode = PluginMode::List;
        return true;
    }
    if (m == L"command" || m == L"script" || m == L"run") {
        mode = PluginMode::Command;
        return true;
    }
    return false;
}

// `shorthand`: the action fields sit directly on an item / response object, so its
// "title" belongs to the item; an optional "actionTitle" names the action instead.
std::optional<PluginAction> ParseAction(const Json& j, bool allowCallback, bool shorthand) {
    if (!j.IsObject()) return std::nullopt;
    struct Kind {
        const char* key;
        PluginActionKind kind;
    };
    static const Kind kKinds[] = {
        {"open", PluginActionKind::Open},         {"copy", PluginActionKind::Copy},
        {"run", PluginActionKind::Run},           {"setQuery", PluginActionKind::SetQuery},
        {"callback", PluginActionKind::Callback},
    };
    PluginAction a;
    for (const auto& k : kKinds) {
        const Json* v = j.Find(k.key);
        if (!v || (k.kind == PluginActionKind::Callback && !allowCallback)) continue;
        if (v->IsString()) {
            a.value = Utf8ToWide(v->AsString());
        } else if (v->IsNumber() && (k.kind == PluginActionKind::Copy || k.kind == PluginActionKind::Callback)) {
            a.value = NumberToString(v->AsNumber());
        } else if (v->IsBool() && v->AsBool() && k.kind == PluginActionKind::Callback) {
            a.value.clear();
        } else {
            continue;
        }
        a.kind = k.kind;
        break;
    }
    if (a.kind == PluginActionKind::None) return std::nullopt;
    a.title = Trim(Str(j, shorthand ? "actionTitle" : "title"));
    if (a.title.empty()) a.title = DefaultActionTitle(a.kind);
    a.cwd = Str(j, "cwd");
    a.terminal = OptBool(j, "terminal").value_or(false);
    a.keepOpen = OptBool(j, "keepOpen");
    a.shortcut = Trim(Str(j, "shortcut"));
    a.raw = j;
    return a;
}

std::optional<PluginItem> ParseItem(const Json& j) {
    PluginItem it;
    if (j.IsString()) {  // ["a", "b"] is a valid, minimal list: Enter copies the text
        it.title = Utf8ToWide(j.AsString());
        if (Trim(it.title).empty()) return std::nullopt;
        PluginAction copy;
        copy.kind = PluginActionKind::Copy;
        copy.value = it.title;
        copy.title = DefaultActionTitle(PluginActionKind::Copy);
        copy.raw = j;
        it.actions.push_back(std::move(copy));
        it.raw = j;
        return it;
    }
    if (!j.IsObject()) return std::nullopt;
    it.title = Str(j, "title");
    if (Trim(it.title).empty()) return std::nullopt;
    if (const Json* id = j.Find("id")) {
        if (id->IsString()) it.id = Utf8ToWide(id->AsString());
        else if (id->IsNumber()) it.id = NumberToString(id->AsNumber());
    }
    it.subtitle = Str(j, "subtitle");
    it.accessory = Str(j, "accessory");
    it.icon = Trim(Str(j, "icon"));
    it.autocomplete = Str(j, "autocomplete");
    it.section = Trim(Str(j, "section"));
    if (auto primary = ParseAction(j, true, true)) it.actions.push_back(std::move(*primary));
    if (const Json* actions = j.Find("actions"); actions && actions->IsArray()) {
        for (const auto& a : actions->Items()) {
            if (it.actions.size() >= 16) break;
            if (auto parsed = ParseAction(a, true, false)) it.actions.push_back(std::move(*parsed));
        }
    }
    it.raw = j;
    return it;
}

}  // namespace

int DefaultTimeoutMs(PluginMode mode) { return mode == PluginMode::List ? 5000 : 60000; }

std::wstring DefaultActionTitle(PluginActionKind kind) {
    switch (kind) {
        case PluginActionKind::Open: return L"開啟";
        case PluginActionKind::Copy: return L"複製";
        case PluginActionKind::Run: return L"執行";
        case PluginActionKind::SetQuery: return L"填入搜尋";
        case PluginActionKind::Callback: return L"執行動作";
        default: return L"";
    }
}

std::optional<PluginManifest> ParsePluginManifest(std::string_view text, const std::wstring& folderName,
                                                  std::vector<std::string>* problems) {
    auto report = [problems](const std::string& p) {
        if (problems) problems->push_back(p);
    };
    std::string error;
    const auto json = Json::Parse(text, &error);
    if (!json) {
        report("plugin.json: " + error);
        return std::nullopt;
    }
    if (!json->IsObject()) {
        report("plugin.json must contain a JSON object");
        return std::nullopt;
    }

    PluginManifest m;
    m.name = Trim(Str(*json, "name", folderName));
    if (m.name.empty()) m.name = folderName;
    m.description = Str(*json, "description");
    m.icon = Trim(Str(*json, "icon"));

    // Either one command described at the top level, or a "commands" array whose
    // entries inherit the top-level fields as defaults.
    std::vector<const Json*> sources;
    const Json* list = json->Find("commands");
    const bool multi = list && list->IsArray();
    if (list && !multi) report("\"commands\" must be an array");
    if (multi) {
        for (const auto& c : list->Items()) sources.push_back(&c);
    } else {
        sources.push_back(&*json);
    }

    for (size_t i = 0; i < sources.size(); ++i) {
        const Json& src = *sources[i];
        const std::string where = multi ? "commands[" + std::to_string(i) + "]" : "plugin.json";
        if (!src.IsObject()) {
            report(where + ": must be an object");
            continue;
        }
        PluginCommandSpec c;
        const std::wstring localId = multi ? Trim(Str(src, "id", std::to_wstring(i + 1))) : std::wstring();
        c.id = multi ? folderName + L"/" + localId : folderName;
        c.name = Trim(Str(src, "name"));
        if (c.name.empty()) c.name = multi ? m.name + L" " + localId : m.name;
        c.description = Str(src, "description");
        if (c.description.empty()) c.description = m.description;
        c.keyword = Trim(Str(src, "keyword"));
        c.placeholder = Str(src, "placeholder");
        c.run = Trim(Str(src, "run", Str(*json, "run")));
        c.icon = Trim(Str(src, "icon", m.icon));
        if (!ParseMode(Str(src, "mode", Str(*json, "mode")), c.mode))
            report(where + ": unknown \"mode\" (use \"list\" or \"command\")");
        const std::wstring filtering = Fold(Trim(Str(src, "filtering", Str(*json, "filtering"))));
        if (filtering == L"beam" || filtering == L"local") c.filtering = PluginFiltering::Beam;
        else if (!filtering.empty() && filtering != L"plugin")
            report(where + ": unknown \"filtering\" (use \"plugin\" or \"beam\")");
        std::optional<int> timeout = OptInt(src, "timeoutMs");
        if (!timeout) timeout = OptInt(*json, "timeoutMs");
        c.timeoutMs = timeout ? std::clamp(*timeout, 500, 600000) : DefaultTimeoutMs(c.mode);
        std::optional<int> debounce = OptInt(src, "debounceMs");
        if (!debounce) debounce = OptInt(*json, "debounceMs");
        c.debounceMs = debounce ? std::clamp(*debounce, 0, 2000) : 120;
        if (const size_t space = c.keyword.find_first_of(L" \t"); space != std::wstring::npos) {
            report(where + ": \"keyword\" cannot contain spaces");
            c.keyword.resize(space);
        }
        if (c.run.empty()) {
            report(where + ": missing \"run\"");
            continue;
        }
        m.commands.push_back(std::move(c));
    }
    if (m.commands.empty()) {
        report("no usable command");
        return std::nullopt;
    }
    return m;
}

PluginResponse ParsePluginResponse(std::string_view text) {
    PluginResponse r;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);
    }
    auto isWs = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!text.empty() && isWs(text.front())) text.remove_prefix(1);
    while (!text.empty() && isWs(text.back())) text.remove_suffix(1);
    if (text.empty()) return r;

    if (text.front() == '{' || text.front() == '[') {
        if (const auto j = Json::Parse(text)) {
            r.json = true;
            const Json* items = nullptr;
            if (j->IsArray()) {
                items = &*j;
            } else {
                items = j->Find("items");
                r.message = Str(*j, "message");
                r.error = Str(*j, "error");
                r.output = Str(*j, "output");
                r.placeholder = Str(*j, "placeholder");
                r.hide = OptBool(*j, "hide");
                if (auto d = ParseAction(*j, false, true)) r.directive = std::move(*d);
            }
            if (items && items->IsObject()) {  // a single item where a list was expected
                r.hasItems = true;
                if (auto it = ParseItem(*items)) r.items.push_back(std::move(*it));
            } else if (items && items->IsArray()) {
                r.hasItems = true;
                for (const auto& x : items->Items()) {
                    if (r.items.size() >= 500) break;
                    if (auto it = ParseItem(x)) r.items.push_back(std::move(*it));
                }
            }
            return r;
        }
    }
    r.output = Utf8ToWide(text);
    return r;
}

std::string BuildPluginRequest(std::wstring_view type, std::wstring_view commandId, std::wstring_view query,
                               std::wstring_view pluginDir, std::wstring_view dataDir, const Json* item,
                               const Json* action) {
    Json j = Json::MakeObject();
    j.Set("version", 1);
    j.Set("type", Json(WideToUtf8(type)));
    j.Set("command", Json(WideToUtf8(commandId)));
    j.Set("query", Json(WideToUtf8(query)));
    j.Set("pluginDir", Json(WideToUtf8(pluginDir)));
    j.Set("dataDir", Json(WideToUtf8(dataDir)));
    if (item) j.Set("item", *item);
    if (action) j.Set("action", *action);
    return j.Dump(-1);
}

std::wstring QuoteArgument(std::wstring_view arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) return std::wstring(arg);
    std::wstring out = L"\"";
    for (size_t i = 0;; ++i) {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++backslashes;
            ++i;
        }
        if (i == arg.size()) {
            out.append(backslashes * 2, L'\\');  // double them so the closing quote stays a quote
            break;
        }
        if (arg[i] == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(arg[i]);
        }
    }
    out.push_back(L'"');
    return out;
}

std::wstring ExpandRunTemplate(std::wstring_view run, std::wstring_view query) {
    const std::wstring quoted = QuoteArgument(query);
    std::wstring out;
    size_t from = 0;
    for (;;) {
        const size_t at = FindIgnoreCase(run, L"{query}", from);
        if (at == std::wstring_view::npos) {
            out.append(run.substr(from));
            return out;
        }
        size_t begin = at;
        size_t end = at + 7;
        // "{query}" already wrapped in quotes: replace the quotes as well.
        if (begin > from && run[begin - 1] == L'"' && end < run.size() && run[end] == L'"') {
            --begin;
            ++end;
        }
        out.append(run.substr(from, begin - from));
        out += quoted;
        from = end;
    }
}

std::pair<std::wstring, std::wstring> SplitCommandLine(std::wstring_view s) {
    size_t i = 0;
    while (i < s.size() && IsSpace(s[i])) ++i;
    std::wstring first;
    if (i < s.size() && s[i] == L'"') {
        size_t close = s.find(L'"', i + 1);
        if (close == std::wstring_view::npos) close = s.size();
        first = std::wstring(s.substr(i + 1, close - i - 1));
        i = close == s.size() ? close : close + 1;
    } else {
        size_t j = i;
        while (j < s.size() && !IsSpace(s[j])) ++j;
        first = std::wstring(s.substr(i, j - i));
        i = j;
    }
    while (i < s.size() && IsSpace(s[i])) ++i;
    return {first, std::wstring(s.substr(i))};
}

std::optional<uint32_t> ParseColor(std::wstring_view text) {
    if (text.size() != 7 && text.size() != 9) return std::nullopt;
    if (text[0] != L'#') return std::nullopt;
    uint32_t v = 0;
    for (size_t i = 1; i < text.size(); ++i) {
        const wchar_t c = text[i];
        uint32_t d;
        if (c >= L'0' && c <= L'9') d = c - L'0';
        else if (c >= L'a' && c <= L'f') d = 10 + (c - L'a');
        else if (c >= L'A' && c <= L'F') d = 10 + (c - L'A');
        else return std::nullopt;
        v = (v << 4) | d;
    }
    return text.size() == 7 ? (0xFF000000u | v) : v;
}

std::optional<GlyphSpec> ParseGlyphSpec(std::wstring_view spec) {
    if (!StartsWithIgnoreCase(spec, L"glyph:")) return std::nullopt;
    spec.remove_prefix(6);
    const size_t colon = spec.find(L':');
    const std::wstring_view code = spec.substr(0, colon);
    if (code.empty() || code.size() > 4) return std::nullopt;
    uint32_t cp = 0;
    for (const wchar_t c : code) {
        uint32_t d;
        if (c >= L'0' && c <= L'9') d = c - L'0';
        else if (c >= L'a' && c <= L'f') d = 10 + (c - L'a');
        else if (c >= L'A' && c <= L'F') d = 10 + (c - L'A');
        else return std::nullopt;
        cp = (cp << 4) | d;
    }
    if (cp < 0x20 || (cp >= 0xD800 && cp <= 0xDFFF)) return std::nullopt;
    GlyphSpec g;
    g.glyph = std::wstring(1, static_cast<wchar_t>(cp));
    if (colon != std::wstring_view::npos) {
        const auto color = ParseColor(spec.substr(colon + 1));
        if (!color) return std::nullopt;
        g.color = *color;
    }
    return g;
}

bool IsValidUtf8(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        size_t len = 1;
        uint32_t cp = c;
        if (c < 0x80) {
            ++i;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            len = 2;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
            cp = c & 0x07;
        } else {
            return false;
        }
        if (i + len > s.size()) return false;
        for (size_t k = 1; k < len; ++k) {
            const auto b = static_cast<unsigned char>(s[i + k]);
            if ((b & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (b & 0x3F);
        }
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        i += len;
    }
    return true;
}

}  // namespace beam
