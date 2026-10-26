// Unit tests for the portable core. Build & run natively:
//   cmake -S . -B build-tests -DBEAM_BUILD_TESTS=ON && cmake --build build-tests && ./build-tests/beam_tests
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <tuple>
#include <vector>

#include "core/calc.hpp"
#include "core/edit_buffer.hpp"
#include "core/favorites.hpp"
#include "core/fuzzy.hpp"
#include "core/hotkey.hpp"
#include "core/json.hpp"
#include "core/plugin_protocol.hpp"
#include "core/query.hpp"
#include "core/settings.hpp"
#include "core/text.hpp"
#include "core/usage.hpp"

using namespace beam;

namespace {

int g_failures = 0;
int g_checks = 0;

void Report(bool ok, const char* expr, const char* file, int line, const std::string& detail = {}) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL %s:%d  %s %s\n", file, line, expr, detail.c_str());
    }
}

std::string Narrow(const std::wstring& w) { return WideToUtf8(w); }

#define CHECK(x) Report((x), #x, __FILE__, __LINE__)
#define CHECK_EQ(a, b)                                                                     \
    do {                                                                                   \
        const auto va_ = (a);                                                              \
        const auto vb_ = (b);                                                              \
        Report(va_ == vb_, #a " == " #b, __FILE__, __LINE__);                              \
    } while (0)
#define CHECK_WEQ(a, b)                                                                    \
    do {                                                                                   \
        const std::wstring va_ = (a);                                                      \
        const std::wstring vb_ = (b);                                                      \
        Report(va_ == vb_, #a " == " #b, __FILE__, __LINE__,                               \
               "(got \"" + Narrow(va_) + "\", want \"" + Narrow(vb_) + "\")");             \
    } while (0)
#define CHECK_NEAR(a, b)                                                                   \
    do {                                                                                   \
        const auto oa_ = (a);                                                              \
        const double vb_ = (b);                                                            \
        const bool ok_ = oa_.has_value() && std::fabs(*oa_ - vb_) < 1e-9;                  \
        Report(ok_, #a " ~= " #b, __FILE__, __LINE__,                                      \
               oa_ ? "(got " + std::to_string(*oa_) + ")" : std::string("(got nullopt)")); \
    } while (0)

void TestText() {
    const std::string utf8 = "Beam 記事本 \xF0\x9F\x9A\x80";  // includes an astral emoji
    CHECK_EQ(WideToUtf8(Utf8ToWide(utf8)), utf8);
    CHECK_WEQ(Utf8ToWide("\xE8\xA8\x98"), L"記");
    CHECK_WEQ(Utf8ToWide("\xFF"), L"\xFFFD");
    CHECK_WEQ(Fold(L"ChRoMe"), L"chrome");
    CHECK_WEQ(Fold(L"\xFF23\xFF48"), L"ch");  // full-width "Ｃｈ"
    CHECK_WEQ(Trim(L"  a b \x3000"), L"a b");
    CHECK(EqualsIgnoreCase(L"Notepad", L"NOTEPAD"));
    CHECK_EQ(FindIgnoreCase(L"Visual Studio Code", L"CODE"), size_t{14});
    CHECK_EQ(SplitWhitespace(L"  vs   code ").size(), size_t{2});
}

void TestFuzzy() {
    CHECK_EQ(FuzzyScore(L"code", L"Code"), 1000);
    CHECK(FuzzyScore(L"chr", L"Chrome") >= 850);
    CHECK(FuzzyScore(L"code", L"Visual Studio Code") >= 700);
    CHECK(FuzzyScore(L"vsc", L"Visual Studio Code") >= 650);
    CHECK(FuzzyScore(L"vscode", L"Visual Studio Code") > 0);
    CHECK(FuzzyScore(L"studio code", L"Visual Studio Code") >= 700);
    CHECK_EQ(FuzzyScore(L"xyz", L"Visual Studio Code"), kNoMatch);
    CHECK_EQ(FuzzyScore(L"vs", L"Developer Tools - devtools"), kNoMatch);  // too scattered
    CHECK_EQ(FuzzyScore(L"note", L"conhost"), kNoMatch);                  // must start on a word
    CHECK(FuzzyScore(L"gchr", L"Google Chrome") > 0);
    CHECK(FuzzyScore(L"提示", L"命令提示字元") >= 700);                  // CJK word start
    CHECK(FuzzyScore(L"ps", L"PowerShell") >= 650);                      // camelCase initials
    CHECK(FuzzyScore(L"term", L"Windows Terminal") > FuzzyScore(L"term", L"Determine Thing"));

    // Ordering for one query across typical candidates.
    const std::vector<std::wstring> names = {L"Unicode Viewer", L"Visual Studio Code", L"Code", L"Codex Helper"};
    std::vector<std::pair<int, std::wstring>> ranked;
    for (const auto& n : names) ranked.emplace_back(FuzzyScore(L"code", n), n);
    std::sort(ranked.begin(), ranked.end(), [](auto& a, auto& b) { return a.first > b.first; });
    CHECK_WEQ(ranked[0].second, L"Code");
    CHECK_WEQ(ranked[1].second, L"Codex Helper");
    CHECK_WEQ(ranked[2].second, L"Visual Studio Code");
    CHECK_WEQ(ranked[3].second, L"Unicode Viewer");

    // Aliases let English input find localized names; title hits win ties.
    CHECK(BestScore(L"notepad", L"記事本", {L"notepad"}) > 900);
    CHECK(BestScore(L"note", L"Notepad", {}) > BestScore(L"note", L"記事本", {L"notepad"}));
    CHECK_EQ(BestScore(L"zzz", L"記事本", {L"notepad"}), kNoMatch);
    CHECK_EQ(BestScore(L"", L"Anything", {}), 0);
}

void TestCalc() {
    CHECK_NEAR(EvaluateExpression(L"1+2*3"), 7);
    CHECK_NEAR(EvaluateExpression(L"(1+2)*3"), 9);
    CHECK_NEAR(EvaluateExpression(L"2^3^2"), 512);
    CHECK_NEAR(EvaluateExpression(L"-2^2"), -4);
    CHECK_NEAR(EvaluateExpression(L"2^-1"), 0.5);
    CHECK_NEAR(EvaluateExpression(L"2**10"), 1024);
    CHECK_NEAR(EvaluateExpression(L"10 % 3"), 1);
    CHECK_NEAR(EvaluateExpression(L"0xff + 1"), 256);
    CHECK_NEAR(EvaluateExpression(L"0b1010"), 10);
    CHECK_NEAR(EvaluateExpression(L"0o17"), 15);
    CHECK_NEAR(EvaluateExpression(L"sqrt(16)"), 4);
    CHECK_NEAR(EvaluateExpression(L"max(3, 7, 5)"), 7);
    CHECK_NEAR(EvaluateExpression(L"log(100)"), 2);
    CHECK_NEAR(EvaluateExpression(L"log(8, 2)"), 3);
    CHECK_NEAR(EvaluateExpression(L"ln(e)"), 1);
    CHECK_NEAR(EvaluateExpression(L"2pi"), 2 * 3.14159265358979323846);
    CHECK_NEAR(EvaluateExpression(L"2(3+4)"), 14);
    CHECK_NEAR(EvaluateExpression(L"(1+2)(3+4)"), 21);
    CHECK_NEAR(EvaluateExpression(L"1920x1080"), 2073600);
    CHECK_NEAR(EvaluateExpression(L"3 × 4 ÷ 2"), 6);
    CHECK_NEAR(EvaluateExpression(L"\xFF11\xFF0B\xFF12"), 3);  // full-width "１＋２"
    CHECK_NEAR(EvaluateExpression(L"5!"), 120);
    CHECK_NEAR(EvaluateExpression(L"1e3 + 1"), 1001);
    CHECK_NEAR(EvaluateExpression(L"1_000 * 3"), 3000);
    CHECK_NEAR(EvaluateExpression(L"round(3.14159, 2)"), 3.14);
    CHECK(!EvaluateExpression(L"1/0"));
    CHECK(!EvaluateExpression(L"7-zip"));
    CHECK(!EvaluateExpression(L"hello"));
    CHECK(!EvaluateExpression(L"(1+2"));
    CHECK(!EvaluateExpression(L"5 % 0"));
    CHECK(!EvaluateExpression(L"((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((1))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))"));

    CHECK(!LooksLikeMath(L"2048"));
    CHECK(!LooksLikeMath(L"notepad"));
    CHECK(!LooksLikeMath(L"v1.2"));
    CHECK(LooksLikeMath(L"0xff"));
    CHECK(LooksLikeMath(L"2+2"));
    CHECK(!TryCalculate(L"7-zip"));
    CHECK_NEAR(TryCalculate(L"12*12"), 144);

    CHECK_WEQ(FormatNumber(0.1 + 0.2), L"0.3");
    CHECK_WEQ(FormatNumber(1.0 / 3), L"0.3333333333");
    CHECK_WEQ(FormatNumber(-0.0), L"0");
    CHECK_WEQ(FormatNumber(1e20), L"1e+20");
    CHECK_WEQ(FormatNumber(42), L"42");
    CHECK_WEQ(FormatNumberGrouped(1234567.5), L"1,234,567.5");
    CHECK_WEQ(FormatNumberGrouped(-1234), L"-1,234");
    CHECK_WEQ(FormatNumberGrouped(123), L"123");
    CHECK_WEQ(FormatProgrammer(255), L"0xFF  \xB7  0b11111111");
    CHECK_WEQ(FormatProgrammer(1.5), L"");
}

void TestJson() {
    std::string err;
    auto j = Json::Parse(R"({
        // comment
        "name": "Beam \u8a18 \ud83d\ude80", /* block */
        "n": -12.5e1,
        "list": [1, true, null, "x",],
        "nested": {"k": "v"},
    })", &err);
    CHECK(j.has_value());
    if (j) {
        CHECK_EQ(j->Find("name")->AsString(), std::string("Beam \xE8\xA8\x98 \xF0\x9F\x9A\x80"));
        CHECK_EQ(j->Find("n")->AsNumber(), -125.0);
        CHECK_EQ(j->Find("list")->Items().size(), size_t{4});
        CHECK_EQ(j->Find("nested")->Find("k")->AsString(), std::string("v"));
        auto again = Json::Parse(j->Dump());
        CHECK(again.has_value() && again->Dump() == j->Dump());
    }
    CHECK(!Json::Parse(R"({"path": "C:\Tools"})", &err));
    CHECK(err.find("line 1") != std::string::npos);
    CHECK(!Json::Parse("{\"a\": 1} x"));
    CHECK(!Json::Parse("[1, 2"));
    CHECK(Json::Parse("\xEF\xBB\xBF{}").has_value());  // BOM
    Json o = Json::MakeObject();
    o.Set("a", 1);
    o.Set("a", 2);
    CHECK_EQ(o.Members().size(), size_t{1});
    CHECK_EQ(o.Dump(), std::string("{\n  \"a\": 2\n}\n"));
}

