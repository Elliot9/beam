// Linux side of the plugin runner: posix_spawn into a new process group, one poll()
// loop for stdin/stdout/stderr, and the whole group killed on timeout / cancel.
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/text.hpp"
#include "plugin/plugin_process.hpp"

extern char** environ;
extern const unsigned char kBeamPluginsDoc[];  // generated from PLUGINS.md

namespace beam {
namespace {

constexpr size_t kMaxStdout = 8u << 20;
constexpr size_t kMaxStderr = 1u << 20;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

std::string U8(const std::wstring& w) { return WideToUtf8(w); }

void IgnoreSigpipeOnce() {
    static std::once_flag once;
    std::call_once(once, [] {
        struct sigaction current {};
        if (sigaction(SIGPIPE, nullptr, &current) == 0 && current.sa_handler == SIG_DFL) signal(SIGPIPE, SIG_IGN);
    });
}

std::string HomeDirectory() {
    if (const char* home = std::getenv("HOME"); home && *home) return home;
    if (const passwd* pw = getpwuid(getuid())) return pw->pw_dir;
    return "/";
}

// POSIX-shell-like word splitting: quotes and backslashes, no expansion (like g_shell_parse_argv).
bool SplitArgs(const std::string& line, std::vector<std::string>& out, std::string* error) {
    out.clear();
    std::string cur;
    bool inWord = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '\'') {
            inWord = true;
            const size_t end = line.find('\'', i + 1);
            if (end == std::string::npos) {
                if (error) *error = "'";
                return false;
            }
            cur.append(line, i + 1, end - i - 1);
            i = end;
        } else if (c == '"') {
            inWord = true;
            for (++i; i < line.size() && line[i] != '"'; ++i) {
                if (line[i] == '\\' && i + 1 < line.size() && std::strchr("\"\\$`", line[i + 1])) ++i;
                cur.push_back(line[i]);
            }
            if (i >= line.size()) {
                if (error) *error = "\"";
                return false;
            }
        } else if (c == '\\') {
            inWord = true;
            if (i + 1 < line.size()) cur.push_back(line[++i]);
        } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (inWord) {
                out.push_back(cur);
                cur.clear();
                inWord = false;
            }
        } else {
            inWord = true;
            cur.push_back(c);
        }
    }
    if (inWord) out.push_back(cur);
    return true;
}

