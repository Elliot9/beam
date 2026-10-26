// Plugin loading and execution, shared by the Windows and Linux front ends.
//
// A plugin is a folder with plugin.json and any program. Each run is a child
// process: stdin gets the JSON request, stdout/stderr are captured, and the
// whole process tree is killed on timeout or when a newer query supersedes it
// (Windows: job object, Linux: process group). The platform part lives in
// win/plugin_process.cpp and linux/plugin_process.cpp.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/plugin_protocol.hpp"

namespace beam {

struct PluginCommand {
    PluginCommandSpec spec;
    std::wstring pluginName;
    std::wstring folder;        // absolute plugin folder
    std::wstring manifestPath;  // .../plugin.json
};

enum class PluginRequestKind { Query, Command, Action };

// Starter templates. Windows offers Python / Node / PowerShell, Linux Python / Node / Shell.
enum class PluginTemplate { Python, Node, PowerShell, Shell };

struct PluginRunRecord {
    std::wstring commandId;
    std::wstring commandName;
    std::wstring commandLine;
    std::wstring kind;
    std::wstring query;
    std::string request;
    std::string stdoutText;  // UTF-8
    std::string stderrText;
    long exitCode = 0;
    uint64_t durationMs = 0;
    bool timedOut = false;
    bool cancelled = false;
    std::wstring launchError;
    std::wstring when;  // local time, "HH:MM:SS"
};

struct PluginResult {
    uint64_t ticket = 0;
    PluginRequestKind kind = PluginRequestKind::Query;
    std::wstring commandId;
    std::wstring query;
    PluginResponse response;
    std::wstring failure;  // non-empty: did not start, timed out, crashed, or produced unusable output
};

// Receives finished runs on a worker thread; the front end forwards them to its UI thread.
using PluginResultSink = std::function<void(std::unique_ptr<PluginResult>)>;

class PluginManager {
public:
    using Template = PluginTemplate;

    PluginManager();
    ~PluginManager();
    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    void Initialize(const std::wstring& configDirectory);
    void SetLog(std::function<void(const std::wstring&)> log) { log_ = std::move(log); }
    const std::wstring& Directory() const { return dir_; }
    std::wstring DataDirectory(const PluginCommand& cmd) const;

    void Reload();
    bool ReloadIfChanged();  // cheap; true when the set of manifests changed
    const std::vector<PluginCommand>& Commands() const { return commands_; }
    const std::vector<std::wstring>& Problems() const { return problems_; }
    const PluginCommand* Find(const std::wstring& id) const;
    const PluginCommand* FindByKeyword(std::wstring_view keyword) const;

    // Asynchronous. A new Query cancels older Query runs (typing supersedes them).
    // Cancelled runs are not delivered.
    uint64_t Start(PluginResultSink sink, const PluginCommand& cmd, PluginRequestKind kind, const std::wstring& query,
                   const Json* item = nullptr, const Json* action = nullptr);
    void Cancel(uint64_t ticket);
    void CancelAll();  // command runs are left alone: they may have started programs the user keeps

    // Synchronous, for the beam-plugin CLI.
    PluginResult RunBlocking(const PluginCommand& cmd, PluginRequestKind kind, const std::wstring& query,
                             const Json* item, const Json* action, PluginRunRecord* record);

    std::vector<PluginRunRecord> Records() const;  // newest first
    std::wstring DescribeRecords() const;           // for the debug view

    bool CreateFromTemplate(const std::wstring& name, PluginTemplate language, std::wstring* folder,
                            std::wstring* keyword, std::wstring* error);
    void EnsureDocumentation() const;  // writes PLUGINS.md into the plugins folder

    static std::vector<PluginCommand> LoadFolder(const std::wstring& folder, std::vector<std::wstring>* problems);

private:
    struct Shared;
    std::wstring Signature() const;
    void Log(const std::wstring& text) const {
        if (log_) log_(text);
    }

    std::shared_ptr<Shared> shared_;
    std::wstring dir_;
    std::wstring dataRoot_;
    std::vector<PluginCommand> commands_;
    std::vector<std::wstring> problems_;
    std::wstring signature_;
    std::atomic<uint64_t> nextTicket_{1};
    std::function<void(const std::wstring&)> log_;
};

// --- templates (plugin/plugin_templates.cpp) -------------------------------
const wchar_t* PluginScriptFileName(PluginTemplate language);  // main.py / main.js / main.ps1 / main.sh
std::string PluginScriptTemplate(PluginTemplate language, const std::wstring& name);
std::string PluginManifestTemplate(PluginTemplate language, const std::wstring& name, const std::wstring& keyword);
std::vector<PluginTemplate> PlatformPluginTemplates();
const wchar_t* PluginTemplateLabel(PluginTemplate language);  // "Python", "Node.js", ...

// --- platform (win/plugin_process.cpp, linux/plugin_process.cpp) -----------
// How a plugin "run" line is actually started (interpreters for .py/.js/...), for display.
std::wstring ResolveCommandLine(const std::wstring& run, const std::wstring& folder);
// Fire-and-forget process for "run" actions (hidden, or in a terminal window).
bool SpawnDetached(const std::wstring& commandLine, const std::wstring& directory, bool visibleConsole,
                   std::wstring* error);
const char* PluginDocumentation();       // PLUGINS.md contents
std::wstring PlatformConfigDirectory();  // %APPDATA%\Beam or ~/.config/beam

}  // namespace beam