void TestHotkey() {
    auto hk = ParseHotkey(L"Alt+Space");
    CHECK(hk && hk->mods == kModAlt && hk->vk == 0x20);
    hk = ParseHotkey(L" ctrl + shift + k ");
    CHECK(hk && hk->mods == (kModControl | kModShift) && hk->vk == 'K');
    hk = ParseHotkey(L"Win+`");
    CHECK(hk && hk->mods == kModWin && hk->vk == 0xC0);
    hk = ParseHotkey(L"F13");
    CHECK(hk && hk->mods == 0 && hk->vk == 0x7C);
    CHECK(!ParseHotkey(L"K"));
    CHECK(!ParseHotkey(L"Ctrl+"));
    CHECK(!ParseHotkey(L"Ctrl+Alt"));
    CHECK(!ParseHotkey(L"Ctrl+A+B"));
    CHECK(!ParseHotkey(L"Ctrl+Banana"));
    CHECK_WEQ(FormatHotkey(Hotkey{kModControl | kModAlt | kModShift | kModWin, 'K'}), L"Win+Ctrl+Alt+Shift+K");
    for (const wchar_t* text : {L"Alt+Space", L"Ctrl+Shift+F12", L"Win+Alt+=", L"Ctrl+Num5", L"Alt+Esc"}) {
        auto p = ParseHotkey(text);
        CHECK(p.has_value());
        if (p) CHECK_WEQ(FormatHotkey(*p), text);
    }
}