std::string ShellQuote(const std::string& s) {  // display only
    if (!s.empty() && s.find_first_of(" \t\n'\"\\$`!*?[]{}()<>|&;#~") == std::string::npos) return s;
    std::string out = "'";
    for (const char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
}

std::string BaseName(const std::string& p) {
    const size_t slash = p.rfind('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

std::string Extension(const std::string& p) {
    const std::string name = BaseName(p);
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0) return "";
    std::string ext = name.substr(dot);
    for (auto& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return ext;
}

bool IsRegularFile(const std::string& path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool IsExecutableFile(const std::string& path) { return IsRegularFile(path) && access(path.c_str(), X_OK) == 0; }

std::string FindInPath(const std::string& name) {
    if (name.empty()) return "";
    if (name.find('/') != std::string::npos) return IsExecutableFile(name) ? name : "";
    const char* path = std::getenv("PATH");
    std::string dirs = path && *path ? path : "/usr/local/bin:/usr/bin:/bin";
    size_t start = 0;
    while (start <= dirs.size()) {
        size_t end = dirs.find(':', start);
        if (end == std::string::npos) end = dirs.size();
        const std::string dir = dirs.substr(start, end - start);
        const std::string candidate = (dir.empty() ? "." : dir) + "/" + name;
        if (IsExecutableFile(candidate)) return candidate;
        start = end + 1;
    }
    return "";
}

// "#!/usr/bin/env python3" -> {"/usr/bin/env", "python3"}; empty when there is no shebang.
std::vector<std::string> Shebang(const std::string& path) {
    std::vector<std::string> out;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    char line[256] = {};
    const bool ok = std::fgets(line, sizeof line, f) != nullptr;
    std::fclose(f);
    if (!ok || line[0] != '#' || line[1] != '!') return out;
    std::string s(line + 2);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = s.find_first_not_of(" \t");
    if (i == std::string::npos) return out;
    const size_t end = s.find_first_of(" \t", i);
    out.push_back(s.substr(i, end == std::string::npos ? std::string::npos : end - i));
    if (end != std::string::npos) {
        const size_t argStart = s.find_first_not_of(" \t", end);
        if (argStart != std::string::npos) out.push_back(s.substr(argStart));  // Linux: one optional argument
    }
    return out;
}

// How a script inside the plugin folder is started.
std::vector<std::string> RunnerFor(const std::string& local) {
    const std::string ext = Extension(local);
    if (ext == ".py" || ext == ".pyw") return {"python3", local};
    if (ext == ".js" || ext == ".mjs" || ext == ".cjs") return {"node", local};
    if (ext == ".sh" || ext == ".bash") return {"bash", local};
    if (ext == ".ps1") return {"pwsh", "-NoProfile", "-NonInteractive", "-File", local};
    if (ext == ".rb") return {"ruby", local};
    if (ext == ".pl") return {"perl", local};
    if (ext == ".php") return {"php", local};
    if (IsExecutableFile(local)) return {local};
    std::vector<std::string> bang = Shebang(local);
    if (!bang.empty()) {
        bang.push_back(local);
        return bang;
    }
    return {local};  // fails with "permission denied": the user needs chmod +x
}

struct Resolved {
    std::vector<std::string> argv;  // argv[0] resolved to a path when found
    std::string error;
};

Resolved Resolve(const std::wstring& run, const std::wstring& folder) {
    Resolved r;
    std::vector<std::string> words;
    std::string unbalanced;
    if (!SplitArgs(U8(run), words, &unbalanced)) {
        r.error = "run 裡的引號 " + unbalanced + " 沒有成對";
        return r;
    }
    if (words.empty()) {
        r.error = "run 是空的";
        return r;
    }
    const std::string first = words[0];
    std::string local;
    if (first[0] == '/') {
        if (IsRegularFile(first)) local = first;
    } else if (first == "~" || first.rfind("~/", 0) == 0) {
        const std::string expanded = HomeDirectory() + first.substr(1);
        if (IsRegularFile(expanded)) local = expanded;
    } else if (!folder.empty()) {
        std::string relative = first.rfind("./", 0) == 0 ? first.substr(2) : first;
        const std::string candidate = U8(folder) + "/" + relative;
        if (IsRegularFile(candidate)) local = candidate;
    }
    if (!local.empty()) {
        r.argv = RunnerFor(local);
        r.argv.insert(r.argv.end(), words.begin() + 1, words.end());
    } else {
        r.argv = words;
    }
    const std::string program = FindInPath(r.argv[0]);
    if (program.empty()) {
        if (r.argv[0].find('/') != std::string::npos && IsRegularFile(r.argv[0]))
            r.error = "沒有執行權限：" + r.argv[0] + "（chmod +x，或在 run 前面寫上直譯器）";
        else
            r.error = "找不到程式「" + r.argv[0] + "」（是否已安裝並在 PATH 中？）";
    } else {
        r.argv[0] = program;
    }
    return r;
}

// "{query}" becomes exactly one argument (no shell is involved), except inside arguments that
// are program code: "bash -c CODE", "python3 -c CODE", "node -e CODE", "perl -le CODE",
// awk / sed programs... There a quote or $(...) in the typed text would run as code, so the
// query is left out of that argument (the program can still read it from stdin or BEAM_QUERY).
// Shells and interpreters are found anywhere in the line ("timeout 5 bash -c ..."), and data
// arguments after the code stay usable ("bash -c 'grep -- \"$1\" file' _ {query}").
std::vector<bool> CodeArguments(const std::vector<std::string>& argv) {
    enum class Kind { None, Shell, Python, Node, Perl, Ruby, Php, Lua, Pwsh, OneFlag, Awk, Sed };
    auto kindOf = [](const std::string& path) {
        static const std::set<std::string> shells{"sh", "bash", "dash", "zsh", "ksh", "mksh", "fish",
                                                  "csh", "tcsh", "ash", "yash", "busybox"};
        std::string exe = BaseName(path);
        while (!exe.empty() && (std::isdigit(static_cast<unsigned char>(exe.back())) || exe.back() == '.' || exe.back() == '-'))
            exe.pop_back();  // python3.12 -> python
        if (shells.count(exe)) return Kind::Shell;
        if (exe == "python" || exe == "pypy") return Kind::Python;
        if (exe == "node" || exe == "nodejs" || exe == "bun" || exe == "deno") return Kind::Node;
        if (exe == "perl") return Kind::Perl;
        if (exe == "ruby") return Kind::Ruby;
        if (exe == "php") return Kind::Php;
        if (exe == "lua" || exe == "luajit") return Kind::Lua;
        if (exe == "pwsh" || exe == "powershell") return Kind::Pwsh;
        if (exe == "osascript" || exe == "Rscript" || exe == "R" || exe == "julia") return Kind::OneFlag;  // -e CODE
        if (exe == "awk" || exe == "gawk" || exe == "mawk" || exe == "nawk") return Kind::Awk;
        if (exe == "sed" || exe == "gsed") return Kind::Sed;
        return Kind::None;
    };
    std::vector<bool> code(argv.size(), false);
    Kind kind = Kind::None;
    enum class Next { Nothing, Code, Skip, ModuleThenData } next = Next::Nothing;
    bool shellCode = false;     // shell: -c seen, the first non-option argument is the code
    bool programGiven = false;  // awk / sed: program already given with -e / -f
    for (size_t i = 0; i < argv.size(); ++i) {
        const std::string& a = argv[i];
        if (next == Next::Code) {
            code[i] = true;
            next = Next::Nothing;
            programGiven = true;
            if (kind != Kind::Perl && kind != Kind::Node && kind != Kind::Sed && kind != Kind::Awk) kind = Kind::None;
            continue;
        }
        if (next == Next::Skip) {
            next = Next::Nothing;
            continue;
        }
        if (next == Next::ModuleThenData) {
            next = Next::Nothing;
            kind = Kind::None;
            continue;
        }
        if (const Kind k = kindOf(a); k != Kind::None) {  // also behind wrappers: "timeout 5 bash -c ..."
            kind = k;
            shellCode = false;
            programGiven = false;
            continue;
        }
        if (kind == Kind::None) continue;
        const bool option = a.size() > 1 && (a[0] == '-' || (kind == Kind::Shell && a[0] == '+'));
        if (a == "--") {
            if (kind == Kind::Shell && shellCode) next = Next::Code;
            else if (kind != Kind::Awk && kind != Kind::Sed) kind = Kind::None;
            continue;
        }
        if (!option) {
            if (kind == Kind::Shell) {
                if (shellCode) code[i] = true;
                kind = Kind::None;  // the code, or a script file: the rest are its arguments
            } else if ((kind == Kind::Awk || kind == Kind::Sed) && !programGiven) {
                code[i] = true;
                programGiven = true;
            } else if (kind != Kind::Awk && kind != Kind::Sed) {
                kind = Kind::None;  // a script file
            }
            continue;
        }
        const std::string flags = a.substr(1);
        switch (kind) {
            case Kind::Shell:
                if (a == "-o" || a == "+o" || a == "-O" || a == "+O") next = Next::Skip;
                else if (a[0] == '-' && a[1] != '-' && flags.find('c') != std::string::npos) shellCode = true;
                break;
            case Kind::Python:
                for (size_t j = 0; j < flags.size(); ++j) {
                    const char f = flags[j];
                    if (f == 'c' || f == 'm' || f == 'W' || f == 'X') {
                        const bool attached = j + 1 < flags.size();
                        if (f == 'c') {
                            if (attached) code[i] = true;
                            else next = Next::Code;
                        } else if (f == 'm') {
                            if (attached) kind = Kind::None;
                            else next = Next::ModuleThenData;
                        } else if (!attached) {
                            next = Next::Skip;
                        }
                        break;
                    }
                }
                break;
            case Kind::Node:
                if (a == "-e" || a == "-p" || a == "-pe" || a == "-ep" || a == "--eval" || a == "--print") next = Next::Code;
                else if (a.rfind("--eval=", 0) == 0 || a.rfind("--print=", 0) == 0) code[i] = true;
                else if (a == "-r" || a == "--require") next = Next::Skip;
                break;
            case Kind::Perl:
            case Kind::Ruby:
            case Kind::Lua: {
                const char* letters = kind == Kind::Perl ? "eE" : "e";
                for (size_t j = 0; j < flags.size(); ++j) {
                    if (std::strchr(letters, flags[j])) {
                        if (j + 1 < flags.size()) code[i] = true;
                        else next = Next::Code;
                        break;
                    }
                }
                break;
            }
            case Kind::Php:
                if (a == "-r" || a == "-B" || a == "-R" || a == "-E") next = Next::Code;
                else if (a.size() > 2 && (a[1] == 'r' || a[1] == 'B' || a[1] == 'R' || a[1] == 'E')) code[i] = true;
                else if (a == "-f" || a == "-c" || a == "-d" || a == "-z") next = Next::Skip;
                break;
            case Kind::Pwsh: {
                std::string lower = a;
                for (auto& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (lower == "-c" || lower.rfind("-com", 0) == 0 || lower == "-e" || lower.rfind("-ec", 0) == 0 ||
                    lower.rfind("-enc", 0) == 0)
                    next = Next::Code;
                else if (lower == "-f" || lower.rfind("-fi", 0) == 0) kind = Kind::None;  // -File script args...
                break;
            }
            case Kind::OneFlag:
                if (a == "-e" || a == "-E") next = Next::Code;
                break;
            case Kind::Awk:
                if (a == "-f" || a == "--file") {
                    programGiven = true;
                    next = Next::Skip;
                } else if (a == "-e" || a == "--source") {
                    next = Next::Code;
                } else if (a.rfind("--source=", 0) == 0) {
                    code[i] = true;
                    programGiven = true;
                } else if (a == "-v" || a == "-F" || a == "--assign" || a == "--field-separator") {
                    next = Next::Skip;
                }
                break;
            case Kind::Sed:
                if (a == "-e" || a == "--expression") {
                    next = Next::Code;
                } else if (a.rfind("--expression=", 0) == 0 || (a.size() > 2 && a[1] == 'e')) {
                    code[i] = true;
                    programGiven = true;
                } else if (a == "-f" || a == "--file") {
                    programGiven = true;
                    next = Next::Skip;
                } else if (a == "-l" || a == "--line-length") {
                    next = Next::Skip;
                }
                break;
            default:
                break;
        }
    }
    return code;
}

std::string ReplaceQuery(std::string arg, const std::string& query) {
    for (size_t at = 0;;) {
        size_t found = std::string::npos;
        for (size_t i = at; i + 7 <= arg.size(); ++i) {
            if (arg[i] == '{' && EqualsIgnoreCase(Utf8ToWide(arg.substr(i, 7)), L"{query}")) {
                found = i;
                break;
            }
        }
        if (found == std::string::npos) return arg;
        arg.replace(found, 7, query);
        at = found + query.size();
    }
}

std::wstring Display(const std::vector<std::string>& argv) {
    std::string line;
    for (const auto& a : argv) line += (line.empty() ? "" : " ") + ShellQuote(a);
    return Utf8ToWide(line);
}

std::vector<char*> CArray(std::vector<std::string>& strings) {
    std::vector<char*> out;
    out.reserve(strings.size() + 1);
    for (auto& s : strings) out.push_back(s.data());
    out.push_back(nullptr);
    return out;
}

std::vector<std::string> Environment(const std::vector<std::pair<std::wstring, std::wstring>>& extra) {
    std::vector<std::string> overrides;
    for (const auto& [name, value] : extra) overrides.push_back(U8(name) + "=" + U8(value));
    std::vector<std::string> env;
    for (char** e = environ; e && *e; ++e) {
        const std::string entry = *e;
        const std::string name = entry.substr(0, entry.find('='));
        bool replaced = false;
        for (const auto& o : overrides) replaced = replaced || o.compare(0, name.size() + 1, name + "=") == 0;
        if (!replaced) env.push_back(entry);
    }
    env.insert(env.end(), overrides.begin(), overrides.end());
    return env;
}

void SetNonBlocking(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }

void CloseFd(int& fd) {
    if (fd >= 0) close(fd);
    fd = -1;
}

int PidFd(pid_t pid) {
#ifdef SYS_pidfd_open
    return static_cast<int>(syscall(SYS_pidfd_open, pid, 0));
#else
    (void)pid;
    return -1;
#endif
}

std::string ErrorText(int error) {
    char buf[256];
    return std::string(strerror_r(error, buf, sizeof buf));
}

}  // namespace

void PreparePluginLaunch(const PluginCommand& cmd, const std::wstring& query, PluginLaunch& launch) {
    Resolved r = Resolve(cmd.spec.run, cmd.folder);
    if (!r.error.empty()) {
        launch.prepareError = Utf8ToWide(r.error);
        launch.display = cmd.spec.run;
        return;
    }
    const std::vector<bool> code = CodeArguments(r.argv);
    const std::string q = U8(query);
    for (size_t i = 0; i < r.argv.size(); ++i)
        launch.argv.push_back(Utf8ToWide(ReplaceQuery(r.argv[i], code[i] ? std::string() : q)));
    std::vector<std::string> shown;
    for (const auto& a : launch.argv) shown.push_back(U8(a));
    launch.display = Display(shown);
}

void ExecutePluginLaunch(const PluginLaunch& launch, PluginRunControl& control, PluginRunRecord& rec) {
    IgnoreSigpipeOnce();
    const uint64_t start = NowMs();
    {
        std::lock_guard lock(control.mutex);
        if (control.cancelled) {  // superseded before it even started
            rec.cancelled = true;
            return;
        }
    }
    if (launch.argv.empty()) {
        rec.launchError = L"沒有可以執行的指令";
        return;
    }
    std::vector<std::string> argv;
    for (const auto& a : launch.argv) argv.push_back(U8(a));
    std::vector<std::string> env = Environment(launch.environment);
    std::vector<char*> argvp = CArray(argv);
    std::vector<char*> envp = CArray(env);

    int in[2] = {-1, -1}, out[2] = {-1, -1}, err[2] = {-1, -1};
    if (pipe2(in, O_CLOEXEC) != 0 || pipe2(out, O_CLOEXEC) != 0 || pipe2(err, O_CLOEXEC) != 0) {
        rec.launchError = L"pipe: " + Utf8ToWide(ErrorText(errno));
        for (int* fd : {&in[0], &in[1], &out[0], &out[1], &err[0], &err[1]}) CloseFd(*fd);
        return;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in[0], 0);
    posix_spawn_file_actions_adddup2(&actions, out[1], 1);
    posix_spawn_file_actions_adddup2(&actions, err[1], 2);
    const std::string dir = U8(launch.directory);
    if (!dir.empty()) posix_spawn_file_actions_addchdir_np(&actions, dir.c_str());
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    posix_spawnattr_setpgroup(&attr, 0);  // own process group: the whole tree can be killed at once
    sigset_t none, defaults;
    sigemptyset(&none);
    posix_spawnattr_setsigmask(&attr, &none);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    sigaddset(&defaults, SIGINT);
    sigaddset(&defaults, SIGTERM);
    sigaddset(&defaults, SIGCHLD);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    pid_t pid = -1;
    const int spawnError = posix_spawn(&pid, argv[0].c_str(), &actions, &attr, argvp.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    CloseFd(in[0]);
    CloseFd(out[1]);
    CloseFd(err[1]);
    if (spawnError != 0) {
        rec.launchError = Utf8ToWide(ErrorText(spawnError));
        if (spawnError == ENOENT) rec.launchError += L"（找不到程式或工作目錄）";
        if (spawnError == EACCES) rec.launchError += L"（沒有執行權限：chmod +x？）";
        if (spawnError == E2BIG) rec.launchError += L"（{query} 太長，無法當作參數；請改讀 stdin 的 query）";
        CloseFd(in[1]);
        CloseFd(out[0]);
        CloseFd(err[0]);
        rec.durationMs = NowMs() - start;
        return;
    }
    {
        std::lock_guard lock(control.mutex);
        control.kill = [pid] { kill(-pid, SIGKILL); };
        if (control.cancelled) control.kill();  // superseded while starting
    }

    SetNonBlocking(in[1]);
    SetNonBlocking(out[0]);
    SetNonBlocking(err[0]);
    int pidfd = PidFd(pid);
    const std::string& request = launch.request;
    size_t written = 0;
    if (request.empty()) CloseFd(in[1]);
    const uint64_t deadline = start + launch.timeoutMs;
    uint64_t drainUntil = 0;
    bool exited = false;
    int status = 0;
    char buf[16384];

    auto reap = [&](bool block) {
        if (exited) return;
        if (waitpid(pid, &status, block ? 0 : WNOHANG) == pid) {
            exited = true;
            // Grandchildren may still hold the pipes: list/action runs take them down with the group;
            // command runs may legitimately leave programs running, so just stop reading soon.
            if (launch.killTree) kill(-pid, SIGKILL);
            drainUntil = NowMs() + 1500;
        }
    };
    auto readInto = [&](int& fd, std::string& sink, size_t limit) {
        for (;;) {
            const ssize_t n = read(fd, buf, sizeof buf);
            if (n > 0) {
                if (sink.size() < limit) sink.append(buf, std::min<size_t>(static_cast<size_t>(n), limit - sink.size()));
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && errno == EAGAIN) return;
            CloseFd(fd);  // EOF or error
            return;
        }
    };

    for (;;) {
        reap(false);
        const uint64_t now = NowMs();
        if (exited && out[0] < 0 && err[0] < 0) break;
        if (exited && now >= drainUntil) break;
        if (!exited && now >= deadline) {
            rec.timedOut = true;
            kill(-pid, SIGKILL);
            reap(true);
            drainUntil = NowMs() + 300;
            continue;
        }
        pollfd fds[4];
        int count = 0;
        int outIndex = -1, errIndex = -1, inIndex = -1;
        if (out[0] >= 0) {
            outIndex = count;
            fds[count++] = {out[0], POLLIN, 0};
        }
        if (err[0] >= 0) {
            errIndex = count;
            fds[count++] = {err[0], POLLIN, 0};
        }
        if (in[1] >= 0) {
            inIndex = count;
            fds[count++] = {in[1], POLLOUT, 0};
        }
        if (!exited && pidfd >= 0) fds[count++] = {pidfd, POLLIN, 0};
        const uint64_t until = exited ? drainUntil : deadline;
        int timeout = static_cast<int>(std::min<uint64_t>(until > now ? until - now : 0, 1000));
        if (!exited && pidfd < 0) timeout = std::min(timeout, 20);  // no pidfd: poll for the exit
        const int ready = poll(fds, static_cast<nfds_t>(count), timeout);
        if (ready < 0 && errno != EINTR) break;
        if (ready <= 0) continue;
        if (outIndex >= 0 && fds[outIndex].revents) readInto(out[0], rec.stdoutText, kMaxStdout);
        if (errIndex >= 0 && fds[errIndex].revents) readInto(err[0], rec.stderrText, kMaxStderr);
        if (inIndex >= 0 && fds[inIndex].revents) {
            if (fds[inIndex].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                CloseFd(in[1]);  // the plugin closed stdin / exited without reading it
            } else {
                const ssize_t n = write(in[1], request.data() + written, std::min<size_t>(request.size() - written, 65536));
                if (n > 0) {
                    written += static_cast<size_t>(n);
                    if (written >= request.size()) CloseFd(in[1]);  // EOF: the request is complete
                } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
                    CloseFd(in[1]);
                }
            }
        }
    }
    if (!exited) {
        kill(-pid, SIGKILL);
        reap(true);
    }
    {
        std::lock_guard lock(control.mutex);
        control.kill = nullptr;
        rec.cancelled = control.cancelled;
    }
    CloseFd(in[1]);
    if (!launch.killTree && (out[0] >= 0 || err[0] >= 0)) {
        // A program started by a command plugin inherited stdout/stderr. Closing our ends would
        // kill it with SIGPIPE on its next write, so keep draining them until it closes them.
        std::thread([o = out[0], e = err[0]]() mutable {
            char sink[4096];
            while (o >= 0 || e >= 0) {
                pollfd fds[2];
                int n = 0;
                if (o >= 0) fds[n++] = {o, POLLIN, 0};
                if (e >= 0) fds[n++] = {e, POLLIN, 0};
                if (poll(fds, static_cast<nfds_t>(n), -1) < 0 && errno != EINTR) break;
                for (int i = 0; i < n; ++i) {
                    if (!fds[i].revents) continue;
                    const ssize_t got = read(fds[i].fd, sink, sizeof sink);
                    if (got == 0 || (got < 0 && errno != EAGAIN && errno != EINTR)) {
                        close(fds[i].fd);
                        (fds[i].fd == o ? o : e) = -1;
                    }
                }
            }
            if (o >= 0) close(o);
            if (e >= 0) close(e);
        }).detach();
        out[0] = err[0] = -1;
    }
    CloseFd(out[0]);
    CloseFd(err[0]);
    CloseFd(pidfd);
    if (WIFEXITED(status)) rec.exitCode = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) rec.exitCode = 128 + WTERMSIG(status);
    rec.durationMs = NowMs() - start;
}

std::vector<std::string> TerminalArgv(const std::vector<std::string>& command, const std::string& directory) {
    struct Terminal {
        const char* exe;
        std::vector<std::string> base;
        const char* dirFlag;  // "--x=" form, or nullptr
        const char* dirOption;  // separate-argument form, or nullptr
        const char* execFlag;
    };
    static const Terminal kTerminals[] = {
        {"xdg-terminal-exec", {}, "--dir=", nullptr, "--"},  // freedesktop default terminal (Ubuntu 25.10+)
        {"ptyxis", {"--new-window"}, "--working-directory=", nullptr, "--"},
        {"gnome-terminal", {}, "--working-directory=", nullptr, "--"},
        {"kgx", {}, "--working-directory=", nullptr, "--"},
        {"konsole", {}, nullptr, "--workdir", "-e"},
        {"xfce4-terminal", {}, "--working-directory=", nullptr, "-x"},
        {"x-terminal-emulator", {}, nullptr, nullptr, "-e"},
        {"xterm", {}, nullptr, nullptr, "-e"},
    };
    for (const auto& t : kTerminals) {
        const std::string path = FindInPath(t.exe);
        if (path.empty()) continue;
        std::vector<std::string> argv{path};
        argv.insert(argv.end(), t.base.begin(), t.base.end());
        if (!directory.empty() && t.dirFlag) argv.push_back(t.dirFlag + directory);
        if (!directory.empty() && t.dirOption) {
            argv.push_back(t.dirOption);
            argv.push_back(directory);
        }
        if (!command.empty()) {
            argv.push_back(t.execFlag);
            argv.insert(argv.end(), command.begin(), command.end());
        }
        return argv;
    }
    return {};
}

bool ResolvePluginArgv(const std::wstring& run, const std::wstring& folder, std::vector<std::string>* argv,
                       std::wstring* error) {
    Resolved r = Resolve(run, folder);
    if (!r.error.empty()) {
        if (error) *error = Utf8ToWide(r.error);
        return false;
    }
    if (argv) *argv = std::move(r.argv);
    return true;
}

std::wstring ResolveCommandLine(const std::wstring& run, const std::wstring& folder) {
    const Resolved r = Resolve(run, folder);
    if (!r.error.empty()) return run + L"  （" + Utf8ToWide(r.error) + L"）";
    return Display(r.argv);
}

bool SpawnDetached(const std::wstring& commandLine, const std::wstring& directory, bool visibleConsole,
                   std::wstring* error) {
    IgnoreSigpipeOnce();
    std::vector<std::string> argv;
    if (visibleConsole) {
        // Like cmd /k: the terminal stays open with a shell after the command finishes.
        argv = TerminalArgv({"bash", "-c", U8(commandLine) + "\nexec \"${SHELL:-bash}\""}, U8(directory));
        if (argv.empty()) {
            if (error) *error = L"找不到終端機程式（xdg-terminal-exec、ptyxis、gnome-terminal…）";
            return false;
        }
    } else {
        const Resolved r = Resolve(commandLine, directory);
        if (!r.error.empty()) {
            if (error) *error = Utf8ToWide(r.error);
            return false;
        }
        argv = r.argv;
    }
    std::vector<char*> argvp = CArray(argv);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    const std::string dir = U8(directory);
    struct stat st {};
    if (!dir.empty() && stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
        posix_spawn_file_actions_addchdir_np(&actions, dir.c_str());
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    sigset_t none, defaults;
    sigemptyset(&none);
    posix_spawnattr_setsigmask(&attr, &none);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    sigaddset(&defaults, SIGCHLD);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    pid_t pid = -1;
    const int rc = posix_spawn(&pid, argv[0].c_str(), &actions, &attr, argvp.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) {
        if (error) *error = Utf8ToWide(ErrorText(rc));
        return false;
    }
    std::thread([pid] { waitpid(pid, nullptr, 0); }).detach();  // no zombies
    return true;
}

const char* PluginDocumentation() { return reinterpret_cast<const char*>(kBeamPluginsDoc); }

std::wstring PlatformConfigDirectory() {
    std::string base;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg == '/') base = xdg;
    else base = HomeDirectory() + "/.config";
    const std::string dir = base + "/beam";
    mkdir(base.c_str(), 0700);
    mkdir(dir.c_str(), 0700);
    return Utf8ToWide(dir);
}

}  // namespace beam
