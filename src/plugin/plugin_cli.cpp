// beam-plugin: command-line companion for writing Beam plugins (Windows and Linux).
//
//   beam-plugin new <name> [--lang python|node|powershell|shell]
//   beam-plugin list
//   beam-plugin run <plugin> [query...] [--command <id>] [--type query|run] [--callback <item>.<action>] [--raw]
//   beam-plugin doc | dir
#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <cstdio>
#endif

#include <cwchar>
#include <string>
#include <vector>

#include "core/hotkey.hpp"
#include "core/text.hpp"
#include "plugin/plugin_manager.hpp"
#include "plugin/plugin_process.hpp"

using namespace beam;

namespace {

#ifdef _WIN32
class Console {
public:
    Console() {
        out_ = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        isConsole_ = out_ && out_ != INVALID_HANDLE_VALUE && GetConsoleMode(out_, &mode);
    }
    void Write(const std::wstring& s) const {
        if (!out_ || out_ == INVALID_HANDLE_VALUE) return;
        DWORD n = 0;
        if (isConsole_) {
            WriteConsoleW(out_, s.c_str(), static_cast<DWORD>(s.size()), &n, nullptr);
        } else {
            const std::string u = WideToUtf8(s);  // redirected: UTF-8
            WriteFile(out_, u.data(), static_cast<DWORD>(u.size()), &n, nullptr);
        }
    }

private:
    HANDLE out_ = nullptr;
    bool isConsole_ = false;
};
#else
class Console {
public:
    void Write(const std::wstring& s) const {
        const std::string u = WideToUtf8(s);
        std::fwrite(u.data(), 1, u.size(), stdout);
        std::fflush(stdout);
    }
};
#endif

const Console& Out() {
    static Console console;
    return console;
}

void Print(const std::wstring& line = {}) { Out().Write(line + L"\n"); }

std::wstring LanguageList() {
    std::wstring list;
    for (const auto t : PlatformPluginTemplates()) {
        if (!list.empty()) list += L"|";
        switch (t) {
            case PluginTemplate::Python: list += L"python"; break;
            case PluginTemplate::Node: list += L"node"; break;
            case PluginTemplate::PowerShell: list += L"powershell"; break;
            case PluginTemplate::Shell: list += L"shell"; break;
        }
    }
    return list;
}

int Usage() {
    Print(L"beam-plugin — Beam 外掛開發工具\n");
    Print(L"  beam-plugin new <名稱> [--lang " + LanguageList() + L"]");
    Print(L"      在外掛資料夾建立可直接執行的範本");
    Print(L"  beam-plugin list");
    Print(L"      列出已安裝的外掛指令與 plugin.json 的問題");
    Print(L"  beam-plugin run <外掛> [查詢文字] [--command <id>] [--type query|run] [--callback <項目>.<動作>] [--raw]");
    Print(L"      用和 Beam 一樣的方式執行一次，顯示解析後的結果；<外掛> 可以是資料夾名稱、id 或路徑");
    Print(L"  beam-plugin doc     顯示協定說明（PLUGINS.md）");
    Print(L"  beam-plugin dir     顯示外掛資料夾位置");
    return 2;
}

bool IsAbsolutePathLike(const std::wstring& p) { return !p.empty() && FsPath(p).is_absolute(); }

bool IsIconSpec(const std::wstring& icon) {
    return ParseGlyphSpec(icon).has_value() || StartsWithIgnoreCase(icon, L"icon:");
}

std::wstring ModeName(PluginMode mode) { return mode == PluginMode::List ? L"list" : L"command"; }

std::wstring ActionLine(const PluginAction& a, bool primary) {
    std::wstring kind;
    switch (a.kind) {
        case PluginActionKind::Open: kind = L"open"; break;
        case PluginActionKind::Copy: kind = L"copy"; break;
        case PluginActionKind::Run: kind = a.terminal ? L"run (terminal)" : L"run"; break;
        case PluginActionKind::SetQuery: kind = L"setQuery"; break;
        case PluginActionKind::Callback: kind = L"callback"; break;
        default: kind = L"?"; break;
    }
    std::wstring line = (primary ? L"     ↵ " : L"     • ") + a.title + L"  [" + kind + L"] " + a.value;
    if (!a.shortcut.empty()) {
        line += ParseKeyChord(a.shortcut) ? L"  (" + a.shortcut + L")" : L"  (無效的 shortcut：" + a.shortcut + L")";
    }
    return line;
}

void PrintResult(const PluginCommand& cmd, const PluginRunRecord& rec, const PluginResult& result, bool raw) {
    Print(L"> " + rec.commandLine);
    Print(L"  耗時 " + std::to_wstring(rec.durationMs) + L" ms，結束代碼 " +
          std::to_wstring(static_cast<long>(rec.exitCode)) + (rec.timedOut ? L"（逾時）" : L""));
    if (!rec.stderrText.empty()) {
        Print(L"\n[stderr]");
        Print(Trim(Utf8ToWide(rec.stderrText)));
    }
    const PluginResponse& r = result.response;
    if (raw || (!r.json && !rec.stdoutText.empty())) {
        Print(L"\n[stdout 原文]");
        Print(Utf8ToWide(rec.stdoutText));
    }
    Print();
    if (!result.failure.empty()) {
        Print(L"✗ " + result.failure);
        return;
    }
    if (!r.json) {
        if (cmd.spec.mode == PluginMode::Command)
            Print(L"✓ 非 JSON 輸出（command 模式會當成訊息 / 詳細檢視顯示）");
        else
            Print(L"✓ 沒有輸出（清單為空）");
        return;
    }
    if (r.hasItems) {
        Print(L"✓ " + std::to_wstring(r.items.size()) + L" 筆結果");
        for (size_t i = 0; i < r.items.size(); ++i) {
            const PluginItem& it = r.items[i];
            std::wstring head = L"  " + std::to_wstring(i + 1) + L". " + it.title;
            if (!it.subtitle.empty()) head += L" — " + it.subtitle;
            if (!it.accessory.empty()) head += L"  [" + it.accessory + L"]";
            Print(head);
            if (!it.icon.empty() && !IsIconSpec(it.icon) && !IsAbsolutePathLike(it.icon) &&
                !FsIsFile(FsJoin(cmd.folder, it.icon))) {
                Print(L"     ! 找不到圖示檔：" + it.icon);
            }
            if (it.actions.empty()) Print(L"     ! 沒有動作：按 Enter 不會有反應");
            for (size_t k = 0; k < it.actions.size(); ++k) Print(ActionLine(it.actions[k], k == 0));
        }
    } else {
        Print(L"✓ JSON 回應（沒有 items）");
    }
    if (r.directive.kind != PluginActionKind::None) Print(ActionLine(r.directive, true));
    if (!r.message.empty()) Print(L"  message: " + r.message);
    if (!r.error.empty()) Print(L"  error:   " + r.error);
    if (!r.output.empty()) Print(L"  output:  " + std::to_wstring(r.output.size()) + L" 字元（詳細檢視）");
    if (r.hide) Print(std::wstring(L"  hide:    ") + (*r.hide ? L"true" : L"false"));
}

std::vector<PluginCommand> ResolvePlugin(PluginManager& mgr, const std::wstring& arg) {
    std::vector<std::wstring> problems;
    std::wstring folder;
    if (FsIsDirectory(arg)) folder = arg;
    else if (FsIsDirectory(FsJoin(mgr.Directory(), arg))) folder = FsJoin(mgr.Directory(), arg);
    if (!folder.empty()) {
        auto cmds = PluginManager::LoadFolder(folder, &problems);
        for (const auto& p : problems) Print(L"! " + p);
        if (cmds.empty() && problems.empty()) Print(L"! 找不到 " + FsJoin(folder, L"plugin.json"));
        return cmds;
    }
    mgr.Reload();
    std::vector<PluginCommand> matches;
    for (const auto& c : mgr.Commands()) {
        if (EqualsIgnoreCase(c.spec.id, arg) || EqualsIgnoreCase(c.spec.keyword, arg) ||
            EqualsIgnoreCase(c.spec.name, arg))
            matches.push_back(c);
    }
    if (matches.empty()) Print(L"! 找不到外掛「" + arg + L"」。用 beam-plugin list 看已安裝的外掛。");
    return matches;
}

int Run(const std::vector<std::wstring>& args) {
    std::wstring target, commandId, type, callback;
    bool raw = false;
    std::vector<std::wstring> words;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::wstring& a = args[i];
        if (a == L"--command" && i + 1 < args.size()) commandId = args[++i];
        else if (a == L"--type" && i + 1 < args.size()) type = args[++i];
        else if (a == L"--callback" && i + 1 < args.size()) callback = args[++i];
        else if (a == L"--raw") raw = true;
        else if (target.empty()) target = a;
        else words.push_back(a);
    }
    if (target.empty()) return Usage();
    std::wstring query;
    for (const auto& w : words) query += (query.empty() ? L"" : L" ") + w;