void TestQuery() {
    CHECK_WEQ(TryGetUrl(L"github.com").value_or(L""), L"https://github.com");
    CHECK_WEQ(TryGetUrl(L"https://x.dev/a?b=1").value_or(L""), L"https://x.dev/a?b=1");
    CHECK_WEQ(TryGetUrl(L"localhost:3000").value_or(L""), L"http://localhost:3000");
    CHECK_WEQ(TryGetUrl(L"192.168.1.1:8080/admin").value_or(L""), L"http://192.168.1.1:8080/admin");
    CHECK_WEQ(TryGetUrl(L"www.example.co.uk/path").value_or(L""), L"https://www.example.co.uk/path");
    for (const wchar_t* no : {L"notepad.exe", L"readme.md", L"hello world", L"1.5", L"v1.2.3", L"C:\\foo",
                              L"node.js", L"999.1.1.1", L"example.com:abc"}) {
        Report(!TryGetUrl(no).has_value(), "not a URL", __FILE__, __LINE__, Narrow(no));
    }
    CHECK(LooksLikePath(L"C:\\Windows"));
    CHECK(LooksLikePath(L"d:"));
    CHECK(LooksLikePath(L"\\\\server\\share"));
    CHECK(LooksLikePath(L"~\\Downloads"));
    CHECK(LooksLikePath(L"%APPDATA%\\Beam"));
    CHECK(!LooksLikePath(L"c:foo"));
    CHECK(!LooksLikePath(L"chrome"));

    auto k = SplitKeyword(L"gh  react hooks");
    CHECK_WEQ(k.keyword, L"gh");
    CHECK_WEQ(k.rest, L"react hooks");
    CHECK(k.hasSpace);
    k = SplitKeyword(L"gh");
    CHECK(!k.hasSpace && k.rest.empty());

    CHECK_WEQ(FillTemplate(L"https://s?q={query}&x={Query}", L"a b/記", true),
              L"https://s?q=a%20b%2F%E8%A8%98&x=a%20b%2F%E8%A8%98");
    CHECK_WEQ(FillTemplate(L"-d \"{query}\"", L"C:\\x", false), L"-d \"C:\\x\"");
    CHECK(HasQueryPlaceholder(L"x {QUERY} y"));
}

