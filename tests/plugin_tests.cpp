// End-to-end tests of the POSIX plugin runner: real bash / python3 plugins in a temporary
// config folder. Build with -DBEAM_BUILD_TESTS=ON and run ./beam_plugin_tests (Linux).
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/json.hpp"
#include "core/text.hpp"
#include "plugin/plugin_manager.hpp"
#include "plugin/plugin_process.hpp"

using namespace beam;
namespace fs = std::filesystem;

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

#define CHECK(x) Report((x), #x, __FILE__, __LINE__)
#define CHECK_MSG(x, msg) Report((x), #x, __FILE__, __LINE__, msg)

std::string U8(const std::wstring& w) { return WideToUtf8(w); }

fs::path g_root;

std::string FindProgram(const char* name) {
    const char* path = std::getenv("PATH");
    std::string dirs = path ? path : "/usr/bin:/bin";
    size_t start = 0;
    while (start <= dirs.size()) {
        size_t end = dirs.find(':', start);
        if (end == std::string::npos) end = dirs.size();
        const std::string candidate = dirs.substr(start, end - start) + "/" + name;
        if (access(candidate.c_str(), X_OK) == 0) return candidate;
        start = end + 1;
    }
    return "";
}

void WriteFile(const fs::path& path, const std::string& text, bool executable = false) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
    if (executable) chmod(path.c_str(), 0755);
}

// A plugin folder with one command.
void MakePlugin(const std::string& folder, const std::string& manifest, const std::string& script,
                const std::string& scriptName, bool executable = true) {
    const fs::path dir = g_root / "plugins" / folder;
    WriteFile(dir / "plugin.json", manifest);
    if (!scriptName.empty()) WriteFile(dir / scriptName, script, executable);
}

const PluginCommand* Find(PluginManager& m, const std::string& id) {
    const PluginCommand* c = m.Find(Utf8ToWide(id));
    if (!c) std::printf("  (plugin %s not loaded)\n", id.c_str());
    return c;
}

std::string Describe(const PluginResult& r, const PluginRunRecord& rec) {
    return "failure=\"" + U8(r.failure) + "\" exit=" + std::to_string(rec.exitCode) + " stdout=\"" +
           rec.stdoutText.substr(0, 300) + "\" stderr=\"" + rec.stderrText.substr(0, 300) + "\" launch=\"" +
           U8(rec.launchError) + "\" cmd=\"" + U8(rec.commandLine) + "\"";
}

// Running (a killed process may linger as a zombie until its new parent reaps it).
bool ProcessAlive(pid_t pid) {
    if (pid <= 0 || kill(pid, 0) != 0) return false;
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    if (!std::getline(stat, line)) return false;
    const size_t paren = line.rfind(')');
    return paren == std::string::npos || paren + 2 >= line.size() || line[paren + 2] != 'Z';
}

void TestTemplates(PluginManager& m) {
    for (const PluginTemplate t : PlatformPluginTemplates()) {
        const bool python = t == PluginTemplate::Python, node = t == PluginTemplate::Node;
        if (python && FindProgram("python3").empty()) continue;
        if (node && FindProgram("node").empty()) {
            std::printf("  (node not installed: skipping the Node.js template run)\n");
            continue;
        }
        std::wstring folder, keyword, error;
        const std::wstring name = std::wstring(L"範本 ") + PluginTemplateLabel(t);
        CHECK_MSG(m.CreateFromTemplate(name, t, &folder, &keyword, &error), U8(error));
        {  // a cold start of node / python on a busy machine may take a few seconds
            const fs::path manifest = fs::path(U8(folder)) / "plugin.json";
            std::ifstream in(manifest);
            std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (auto json = Json::Parse(text); json && json->IsObject()) {
                json->Set("timeoutMs", 30000);
                WriteFile(manifest, json->Dump());
            }
        }
        m.Reload();
        const PluginCommand* cmd = nullptr;
        for (const auto& c : m.Commands())
            if (c.folder == folder) cmd = &c;
        CHECK_MSG(cmd != nullptr, U8(folder));
        if (!cmd) continue;
        PluginRunRecord rec;
        const PluginResult r = m.RunBlocking(*cmd, PluginRequestKind::Query, L"hello 世界 & \"quotes\"", nullptr,
                                             nullptr, &rec);
        CHECK_MSG(r.failure.empty() && r.response.json && !r.response.items.empty(), Describe(r, rec));
        bool echoed = false;
        for (const auto& item : r.response.items) echoed = echoed || item.title.find(L"世界") != std::wstring::npos;
        CHECK_MSG(echoed, "the template should echo the query: " + rec.stdoutText.substr(0, 400));
        // empty query
        const PluginResult empty = m.RunBlocking(*cmd, PluginRequestKind::Query, L"", nullptr, nullptr, &rec);
        CHECK_MSG(empty.failure.empty() && empty.response.json, Describe(empty, rec));
        if (t == PluginTemplate::Shell) {
            struct stat st {};
            CHECK(stat((U8(folder) + "/main.sh").c_str(), &st) == 0 && (st.st_mode & S_IXUSR));
        }
    }
}