    PluginManager mgr;
    mgr.Initialize(PlatformConfigDirectory());
    auto commands = ResolvePlugin(mgr, target);
    if (commands.empty()) return 1;
    const PluginCommand* cmd = &commands.front();
    if (!commandId.empty()) {
        cmd = nullptr;
        for (const auto& c : commands) {
            const std::wstring local = c.spec.id.substr(c.spec.id.find(L'/') + 1);
            if (EqualsIgnoreCase(c.spec.id, commandId) || EqualsIgnoreCase(local, commandId)) cmd = &c;
        }
        if (!cmd) {
            Print(L"! 沒有 id 為「" + commandId + L"」的指令");
            return 1;
        }
    } else if (commands.size() > 1) {
        Print(L"（這個外掛有 " + std::to_wstring(commands.size()) + L" 個指令，執行第一個；用 --command 指定）");
    }
    PluginRequestKind kind = cmd->spec.mode == PluginMode::Command ? PluginRequestKind::Command : PluginRequestKind::Query;
    if (type == L"run") kind = PluginRequestKind::Command;
    else if (type == L"query") kind = PluginRequestKind::Query;

    Print(L"外掛：" + cmd->spec.name + L"（" + cmd->spec.id + L"，" + ModeName(cmd->spec.mode) + L"）  查詢：「" + query + L"」");
    PluginRunRecord rec;
    PluginResult result = mgr.RunBlocking(*cmd, kind, query, nullptr, nullptr, &rec);
    PrintResult(*cmd, rec, result, raw);
    if (!result.failure.empty()) return 1;

