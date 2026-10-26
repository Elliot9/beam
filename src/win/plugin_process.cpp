// Windows side of the plugin runner: CreateProcess with pipes and a job object.
#include "win/common.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>

#include "core/text.hpp"
#include "plugin/plugin_process.hpp"
#include "win/log.hpp"
#include "win/shell.hpp"

namespace beam {
namespace {

constexpr size_t kMaxStdout = 8u << 20;
constexpr size_t kMaxStderr = 1u << 20;

std::wstring SearchPathFor(const std::wstring& name, const wchar_t* ext) {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = SearchPathW(nullptr, name.c_str(), ext, ARRAYSIZE(buf), buf, nullptr);
    return (n > 0 && n < ARRAYSIZE(buf)) ? std::wstring(buf, n) : std::wstring();
}

std::wstring PythonLauncher() {
    static const std::wstring launcher = [] {
        if (!SearchPathFor(L"py", L".exe").empty()) return std::wstring(L"py");
        return std::wstring(L"python");
    }();
    return launcher;
}

bool IsAbsolutePath(const std::wstring& p) {
    return p.size() > 2 && ((p[1] == L':' && (p[2] == L'\\' || p[2] == L'/')) || (p[0] == L'\\' && p[1] == L'\\'));
}

std::string AnsiToUtf8(const std::string& text) {
    if (text.empty()) return text;
    const int n = MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (n <= 0) return text;
    std::wstring wide(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), wide.data(), n);
    return WideToUtf8(wide);
}

std::wstring EnvName(const std::wstring& entry) {
    const size_t eq = entry.find(L'=', 1);  // entries like "=C:=C:\x" start with '='
    return eq == std::wstring::npos ? entry : entry.substr(0, eq);
}

std::wstring BuildEnvironment(const std::vector<std::pair<std::wstring, std::wstring>>& extra) {
    std::vector<std::wstring> vars;
    if (wchar_t* env = GetEnvironmentStringsW()) {
        for (const wchar_t* p = env; *p; p += wcslen(p) + 1) vars.emplace_back(p);
        FreeEnvironmentStringsW(env);
    }
    for (const auto& [name, value] : extra) {
        vars.erase(std::remove_if(vars.begin(), vars.end(),
                                  [&](const std::wstring& v) { return EqualsIgnoreCase(EnvName(v), name); }),
                   vars.end());
        vars.push_back(name + L"=" + value);
    }
    std::wstring block;
    for (const auto& v : vars) {
        block += v;
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

// Pipe I/O runs on its own threads (the request is written on one too, so a plugin that never
// reads stdin cannot block Beam past the timeout). Each job owns its pipe handle and buffer:
// a thread still blocked when Execute gives up - a grandchild kept the pipe open - can be
// abandoned safely and cleans up after itself once the pipe finally breaks.
struct PipeJob {
    HANDLE pipe = nullptr;
    std::mutex mutex;
    std::string data;  // reader: collected output; writer: the request
    size_t limit = 0;
    std::atomic<bool> stop{false};
    ~PipeJob() {
        if (pipe) CloseHandle(pipe);
    }
};

std::shared_ptr<PipeJob> TakeJob(LPVOID param) {
    auto* holder = static_cast<std::shared_ptr<PipeJob>*>(param);
    std::shared_ptr<PipeJob> job = std::move(*holder);
    delete holder;
    return job;
}

DWORD WINAPI ReaderProc(LPVOID param) {
    const std::shared_ptr<PipeJob> job = TakeJob(param);
    char buf[16384];
    DWORD n = 0;
    while (!job->stop && ReadFile(job->pipe, buf, sizeof buf, &n, nullptr) && n > 0) {
        std::lock_guard lock(job->mutex);
        if (job->data.size() < job->limit) job->data.append(buf, std::min<size_t>(n, job->limit - job->data.size()));
    }
    return 0;
}

DWORD WINAPI WriterProc(LPVOID param) {
    const std::shared_ptr<PipeJob> job = TakeJob(param);
    const char* p = job->data.data();
    size_t left = job->data.size();
    while (left > 0 && !job->stop) {
        DWORD n = 0;
        if (!WriteFile(job->pipe, p, static_cast<DWORD>(std::min<size_t>(left, 1u << 16)), &n, nullptr) || n == 0)
            break;
        p += n;
        left -= n;
    }
    CloseHandle(job->pipe);  // EOF: the request is complete
    job->pipe = nullptr;
    return 0;
}

HANDLE StartPipeThread(LPTHREAD_START_ROUTINE proc, const std::shared_ptr<PipeJob>& job) {
    auto* holder = new std::shared_ptr<PipeJob>(job);
    HANDLE thread = CreateThread(nullptr, 0, proc, holder, 0, nullptr);
    if (!thread) delete holder;
    return thread;
}

// Waits for a pipe thread; one stuck in ReadFile/WriteFile is cancelled, and left behind if even
// that does not help (CancelSynchronousIo can miss a thread that is between two calls).
void FinishPipeThread(HANDLE thread, PipeJob& job, DWORD waitMs) {
    if (!thread) return;
    if (WaitForSingleObject(thread, waitMs) == WAIT_TIMEOUT) {
        job.stop = true;
        for (int i = 0; i < 4 && WaitForSingleObject(thread, 250) == WAIT_TIMEOUT; ++i) CancelSynchronousIo(thread);
    }
    CloseHandle(thread);
}

std::string Collected(PipeJob& job) {
    std::lock_guard lock(job.mutex);
    return job.data;
}

// "{query}" becomes one argument quoted for CommandLineToArgvW. That is not safe where the whole
// line is parsed again as code: cmd.exe (& | > ^ %) and PowerShell without -File (-Command is
// its default). There the query is left out; such plugins read stdin or %BEAM_QUERY%.
bool QueryUnsafeIn(const std::wstring& commandLine) {
    const auto [first, rest] = SplitCommandLine(commandLine);
    std::wstring exe = Fold(FileNameOf(first));
    if (EndsWithIgnoreCase(exe, L".exe")) exe.resize(exe.size() - 4);
    if (exe == L"cmd" || StartsWithIgnoreCase(first, L"%comspec%")) return true;
    if (exe == L"powershell" || exe == L"pwsh") {
        for (const auto& token : SplitWhitespace(rest)) {
            const std::wstring t = Fold(token);
            if (t == L"-file" || t == L"-f" || t == L"/file") return false;
            const bool command = t == L"-c" || t == L"/c" || t.rfind(L"-com", 0) == 0 || t.rfind(L"/com", 0) == 0;
            const bool encoded = t == L"-e" || t == L"-ec" || t.rfind(L"-enc", 0) == 0;  // not -ExecutionPolicy / -ep
            if (command || encoded) return true;
        }
        return true;  // no -File: Windows PowerShell runs the arguments as a command
    }
    return false;
}

// cmd /s strips the outer quotes; the script keeps its own, so "&" or "(" in a folder name stay literal.
std::wstring CmdInvocation(const std::wstring& script, const std::wstring& rest) {
    return L"cmd.exe /d /s /c \"\"" + script + L"\"" + (rest.empty() ? std::wstring() : L" " + rest) + L"\"";
}

}  // namespace

void PreparePluginLaunch(const PluginCommand& cmd, const std::wstring& query, PluginLaunch& launch) {
    const std::wstring line = ResolveCommandLine(cmd.spec.run, cmd.folder);
    // {query} is substituted as a quoted argument, except where the line is parsed as code
    // (cmd.exe, PowerShell -Command); those plugins read stdin or %BEAM_QUERY% instead.
    launch.commandLine = ExpandRunTemplate(line, QueryUnsafeIn(line) ? std::wstring_view() : std::wstring_view(query));
    launch.display = launch.commandLine;
}

void ExecutePluginLaunch(const PluginLaunch& launch, PluginRunControl& control, PluginRunRecord& rec) {
    const ULONGLONG start = GetTickCount64();
    {
        std::lock_guard lock(control.mutex);
        if (control.cancelled) {  // superseded before it even started
            rec.cancelled = true;
            return;
        }
    }
    HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr, errR = nullptr, errW = nullptr;
    auto closeAll = [&] {
        for (HANDLE* h : {&inR, &inW, &outR, &outW, &errR, &errW}) {
            if (*h) {
                CloseHandle(*h);
                *h = nullptr;
            }
        }
    };
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    if (!CreatePipe(&inR, &inW, &sa, 1 << 16) || !CreatePipe(&outR, &outW, &sa, 1 << 16) ||
        !CreatePipe(&errR, &errW, &sa, 1 << 16)) {
        rec.launchError = L"CreatePipe: " + log::ErrorText(GetLastError());
        closeAll();
        return;
    }
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);

    // Only these three handles are inherited, so concurrent runs never keep each other's pipes open.
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<unsigned char> attrBuffer(attrSize);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuffer.data());
    HANDLE inherit[3] = {inR, outW, errW};
    const bool attrsOk = attrSize > 0 && InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) &&
                         UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                                   sizeof(inherit), nullptr, nullptr);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.StartupInfo.wShowWindow = SW_HIDE;
    si.StartupInfo.hStdInput = inR;
    si.StartupInfo.hStdOutput = outW;
    si.StartupInfo.hStdError = errW;
    si.lpAttributeList = attrsOk ? attrs : nullptr;

    std::wstring cmd = launch.commandLine;
    std::wstring env = BuildEnvironment(launch.environment);
    const std::wstring& directory = launch.directory;
    PROCESS_INFORMATION pi{};
    const DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT |
                        (attrsOk ? EXTENDED_STARTUPINFO_PRESENT : 0);
    const BOOL created = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, flags, env.data(),
                                        directory.empty() ? nullptr : directory.c_str(), &si.StartupInfo, &pi);
    const DWORD createError = GetLastError();
    if (attrsOk) DeleteProcThreadAttributeList(attrs);
    CloseHandle(inR);
    inR = nullptr;
    CloseHandle(outW);
    outW = nullptr;
    CloseHandle(errW);
    errW = nullptr;
    if (!created) {
        rec.launchError = log::ErrorText(createError);
        if (createError == ERROR_FILE_NOT_FOUND || createError == ERROR_PATH_NOT_FOUND)
            rec.launchError += L"（找不到程式：是否已安裝並在 PATH 中？）";
        closeAll();
        rec.durationMs = GetTickCount64() - start;
        return;
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        if (launch.killTree) info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
        if (!AssignProcessToJobObject(job, pi.hProcess)) {
            CloseHandle(job);
            job = nullptr;
        }
    }
    {
        std::lock_guard lock(control.mutex);
        const HANDLE process = pi.hProcess;
        control.kill = [job, process] {
            if (job) TerminateJobObject(job, 1);
            else TerminateProcess(process, 1);
        };
        if (control.cancelled) control.kill();  // superseded while starting
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    auto out = std::make_shared<PipeJob>();
    out->pipe = std::exchange(outR, nullptr);
    out->limit = kMaxStdout;
    auto err = std::make_shared<PipeJob>();
    err->pipe = std::exchange(errR, nullptr);
    err->limit = kMaxStderr;
    auto in = std::make_shared<PipeJob>();
    in->pipe = std::exchange(inW, nullptr);
    in->data = launch.request;
    const HANDLE outThread = StartPipeThread(ReaderProc, out);
    const HANDLE errThread = StartPipeThread(ReaderProc, err);
    const HANDLE inThread = StartPipeThread(WriterProc, in);  // if it fails, ~PipeJob closes stdin (empty request)

    if (WaitForSingleObject(pi.hProcess, launch.timeoutMs) == WAIT_TIMEOUT) {
        rec.timedOut = true;
        if (job) TerminateJobObject(job, 1);
        else TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
    }
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    rec.exitCode = static_cast<long>(exitCode);
    // Grandchildren may still hold the pipes: list/action runs take them down with the job;
    // command runs may legitimately leave programs running, so just stop reading.
    if (launch.killTree && job) TerminateJobObject(job, 0);
    FinishPipeThread(inThread, *in, 200);
    FinishPipeThread(outThread, *out, 1500);
    FinishPipeThread(errThread, *err, 300);
    rec.stdoutText = Collected(*out);
    rec.stderrText = Collected(*err);
    {
        std::lock_guard lock(control.mutex);
        control.kill = nullptr;
        rec.cancelled = control.cancelled;
    }
    if (job) CloseHandle(job);
    CloseHandle(pi.hProcess);
    closeAll();
    rec.durationMs = GetTickCount64() - start;
    if (!IsValidUtf8(rec.stdoutText)) rec.stdoutText = AnsiToUtf8(rec.stdoutText);
    if (!IsValidUtf8(rec.stderrText)) rec.stderrText = AnsiToUtf8(rec.stderrText);
}