void TestRequestAndEnvironment(PluginManager& m) {
    MakePlugin("envcheck",
               R"({"name":"Env","keyword":"envc","mode":"list","run":"check.sh {query}"})",
               "#!/bin/bash\n"
               "req=$(cat)\n"
               "printf '%s' \"$req\" > \"$BEAM_DATA_DIR/request.json\"\n"
               "printf '%s\\n' \"$@\" > \"$BEAM_DATA_DIR/argv.txt\"\n"
               "echo \"{\\\"items\\\":[{\\\"title\\\":\\\"ok\\\",\\\"subtitle\\\":\\\"$BEAM_REQUEST_TYPE\\\"}]}\"\n",
               "check.sh");
    m.Reload();
    const PluginCommand* cmd = Find(m, "envcheck");
    if (!cmd) return;
    const std::wstring query = L"a b; $(touch /tmp/beam-pwned) `id` 'q' \"dq\" 中文";
    PluginRunRecord rec;
    const PluginResult r = m.RunBlocking(*cmd, PluginRequestKind::Query, query, nullptr, nullptr, &rec);
    CHECK_MSG(r.failure.empty() && r.response.items.size() == 1, Describe(r, rec));
    if (!r.response.items.empty()) CHECK(r.response.items[0].subtitle == L"query");
    const fs::path data = g_root / "plugin-data" / "envcheck";
    std::ifstream argvFile(data / "argv.txt");
    std::string line, all;
    std::vector<std::string> args;
    while (std::getline(argvFile, line)) args.push_back(line);
    CHECK_MSG(args.size() == 1 && args[0] == U8(query), "{query} must be exactly one argument, got " +
                                                           std::to_string(args.size()));
    CHECK(!fs::exists("/tmp/beam-pwned"));
    std::ifstream reqFile(data / "request.json");
    std::string request((std::istreambuf_iterator<char>(reqFile)), std::istreambuf_iterator<char>());
    const auto json = Json::Parse(request);
    CHECK_MSG(json && json->IsObject(), request);
    if (json && json->IsObject()) {
        const Json* q = json->Find("query");
        CHECK(q && q->IsString() && q->AsString() == U8(query));
        const Json* type = json->Find("type");
        CHECK(type && type->IsString() && type->AsString() == "query");
    }
}

void TestQueryNotPassedToInlineCode(PluginManager& m) {
    // bash -c '...{query}...' would run the query as shell code: Beam leaves it out.
    MakePlugin("inline", R"({"name":"Inline","mode":"list","run":"bash -c 'echo \"{\\\"items\\\":[{\\\"title\\\":\\\"[{query}]\\\"}]}\"'"})",
               "", "");
    m.Reload();
    const PluginCommand* cmd = Find(m, "inline");
    if (!cmd) return;
    PluginRunRecord rec;
    const PluginResult r = m.RunBlocking(*cmd, PluginRequestKind::Query, L"$(id)", nullptr, nullptr, &rec);
    CHECK_MSG(r.failure.empty() && r.response.items.size() == 1, Describe(r, rec));
    if (!r.response.items.empty()) CHECK_MSG(r.response.items[0].title == L"[]", U8(r.response.items[0].title));
}