    if (!callback.empty()) {
        const size_t dot = callback.find(L'.');
        const int itemIndex = static_cast<int>(std::wcstol(callback.substr(0, dot).c_str(), nullptr, 10)) - 1;
        const int actionIndex =
            dot == std::wstring::npos ? 0 : static_cast<int>(std::wcstol(callback.substr(dot + 1).c_str(), nullptr, 10)) - 1;
        const auto& items = result.response.items;
        if (itemIndex < 0 || itemIndex >= static_cast<int>(items.size()) || actionIndex < 0 ||
            actionIndex >= static_cast<int>(items[itemIndex].actions.size())) {
            Print(L"! --callback 超出範圍（格式：項目編號.動作編號，例如 2.1）");
            return 1;
        }
        const PluginItem& item = items[itemIndex];
        const PluginAction& action = item.actions[actionIndex];
        if (action.kind != PluginActionKind::Callback) {
            Print(L"! 第 " + std::to_wstring(itemIndex + 1) + L" 筆的第 " + std::to_wstring(actionIndex + 1) +
                  L" 個動作不是 callback");
            return 1;
        }
        Print(L"\n── callback「" + action.value + L"」");
        PluginRunRecord rec2;
        PluginResult second = mgr.RunBlocking(*cmd, PluginRequestKind::Action, query, &item.raw, &action.raw, &rec2);
        PrintResult(*cmd, rec2, second, raw);
        if (!second.failure.empty()) return 1;
    }
    return 0;
}