const char* PluginDocumentation() {
    static const std::string doc = [] {
        HMODULE module = GetModuleHandleW(nullptr);
        HRSRC res = FindResourceW(module, L"PLUGINS_MD", MAKEINTRESOURCEW(10));  // RT_RCDATA
        if (!res) return std::string();
        HGLOBAL data = LoadResource(module, res);
        const DWORD size = SizeofResource(module, res);
        const void* bytes = data ? LockResource(data) : nullptr;
        return bytes ? std::string(static_cast<const char*>(bytes), size) : std::string();
    }();
    return doc.c_str();
}

std::wstring PlatformConfigDirectory() { return ConfigDirectory(); }

std::wstring ResolveCommandLine(const std::wstring& run, const std::wstring& folder) {
    auto [first, rest] = SplitCommandLine(run);
    if (first.empty()) return run;
    for (auto& c : first)
        if (c == L'/') c = L'\\';
    std::wstring local;
    if (IsAbsolutePath(first) && FileExists(first)) {
        local = first;
    } else if (!folder.empty() && !IsAbsolutePath(first)) {
        std::wstring relative = first;
        if (relative.rfind(L".\\", 0) == 0) relative = relative.substr(2);
        const std::wstring candidate = folder + L"\\" + relative;
        if (FileExists(candidate)) local = candidate;
    }
    auto join = [&rest](const std::wstring& head) { return rest.empty() ? head : head + L" " + rest; };
    const std::wstring ext = ExtensionOf(first);
    if (!local.empty()) {
        const std::wstring q = QuoteArgument(local);
        if (ext == L".py" || ext == L".pyw") return join(PythonLauncher() + L" " + q);
        if (ext == L".js" || ext == L".mjs" || ext == L".cjs") return join(L"node " + q);
        if (ext == L".ps1")
            return join(L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " + q);
        if (ext == L".bat" || ext == L".cmd") return CmdInvocation(local, rest);
        return join(q);
    }
    // A batch file named with its extension ("tool.cmd"): CreateProcess would run it through cmd.exe
    // anyway, so say so - QueryUnsafeIn then keeps {query} out of a line cmd parses as code.
    if (ext == L".bat" || ext == L".cmd") {
        const std::wstring script = SearchPathFor(first, nullptr);
        if (!script.empty()) return CmdInvocation(script, rest);
    }
    // A program on PATH. CreateProcess only appends ".exe"; .cmd/.bat shims (npm, npx, code...) need cmd.exe.
    if (ext != L".exe" && ext != L".com" && SearchPathFor(first, L".exe").empty()) {
        std::wstring shim = SearchPathFor(first, L".cmd");
        if (shim.empty()) shim = SearchPathFor(first, L".bat");
        if (!shim.empty()) return CmdInvocation(shim, rest);
    }
    return run;
}

bool SpawnDetached(const std::wstring& commandLine, const std::wstring& directory, bool visibleConsole,
                   std::wstring* error) {
    std::wstring cmd = visibleConsole ? L"cmd.exe /d /k " + commandLine : ResolveCommandLine(commandLine, directory);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    AllowSetForegroundWindow(ASFW_ANY);
    const DWORD flags = visibleConsole ? CREATE_NEW_CONSOLE : CREATE_NO_WINDOW;
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, flags, nullptr,
                        directory.empty() || !DirectoryExists(directory) ? nullptr : directory.c_str(), &si, &pi)) {
        if (error) *error = log::ErrorText(GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

}  // namespace beam