// Which arguments get the typed text: never one that is program code (bash -c CODE...).
void TestQueryInCodeArguments() {
    const std::wstring query = L"Q'\"$(id)";
    struct Case {
        const wchar_t* run;
        int argIndex;    // argument to look at
        bool hasQuery;   // whether the query reaches it
    };
    const Case cases[] = {
        {L"bash -c 'echo {query}'", 2, false},
        {L"timeout 5 bash -c 'echo {query}'", 4, false},
        {L"bash -ce 'echo {query}'", 2, false},
        {L"bash -o pipefail -c 'echo {query}'", 4, false},
        {L"env FOO=1 sh -c 'echo {query}'", 4, false},
        {L"bash -c 'printf %s \"$1\"' _ {query}", 4, true},
        {L"python3 -Ic 'print(\"{query}\")'", 2, false},
        {L"python3 -c'print(\"{query}\")'", 1, false},
        {L"python3 -m json.tool {query}", 3, true},
        {L"perl -le 'print \"{query}\"'", 2, false},
        {L"perl -e 'print @ARGV' {query}", 3, true},
        {L"node -pe '\"{query}\"'", 2, false},
        {L"node -e 'console.log(process.argv[1])' {query}", 3, true},
        {L"awk 'BEGIN{print \"{query}\"}'", 1, false},
        {L"awk '{print}' {query}", 2, true},
        {L"sed -n 's/x/{query}/p'", 2, false},
        {L"sed -e s/a/b/ {query}", 3, true},
        {L"grep -r {query} .", 2, true},
        {L"bash script.sh {query}", 2, true},
    };
    for (const auto& c : cases) {
        PluginCommand cmd;
        cmd.spec.id = L"q";
        cmd.spec.run = c.run;
        cmd.folder = Utf8ToWide(g_root.string());
        PluginLaunch launch;
        PreparePluginLaunch(cmd, query, launch);
        if (!launch.prepareError.empty()) {
            if (U8(launch.prepareError).find("node") != std::string::npos && FindProgram("node").empty()) continue;
            CHECK_MSG(false, U8(c.run) + ": " + U8(launch.prepareError));
            continue;
        }
        const bool ok = c.argIndex < static_cast<int>(launch.argv.size()) &&
                        (launch.argv[c.argIndex].find(query) != std::wstring::npos) == c.hasQuery;
        std::string shown;
        for (const auto& a : launch.argv) shown += "[" + U8(a) + "]";
        CHECK_MSG(ok, U8(c.run) + " -> " + shown);
    }
}

void TestFailures(PluginManager& m) {
    MakePlugin("exit3", R"({"name":"Exit","mode":"list","run":"fail.sh"})",
               "#!/bin/sh\necho 'something broke' >&2\nexit 3\n", "fail.sh");
    MakePlugin("noexec", R"({"name":"NoExec","mode":"list","run":"tool"})", "#!/bin/sh\necho '{}'\n", "tool", false);
    MakePlugin("missing", R"({"name":"Missing","mode":"list","run":"no-such-program-xyz --flag"})", "", "");
    MakePlugin("badjson", R"({"name":"BadJson","mode":"list","run":"bad.sh"})", "#!/bin/sh\necho '{not json'\n",
               "bad.sh");
    MakePlugin("shebang", R"({"name":"Shebang","mode":"list","run":"script"})",
               "#!/usr/bin/env bash\necho '{\"items\":[{\"title\":\"via shebang\"}]}'\n", "script", false);
    m.Reload();
    PluginRunRecord rec;
    if (const PluginCommand* c = Find(m, "exit3")) {
        const PluginResult r = m.RunBlocking(*c, PluginRequestKind::Query, L"", nullptr, nullptr, &rec);
        CHECK_MSG(!r.failure.empty() && rec.exitCode == 3, Describe(r, rec));
        CHECK_MSG(r.failure.find(L"something broke") != std::wstring::npos, U8(r.failure));
    }
    if (const PluginCommand* c = Find(m, "noexec")) {  // no extension, no exec bit, but a shebang: runs
        const PluginResult r = m.RunBlocking(*c, PluginRequestKind::Query, L"", nullptr, nullptr, &rec);
        CHECK_MSG(r.failure.empty(), Describe(r, rec));
    }
    if (const PluginCommand* c = Find(m, "shebang")) {
        const PluginResult r = m.RunBlocking(*c, PluginRequestKind::Query, L"", nullptr, nullptr, &rec);
        CHECK_MSG(r.failure.empty() && r.response.items.size() == 1, Describe(r, rec));
    }
    if (const PluginCommand* c = Find(m, "missing")) {
        const PluginResult r = m.RunBlocking(*c, PluginRequestKind::Query, L"", nullptr, nullptr, &rec);
        CHECK_MSG(r.failure.find(L"找不到程式") != std::wstring::npos, U8(r.failure));
    }
    if (const PluginCommand* c = Find(m, "badjson")) {
        const PluginResult r = m.RunBlocking(*c, PluginRequestKind::Query, L"", nullptr, nullptr, &rec);
        CHECK_MSG(!r.failure.empty(), Describe(r, rec));
    }
    // a script without exec bit and without shebang
    MakePlugin("plain", R"({"name":"Plain","mode":"list","run":"plain"})", "echo hi\n", "plain", false);
    m.Reload();
    if (const PluginCommand* c = Find(m, "plain")) {
        const PluginResult r = m.RunBlocking(*c, PluginRequestKind::Query, L"", nullptr, nullptr, &rec);
        CHECK_MSG(r.failure.find(L"chmod") != std::wstring::npos, U8(r.failure));
    }
}