void TestSettings() {
    const auto dir = std::filesystem::temp_directory_path() / "beam_tests";
    std::filesystem::remove_all(dir);
    const auto file = dir / "settings.json";

    auto r = LoadSettings(file);
    CHECK(r.created && r.error.empty());
    CHECK(std::filesystem::exists(file));
    CHECK_WEQ(r.settings.hotkey, L"Alt+Space");

    Settings s = r.settings;
    s.hotkey = L"Ctrl+Shift+Space";
    s.windowSwitcherHotkey = L"Alt+`";
    s.extraFolders = {L"D:\\Tools", L"C:\\中文"};
    s.quicklinks.push_back({L"測試", L"t", L"C:\\x.exe", L"-a {query}", true});
    s.maxResults = 999;  // clamped on load
    CHECK(SaveSettings(file, s));
    r = LoadSettings(file);
    CHECK(!r.created && r.error.empty());
    s.maxResults = 200;
    CHECK(r.settings == s);

    // Comments, trailing commas and unknown keys are fine; bad entries are skipped.
    WriteTextFileAtomic(file, R"({
        // my settings
        "hotkey": "Win+Space",
        "theme": "DARK",
        "quicklinks": [ {"name": "ok", "target": "x"}, {"name": "missing target"}, 3, ],
        "unknown": 1,
    })");
    r = LoadSettings(file);
    CHECK(r.error.empty());
    CHECK_WEQ(r.settings.hotkey, L"Win+Space");
    CHECK_WEQ(r.settings.theme, L"dark");
    CHECK_EQ(r.settings.quicklinks.size(), size_t{1});
    CHECK(r.settings.includeWindowsInSearch);  // default kept

    WriteTextFileAtomic(file, "{ \"hotkey\": ");
    r = LoadSettings(file);
    CHECK(!r.error.empty() && !r.created);
    std::string text;
    ReadTextFile(file, text);
    CHECK_EQ(text, std::string("{ \"hotkey\": "));  // broken file is left untouched
    std::filesystem::remove_all(dir);
}

