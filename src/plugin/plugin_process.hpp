// Contract between the portable PluginManager and the platform process runner.
#pragma once

#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "plugin/plugin_manager.hpp"

namespace beam {

// Lets the manager stop a run from another thread (a newer query, CancelAll, shutdown).
struct PluginRunControl {
    std::mutex mutex;
    uint64_t ticket = 0;
    PluginRequestKind kind = PluginRequestKind::Query;
    bool cancelled = false;
    std::function<void()> kill;  // set by the runner while the process exists; called under `mutex`

    void Cancel() {
        std::lock_guard lock(mutex);
        cancelled = true;
        if (kill) kill();
    }
};

struct PluginLaunch {
    std::wstring display;      // command line as shown in logs
    std::wstring commandLine;  // Windows: the CreateProcess command line
    std::vector<std::wstring> argv;  // POSIX: argv, argv[0] resolved to a path when possible
    std::wstring directory;
    std::vector<std::pair<std::wstring, std::wstring>> environment;  // added to the inherited environment
    std::string request;
    uint32_t timeoutMs = 5000;
    bool killTree = true;      // false for command runs: programs they start may keep running
    std::wstring prepareError;  // set when the run line cannot be turned into a process
};

// Platform: resolves the plugin's "run" line (interpreters, {query} where safe) into `launch`.
void PreparePluginLaunch(const PluginCommand& cmd, const std::wstring& query, PluginLaunch& launch);
// Platform: runs to completion, timeout or cancellation and fills exitCode, stdout/stderr (UTF-8),
// timedOut, cancelled, launchError and durationMs.
void ExecutePluginLaunch(const PluginLaunch& launch, PluginRunControl& control, PluginRunRecord& record);

#ifndef _WIN32
// The user's terminal running `command` (empty: just a shell) in `directory`; empty when no
// terminal is installed. Tries xdg-terminal-exec, ptyxis, gnome-terminal, konsole, ... in order.
std::vector<std::string> TerminalArgv(const std::vector<std::string>& command, const std::string& directory);
// How a plugin "run" line is started (interpreters for scripts in `folder`, PATH lookup).
bool ResolvePluginArgv(const std::wstring& run, const std::wstring& folder, std::vector<std::string>* argv,
                       std::wstring* error);
#endif

// Filesystem helpers that keep non-ASCII paths intact on both platforms.
std::filesystem::path FsPath(const std::wstring& path);
std::wstring FromFsPath(const std::filesystem::path& path);
bool FsIsFile(const std::wstring& path);
bool FsIsDirectory(const std::wstring& path);
std::wstring FsFileName(const std::wstring& path);
std::wstring FsJoin(const std::wstring& dir, const std::wstring& name);

}  // namespace beam