int New(const std::vector<std::wstring>& args) {
    std::wstring name;
    PluginTemplate language = PluginTemplate::Python;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == L"--lang" && i + 1 < args.size()) {
            const std::wstring lang = Fold(args[++i]);
            bool found = true;
            if (lang == L"node" || lang == L"js" || lang == L"javascript") language = PluginTemplate::Node;
            else if (lang == L"powershell" || lang == L"ps" || lang == L"ps1") language = PluginTemplate::PowerShell;
            else if (lang == L"shell" || lang == L"sh" || lang == L"bash") language = PluginTemplate::Shell;
            else if (lang == L"python" || lang == L"py") language = PluginTemplate::Python;
            else found = false;
            bool available = false;
            for (const auto t : PlatformPluginTemplates()) available = available || t == language;
            if (!found || !available) {
                Print(L"! 不支援的語言：" + args[i] + L"（" + LanguageList() + L"）");
                return 2;
            }
        } else {
            name += (name.empty() ? L"" : L" ") + args[i];
        }
    }
    if (Trim(name).empty()) return Usage();
    PluginManager mgr;
    mgr.Initialize(PlatformConfigDirectory());
    mgr.Reload();
    std::wstring folder, keyword, error;
    if (!mgr.CreateFromTemplate(name, language, &folder, &keyword, &error)) {
        Print(L"✗ 建立失敗：" + error);
        return 1;
    }
    Print(L"✓ 已建立 " + folder);
    Print(L"  在 Beam 輸入「" + keyword + L" 」（關鍵字加空白）就能使用，存檔後自動重新載入。");
    Print(L"  測試：beam-plugin run " + FsFileName(folder) + L" \"hello\"");
    return 0;
}

int List() {
    PluginManager mgr;
    mgr.Initialize(PlatformConfigDirectory());
    mgr.Reload();
    Print(L"外掛資料夾：" + mgr.Directory());
    if (mgr.Commands().empty()) Print(L"（沒有外掛，用 beam-plugin new <名稱> 建立一個）");
    for (const auto& c : mgr.Commands()) {
        std::wstring line = L"  " + c.spec.id + L"  「" + c.spec.name + L"」  " + ModeName(c.spec.mode);
        if (!c.spec.keyword.empty()) line += L"  keyword: " + c.spec.keyword;
        if (c.spec.filtering == PluginFiltering::Beam) line += L"  filtering: beam";
        Print(line);
        Print(L"      run: " + c.spec.run + L"  →  " + ResolveCommandLine(c.spec.run, c.folder));
    }
    if (!mgr.Problems().empty()) {
        Print(L"\n問題：");
        for (const auto& p : mgr.Problems()) Print(L"  ! " + p);
        return 1;
    }
    return 0;
}

int CliMain(std::vector<std::wstring> args) {
    if (args.empty()) return Usage();
    const std::wstring verb = Fold(args.front());
    args.erase(args.begin());
    if (verb == L"new") return New(args);
    if (verb == L"run" || verb == L"test") return Run(args);
    if (verb == L"list" || verb == L"ls") return List();
    if (verb == L"doc" || verb == L"docs") {
        Out().Write(Utf8ToWide(PluginDocumentation()));
        return 0;
    }
    if (verb == L"dir") {
        PluginManager mgr;
        mgr.Initialize(PlatformConfigDirectory());
        Print(mgr.Directory());
        return 0;
    }
    return Usage();
}

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) { return CliMain(std::vector<std::wstring>(argv + 1, argv + argc)); }
#else
int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);  // a plugin that exits without reading stdin must not kill us
    std::vector<std::wstring> args;
    for (int i = 1; i < argc; ++i) args.push_back(Utf8ToWide(argv[i]));
    return CliMain(std::move(args));
}
#endif