void TestUsage() {
    const auto dir = std::filesystem::temp_directory_path() / "beam_usage_tests";
    std::filesystem::remove_all(dir);
    const int64_t now = 1'800'000'000;
    UsageStore u(dir / "usage.json");
    u.Record(L"app:a", L"c", now - 10 * 86400);
    u.Record(L"app:b", L"ch", now - 100);
    u.Record(L"app:b", L"ch", now - 50);
    CHECK(u.Boost(L"app:b", L"", now) > u.Boost(L"app:a", L"", now));
    CHECK(u.Boost(L"app:a", L"C", now) >= 250);  // remembered choice for "c"
    CHECK(u.Boost(L"app:b", L"c", now) < 250);
    CHECK_EQ(u.Boost(L"app:zzz", L"c", now), 0.0);
    auto recent = u.RecentKeys(5);
    CHECK(recent.size() == 2 && recent[0] == L"app:b");
    CHECK(u.Save());
    UsageStore v(dir / "usage.json");
    v.Load();
    CHECK_EQ(v.size(), size_t{2});
    CHECK_EQ(v.Boost(L"app:a", L"c", now), u.Boost(L"app:a", L"c", now));
    v.Remove(L"app:a");
    CHECK(v.Boost(L"app:a", L"c", now) == 0.0);
    std::filesystem::remove_all(dir);
}

void TestEditBuffer() {
    EditBuffer e;
    e.Insert(L"hello world");
    CHECK_EQ(e.Caret(), size_t{11});
    e.MoveLeft(true, false);
    CHECK_EQ(e.Caret(), size_t{6});
    e.MoveLeft(true, true);
    CHECK_WEQ(e.SelectedText(), L"hello ");
    e.Insert(L"X");
    CHECK_WEQ(e.Text(), L"Xworld");
    e.End(false);
    e.Backspace(true);
    CHECK_WEQ(e.Text(), L"");
    e.Insert(L"a\r\nb\tc\x01");
    CHECK_WEQ(e.Text(), L"a b c");
    e.SelectAll();
    e.Backspace(false);
    CHECK_WEQ(e.Text(), L"");
    e.Insert(L"foo.bar baz");
    e.Home(false);
    e.Delete(true);
    CHECK_WEQ(e.Text(), L".bar baz");
    e.SelectWordAt(2);
    CHECK_WEQ(e.SelectedText(), L"bar");
    e.SetText(L"x\U0001F680y");
    if constexpr (sizeof(wchar_t) == 2) {
        e.SetCaret(3, false);  // after the surrogate pair
        e.Backspace(false);
        CHECK_WEQ(e.Text(), L"xy");
    }
    e.SetText(L"abc", true);
    CHECK_WEQ(e.SelectedText(), L"abc");
    e.MoveRight(false, false);
    CHECK(!e.HasSelection() && e.Caret() == 3);
}


void TestKeyChord() {
    auto k = ParseKeyChord(L"Enter");
    CHECK(k && k->mods == 0 && k->vk == 0x0D);
    k = ParseKeyChord(L"Ctrl+Shift+C");
    CHECK(k && k->mods == (kModControl | kModShift) && k->vk == 'C');
    k = ParseKeyChord(L"alt+up");
    CHECK(k && k->mods == kModAlt && k->vk == 0x26);
    CHECK(!ParseKeyChord(L"Ctrl+"));
    CHECK(!ParseHotkey(L"Enter"));  // still not acceptable as a global hotkey
}

void TestFavorites() {
    const auto dir = std::filesystem::temp_directory_path() / "beam_fav_tests";
    std::filesystem::remove_all(dir);
    FavoritesStore f(dir / "favorites.json");
    CHECK(f.Add(L"app:a"));
    CHECK(f.Add(L"cmd:b"));
    CHECK(f.Add(L"ql:c"));
    CHECK(!f.Add(L"app:a"));
    CHECK(f.Move(L"ql:c", -2));
    CHECK(f.Keys().size() == 3 && f.Keys()[0] == L"ql:c" && f.Keys()[1] == L"app:a");
    CHECK(!f.Move(L"ql:c", -1));  // already first
    CHECK(!f.Move(L"zzz", 1));
    CHECK(!f.Toggle(L"cmd:b"));   // removed
    CHECK(f.Toggle(L"cmd:b"));    // added back at the end
    CHECK_EQ(f.IndexOf(L"cmd:b"), 2);
    CHECK(f.Save());
    FavoritesStore g(dir / "favorites.json");
    g.Load();
    CHECK(g.Keys() == f.Keys());
    std::filesystem::remove_all(dir);
}