void TestTimeoutKillsTree(PluginManager& m) {
    const fs::path pidFile = g_root / "grandchild.pid";
    MakePlugin("slow", R"({"name":"Slow","mode":"list","run":"slow.sh","timeoutMs":800})",
               "#!/bin/bash\nsleep 60 &\necho $! > '" + pidFile.string() + "'\nsleep 60\n", "slow.sh");
    m.Reload();
    const PluginCommand* c = Find(m, "slow");
    if (!c) return;
    PluginRunRecord rec;
    const auto start = std::chrono::steady_clock::now();
    const PluginResult r = m.RunBlocking(*c, PluginRequestKind::Query, L"", nullptr, nullptr, &rec);
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    CHECK_MSG(rec.timedOut && !r.failure.empty(), Describe(r, rec));
    CHECK_MSG(ms < 3000, std::to_string(ms) + " ms");
    std::ifstream in(pidFile);
    pid_t grandchild = 0;
    in >> grandchild;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK_MSG(grandchild > 0 && !ProcessAlive(grandchild), "grandchild " + std::to_string(grandchild) + " survived");
}

void TestBigOutputAndUnreadStdin(PluginManager& m) {
    MakePlugin("big", R"({"name":"Big","mode":"list","run":"big.py"})",
               "import json, sys\nreq = json.load(sys.stdin)\n"
               "items = [{'title': 'item %d ' % i + 'x' * 200} for i in range(8000)]\n"
               "print(json.dumps({'items': items}))\n",
               "big.py");
    // exits at once without reading its (large) request: Beam must not die of SIGPIPE
    MakePlugin("deaf", R"({"name":"Deaf","mode":"list","run":"deaf.sh"})",
               "#!/bin/sh\necho '{\"items\":[{\"title\":\"deaf\"}]}'\n", "deaf.sh");
    m.Reload();
    PluginRunRecord rec;
    if (!FindProgram("python3").empty()) {
        if (const PluginCommand* c = Find(m, "big")) {
            const PluginResult r = m.RunBlocking(*c, PluginRequestKind::Query, L"x", nullptr, nullptr, &rec);
            CHECK_MSG(r.failure.empty() && r.response.items.size() == 500, Describe(r, rec));  // Beam keeps 500
        }
    }
    if (const PluginCommand* c = Find(m, "deaf")) {
        const std::wstring huge(3'000'000, L'q');
        const PluginResult r = m.RunBlocking(*c, PluginRequestKind::Query, huge, nullptr, nullptr, &rec);
        CHECK_MSG(r.failure.empty() && r.response.items.size() == 1, Describe(r, rec));
    }
}