void TestPluginManifest() {
    std::vector<std::string> problems;
    auto m = ParsePluginManifest(R"({
        "name": "GitHub Repos", "description": "search", "keyword": "repo",
        "run": "python main.py", "icon": "glyph:E8A7", "timeoutMs": 999999, // clamped
    })", L"gh", &problems);
    CHECK(m.has_value() && problems.empty());
    if (m) {
        CHECK_EQ(m->commands.size(), size_t{1});
        const auto& c = m->commands[0];
        CHECK_WEQ(c.id, L"gh");
        CHECK_WEQ(c.name, L"GitHub Repos");
        CHECK_WEQ(c.keyword, L"repo");
        CHECK(c.mode == PluginMode::List && c.filtering == PluginFiltering::Plugin);
        CHECK_EQ(c.timeoutMs, 600000);
        CHECK_EQ(c.debounceMs, 120);
    }
    problems.clear();
    m = ParsePluginManifest(R"({
        "name": "Tools", "run": "node main.js", "mode": "command",
        "commands": [
            {"id": "a", "name": "A", "keyword": "ta"},
            {"name": "B", "mode": "list", "filtering": "beam", "run": "b.exe"},
            {"id": "broken", "run": ""},
            {"id": "c", "keyword": "has space"}
        ]
    })", L"tools", &problems);
    CHECK(m.has_value());
    if (m) {
        CHECK_EQ(m->commands.size(), size_t{3});
        CHECK_WEQ(m->commands[0].id, L"tools/a");
        CHECK(m->commands[0].mode == PluginMode::Command);
        CHECK_WEQ(m->commands[0].run, L"node main.js");  // inherited
        CHECK_EQ(m->commands[0].timeoutMs, DefaultTimeoutMs(PluginMode::Command));
        CHECK_WEQ(m->commands[1].id, L"tools/2");
        CHECK(m->commands[1].mode == PluginMode::List && m->commands[1].filtering == PluginFiltering::Beam);
        CHECK_WEQ(m->commands[2].keyword, L"has");
        CHECK_WEQ(m->commands[2].name, L"Tools c");
    }
    CHECK_EQ(problems.size(), size_t{2});  // missing run, keyword with space
    problems.clear();
    CHECK(!ParsePluginManifest("{\"name\": \"x\"}", L"x", &problems));
    CHECK(!problems.empty());
    CHECK(!ParsePluginManifest("not json", L"x"));
}

void TestPluginResponse() {
    auto r = ParsePluginResponse(R"({"items": [
        {"title": "user/beam", "subtitle": "12", "id": 42, "open": "https://github.com/user/beam",
         "actions": [{"title": "Copy URL", "copy": "git@x", "shortcut": "Ctrl+Shift+C"},
                     {"title": "Refresh", "callback": "refresh", "data": {"n": 1}},
                     {"title": "nothing"}]},
        {"subtitle": "no title: skipped"},
        "plain string"
    ], "message": "2 repos"})");
    CHECK(r.json && r.hasItems);
    CHECK_EQ(r.items.size(), size_t{2});
    CHECK_WEQ(r.message, L"2 repos");
    if (r.items.size() == 2) {
        const auto& a = r.items[0];
        CHECK_WEQ(a.id, L"42");
        CHECK_EQ(a.actions.size(), size_t{3});  // shorthand open + copy + callback
        if (a.actions.size() == 3) {
            CHECK(a.actions[0].kind == PluginActionKind::Open);
            CHECK_WEQ(a.actions[0].title, L"開啟");  // the item title is not the action title
            CHECK(a.actions[1].kind == PluginActionKind::Copy && a.actions[1].shortcut == L"Ctrl+Shift+C");
            CHECK(a.actions[2].kind == PluginActionKind::Callback && a.actions[2].value == L"refresh");
            CHECK(a.actions[2].raw.Find("data") != nullptr);
        }
        CHECK(r.items[1].actions.size() == 1 && r.items[1].actions[0].kind == PluginActionKind::Copy);
    }
    r = ParsePluginResponse("[{\"title\": \"x\", \"run\": \"calc.exe\", \"terminal\": true}]");
    CHECK(r.json && r.hasItems && r.items.size() == 1 && r.items[0].actions[0].terminal);
    r = ParsePluginResponse("{\"items\": {\"title\": \"single\"}}");  // PowerShell-style single object
    CHECK(r.hasItems && r.items.size() == 1);
    r = ParsePluginResponse(R"([{"title": "a", "section": " 釘選 "}, {"title": "b"}])");
    CHECK(r.items.size() == 2 && r.items[0].section == L"釘選" && r.items[1].section.empty());
    r = ParsePluginResponse("\xEF\xBB\xBF  {\"copy\": \"done\", \"message\": \"ok\", \"hide\": false} \r\n");
    CHECK(r.json && !r.hasItems && r.directive.kind == PluginActionKind::Copy && r.hide == false);
    r = ParsePluginResponse("{\"callback\": \"x\"}");
    CHECK(r.directive.kind == PluginActionKind::None);  // callbacks only make sense on items
    r = ParsePluginResponse("hello\nworld\n");
    CHECK(!r.json && r.output == L"hello\nworld");
    r = ParsePluginResponse("{broken");
    CHECK(!r.json && r.output == L"{broken");
    r = ParsePluginResponse("   ");
    CHECK(!r.json && r.output.empty());
}

void TestPluginRequestAndCommandLine() {
    Json item = Json::MakeObject();
    item.Set("title", "t");
    const std::string req = BuildPluginRequest(L"action", L"gh", L"記 \"x\"", L"C:\\p", L"C:\\d", &item, &item);
    CHECK(req.find('\n') == req.size() - 1);  // compact, one line
    const auto j = Json::Parse(req);
    CHECK(j.has_value());
    if (j) {
        CHECK_EQ(j->Find("type")->AsString(), std::string("action"));
        CHECK_EQ(j->Find("query")->AsString(), std::string("\xE8\xA8\x98 \"x\""));
        CHECK(j->Find("item") && j->Find("action"));
        CHECK_EQ(j->Find("version")->AsNumber(), 1.0);
    }
    CHECK_WEQ(QuoteArgument(L"abc"), L"abc");
    CHECK_WEQ(QuoteArgument(L""), L"\"\"");
    CHECK_WEQ(QuoteArgument(L"a b"), L"\"a b\"");
    CHECK_WEQ(QuoteArgument(L"say \"hi\""), L"\"say \\\"hi\\\"\"");
    CHECK_WEQ(QuoteArgument(L"C:\\dir with space\\"), L"\"C:\\dir with space\\\\\"");
    CHECK_WEQ(QuoteArgument(L"a\\\\b c"), L"\"a\\\\b c\"");
    CHECK_WEQ(ExpandRunTemplate(L"tool.exe --q {query} --x", L"a b"), L"tool.exe --q \"a b\" --x");
    CHECK_WEQ(ExpandRunTemplate(L"tool.exe \"{query}\"", L"a b"), L"tool.exe \"a b\"");
    CHECK_WEQ(ExpandRunTemplate(L"tool.exe {QUERY}", L"x"), L"tool.exe x");
    auto [first, rest] = SplitCommandLine(L"  \"C:\\Program Files\\x.exe\" -a b");
    CHECK_WEQ(first, L"C:\\Program Files\\x.exe");
    CHECK_WEQ(rest, L"-a b");
    std::tie(first, rest) = SplitCommandLine(L"python main.py");
    CHECK_WEQ(first, L"python");
    CHECK_WEQ(rest, L"main.py");
    auto g = ParseGlyphSpec(L"glyph:E721:#3B82F6");
    CHECK(g && g->glyph == L"\uE721" && g->color == 0xFF3B82F6);
    CHECK(ParseGlyphSpec(L"GLYPH:e8a7") && ParseGlyphSpec(L"glyph:E8A7")->color == 0);
    CHECK(!ParseGlyphSpec(L"glyph:XYZ"));
    CHECK(!ParseGlyphSpec(L"icon.png"));
    CHECK(ParseColor(L"#80FF0000") == 0x80FF0000u);
    CHECK(!ParseColor(L"red"));
    CHECK(IsValidUtf8("plain"));
    CHECK(IsValidUtf8("\xE8\xA8\x98"));
    CHECK(!IsValidUtf8("\xB0\xF5"));  // cp950 bytes
    CHECK(!IsValidUtf8("\xE8\xA8"));
}

}  // namespace

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> suites = {
        {"text", TestText},       {"fuzzy", TestFuzzy},   {"calc", TestCalc},
        {"json", TestJson},       {"hotkey", TestHotkey}, {"query", TestQuery},
        {"settings", TestSettings}, {"usage", TestUsage}, {"edit", TestEditBuffer},
        {"chord", TestKeyChord},  {"favorites", TestFavorites}, {"manifest", TestPluginManifest},
        {"response", TestPluginResponse}, {"request", TestPluginRequestAndCommandLine},
    };
    for (const auto& [name, fn] : suites) {
        const int before = g_failures;
        fn();
        std::printf("%-9s %s\n", name, g_failures == before ? "ok" : "FAILED");
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