void TestAsyncCancel(PluginManager& m) {
    MakePlugin("sleepy", R"({"name":"Sleepy","mode":"list","run":"sleepy.sh {query}","timeoutMs":5000})",
               "#!/bin/bash\nsleep \"$1\"\necho \"{\\\"items\\\":[{\\\"title\\\":\\\"slept $1\\\"}]}\"\n", "sleepy.sh");
    m.Reload();
    const PluginCommand* c = Find(m, "sleepy");
    if (!c) return;
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::unique_ptr<PluginResult>> results;
    PluginResultSink sink = [&](std::unique_ptr<PluginResult> r) {
        std::lock_guard lock(mutex);
        results.push_back(std::move(r));
        cv.notify_all();
    };
    const uint64_t first = m.Start(sink, *c, PluginRequestKind::Query, L"3");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const uint64_t second = m.Start(sink, *c, PluginRequestKind::Query, L"0.1");  // supersedes the first
    {
        std::unique_lock lock(mutex);
        cv.wait_for(lock, std::chrono::seconds(10), [&] { return !results.empty(); });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    std::lock_guard lock(mutex);
    CHECK_MSG(results.size() == 1, std::to_string(results.size()) + " results");
    if (!results.empty()) {
        CHECK(results[0]->ticket == second && results[0]->ticket != first);
        CHECK_MSG(results[0]->failure.empty() && results[0]->response.items.size() == 1, U8(results[0]->failure));
    }
}

void TestCommandAndCallback(PluginManager& m) {
    MakePlugin("cmd", R"({"name":"Cmd","mode":"command","run":"cmd.sh"})",
               "#!/bin/bash\nreq=$(cat)\n"
               "if [[ \"$BEAM_REQUEST_TYPE\" == action ]]; then echo '{\"message\":\"callback ok\"}'; exit 0; fi\n"
               "echo \"plain text from $BEAM_REQUEST_TYPE\"\n",
               "cmd.sh");
    m.Reload();
    const PluginCommand* c = Find(m, "cmd");
    if (!c) return;
    PluginRunRecord rec;
    const PluginResult r = m.RunBlocking(*c, PluginRequestKind::Command, L"arg", nullptr, nullptr, &rec);
    CHECK_MSG(r.failure.empty() && !r.response.json && r.response.output.find(L"plain text from run") == 0,
              Describe(r, rec));
    Json item = Json::MakeObject();
    item.Set("title", Json(std::string("x")));
    Json action = Json::MakeObject();
    action.Set("type", Json(std::string("callback")));
    action.Set("id", Json(std::string("do-it")));
    const PluginResult cb = m.RunBlocking(*c, PluginRequestKind::Action, L"", &item, &action, &rec);
    CHECK_MSG(cb.failure.empty() && cb.response.message == L"callback ok", Describe(cb, rec));
    const auto req = Json::Parse(rec.request);
    CHECK(req && req->Find("action") && req->Find("item"));
}

void TestResolution() {
    const std::wstring folder = Utf8ToWide((g_root / "plugins" / "envcheck").string());
    std::vector<std::string> argv;
    std::wstring error;
    CHECK(ResolvePluginArgv(L"check.sh --x", folder, &argv, &error));
    CHECK(argv.size() == 3 && argv[0].find("bash") != std::string::npos && argv[2] == "--x");
    CHECK(ResolvePluginArgv(L"python3 -V", L"", &argv, &error) || FindProgram("python3").empty());
    CHECK(!ResolvePluginArgv(L"'unbalanced", L"", &argv, &error) && error.find(L"引號") != std::wstring::npos);
    CHECK(!ResolvePluginArgv(L"", L"", &argv, &error));
    const std::wstring display = ResolveCommandLine(L"check.sh 'a b'", folder);
    CHECK_MSG(display.find(L"'a b'") != std::wstring::npos, U8(display));
}

void TestSpawnDetached() {
    const fs::path marker = g_root / "detached.txt";
    std::wstring error;
    CHECK_MSG(SpawnDetached(L"touch " + Utf8ToWide(marker.string()), Utf8ToWide(g_root.string()), false, &error),
              U8(error));
    for (int i = 0; i < 50 && !fs::exists(marker); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(fs::exists(marker));
    CHECK(!SpawnDetached(L"no-such-program-xyz", L"", false, &error));
}

}  // namespace

int main() {
    signal(SIGPIPE, SIG_IGN);  // as in Beam
    char tmpl[] = "/tmp/beam-plugin-tests-XXXXXX";
    if (!mkdtemp(tmpl)) return 2;
    g_root = tmpl;
    PluginManager manager;
    manager.Initialize(Utf8ToWide(g_root.string()));

    TestTemplates(manager);
    TestRequestAndEnvironment(manager);
    TestQueryNotPassedToInlineCode(manager);
    TestQueryInCodeArguments();
    TestFailures(manager);
    TestTimeoutKillsTree(manager);
    TestBigOutputAndUnreadStdin(manager);
    TestAsyncCancel(manager);
    TestCommandAndCallback(manager);
    TestResolution();
    TestSpawnDetached();

    std::error_code ec;
    fs::remove_all(g_root, ec);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
