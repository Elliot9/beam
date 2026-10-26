#include "plugin/plugin_manager.hpp"

#include <algorithm>
#include <ctime>
#include <cwchar>
#include <deque>
#include <mutex>
#include <system_error>
#include <thread>

#include "core/settings.hpp"
#include "core/text.hpp"
#include "plugin/plugin_process.hpp"

namespace beam {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Filesystem helpers

fs::path FsPath(const std::wstring& path) {
#ifdef _WIN32
    return fs::path(path);
#else
    return fs::path(WideToUtf8(path));  // std::filesystem would convert through the C locale
#endif
}

std::wstring FromFsPath(const fs::path& path) {
#ifdef _WIN32
    return path.wstring();
#else
    return Utf8ToWide(path.string());
#endif
}

bool FsIsFile(const std::wstring& path) {
    std::error_code ec;
    return !path.empty() && fs::is_regular_file(FsPath(path), ec);
}

bool FsIsDirectory(const std::wstring& path) {
    std::error_code ec;
    return !path.empty() && fs::is_directory(FsPath(path), ec);
}

std::wstring FsFileName(const std::wstring& path) {
    fs::path p = FsPath(path);
    if (!p.has_filename()) p = p.parent_path();  // "plugins/echo/"
    return FromFsPath(p.filename());
}

std::wstring FsJoin(const std::wstring& dir, const std::wstring& name) { return FromFsPath(FsPath(dir) / FsPath(name)); }

namespace {

constexpr size_t kMaxRecords = 30;
constexpr wchar_t kBeamVersion[] = L"1.2.0";

std::wstring FirstLine(const std::string& utf8, size_t max = 160) {
    std::wstring text = Trim(Utf8ToWide(utf8));
    const size_t nl = text.find_first_of(L"\r\n");
    if (nl != std::wstring::npos) text.resize(nl);
    if (text.size() > max) text = text.substr(0, max) + L"…";
    return text;
}

std::wstring LastLine(const std::string& utf8, size_t max = 200) {
    std::wstring text = Trim(Utf8ToWide(utf8));
    const size_t nl = text.find_last_of(L"\r\n");
    if (nl != std::wstring::npos) text = text.substr(nl + 1);
    if (text.size() > max) text = text.substr(0, max) + L"…";
    return text;
}

std::wstring KindName(PluginRequestKind kind) {
    switch (kind) {
        case PluginRequestKind::Query: return L"query";
        case PluginRequestKind::Command: return L"run";
        case PluginRequestKind::Action: return L"action";
    }
    return L"query";
}

std::wstring LocalTimeStamp() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    wchar_t buf[16];
    std::swprintf(buf, 16, L"%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

PluginResult MakeResult(uint64_t ticket, PluginRequestKind kind, const std::wstring& commandId,
                        const std::wstring& query, uint32_t timeoutMs, const PluginRunRecord& rec) {
    PluginResult r;
    r.ticket = ticket;
    r.kind = kind;
    r.commandId = commandId;
    r.query = query;
    if (!rec.launchError.empty()) {
        r.failure = L"無法啟動外掛：" + rec.launchError;
        return r;
    }
    if (rec.timedOut) {
        r.failure = L"外掛逾時（超過 " + std::to_wstring((timeoutMs + 999) / 1000) + L" 秒）";
        return r;
    }
    r.response = ParsePluginResponse(rec.stdoutText);
    const bool noOutput = Trim(Utf8ToWide(rec.stdoutText)).empty();
    if (rec.exitCode != 0 && (noOutput || !r.response.json)) {
        r.failure = L"外掛結束代碼 " + std::to_wstring(rec.exitCode);
        const std::wstring err = LastLine(rec.stderrText);
        if (!err.empty()) r.failure += L"：" + err;
        return r;
    }
    if (kind != PluginRequestKind::Command && !noOutput && !r.response.json)
        r.failure = L"外掛輸出不是 JSON：" + FirstLine(rec.stdoutText);
    return r;
}

PluginLaunch Prepare(const PluginManager& manager, const PluginCommand& cmd, PluginRequestKind kind,
                     const std::wstring& query, const Json* item, const Json* action) {
    PluginLaunch l;
    l.directory = cmd.folder;
    const std::wstring dataDir = manager.DataDirectory(cmd);
    const std::wstring kindName = KindName(kind);
    l.request = BuildPluginRequest(kindName, cmd.spec.id, query, cmd.folder, dataDir, item, action);
    // Environment variables have size limits (128 KB each on Linux, 32 K characters on Windows);
    // the full query is always in the request on stdin.
    std::wstring envQuery = query.substr(0, 32000);
    if (!envQuery.empty() && envQuery.size() < query.size() && envQuery.back() >= 0xD800 && envQuery.back() <= 0xDBFF)
        envQuery.pop_back();  // do not cut a UTF-16 surrogate pair in half
    l.environment = {
        {L"BEAM_REQUEST_TYPE", kindName},
        {L"BEAM_QUERY", envQuery},
        {L"BEAM_COMMAND", cmd.spec.id},
        {L"BEAM_PLUGIN_DIR", cmd.folder},
        {L"BEAM_DATA_DIR", dataDir},
        {L"BEAM_VERSION", kBeamVersion},
        {L"PYTHONUTF8", L"1"},
        {L"PYTHONIOENCODING", L"utf-8"},
    };
    l.timeoutMs = static_cast<uint32_t>(cmd.spec.timeoutMs > 0 ? cmd.spec.timeoutMs : DefaultTimeoutMs(cmd.spec.mode));
    l.killTree = kind != PluginRequestKind::Command;
    PreparePluginLaunch(cmd, query, l);
    return l;
}

PluginRunRecord NewRecord(const PluginCommand& cmd, PluginRequestKind kind, const std::wstring& query,
                          const PluginLaunch& launch) {
    PluginRunRecord rec;
    rec.commandId = cmd.spec.id;
    rec.commandName = cmd.spec.name;
    rec.commandLine = launch.display;
    rec.kind = KindName(kind);
    rec.query = query;
    rec.request = launch.request;
    return rec;
}

void Run(const PluginLaunch& launch, PluginRunControl& control, PluginRunRecord& rec) {
    rec.when = LocalTimeStamp();
    if (!launch.prepareError.empty()) {
        rec.launchError = launch.prepareError;
        return;
    }
    ExecutePluginLaunch(launch, control, rec);
}

std::vector<std::wstring> SubFolders(const std::wstring& dir) {
    std::vector<std::wstring> names;
    std::error_code ec;
    for (fs::directory_iterator it(FsPath(dir), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_directory(e2)) names.push_back(FromFsPath(it->path().filename()));
    }
    std::sort(names.begin(), names.end(), [](const auto& a, const auto& b) { return Fold(a) < Fold(b); });
    return names;
}

}  // namespace

// ---------------------------------------------------------------------------
// Running

struct PluginManager::Shared {
    std::mutex mutex;
    std::vector<std::shared_ptr<PluginRunControl>> active;
    std::deque<PluginRunRecord> records;

    void Add(PluginRunRecord record) {
        std::lock_guard lock(mutex);
        records.push_front(std::move(record));
        while (records.size() > kMaxRecords) records.pop_back();
    }
    void Finish(const std::shared_ptr<PluginRunControl>& control, PluginRunRecord record) {
        std::lock_guard lock(mutex);
        active.erase(std::remove(active.begin(), active.end(), control), active.end());
        records.push_front(std::move(record));
        while (records.size() > kMaxRecords) records.pop_back();
    }
};

PluginManager::PluginManager() : shared_(std::make_shared<Shared>()) {}

PluginManager::~PluginManager() { CancelAll(); }

void PluginManager::Initialize(const std::wstring& configDirectory) {
    dir_ = FsJoin(configDirectory, L"plugins");
    dataRoot_ = FsJoin(configDirectory, L"plugin-data");
    std::error_code ec;
    fs::create_directories(FsPath(dir_), ec);
}

std::wstring PluginManager::DataDirectory(const PluginCommand& cmd) const {
    const std::wstring dir = FsJoin(dataRoot_, FsFileName(cmd.folder));
    std::error_code ec;
    fs::create_directories(FsPath(dir), ec);
    return dir;
}

uint64_t PluginManager::Start(PluginResultSink sink, const PluginCommand& cmd, PluginRequestKind kind,
                              const std::wstring& query, const Json* item, const Json* action) {
    PluginLaunch launch = Prepare(*this, cmd, kind, query, item, action);
    const uint64_t ticket = nextTicket_++;
    auto control = std::make_shared<PluginRunControl>();
    control->ticket = ticket;
    control->kind = kind;
    {
        std::lock_guard lock(shared_->mutex);
        if (kind == PluginRequestKind::Query) {
            for (auto& other : shared_->active)
                if (other->kind == PluginRequestKind::Query) other->Cancel();
        }
        shared_->active.push_back(control);
    }
    PluginRunRecord rec = NewRecord(cmd, kind, query, launch);
    std::thread([shared = shared_, control, launch = std::move(launch), rec = std::move(rec), sink = std::move(sink),
                 ticket, kind, id = cmd.spec.id, query]() mutable {
        Run(launch, *control, rec);
        auto result = std::make_unique<PluginResult>(MakeResult(ticket, kind, id, query, launch.timeoutMs, rec));
        const bool cancelled = rec.cancelled;
        shared->Finish(control, std::move(rec));
        if (!cancelled && sink) sink(std::move(result));
    }).detach();
    return ticket;
}

void PluginManager::Cancel(uint64_t ticket) {
    std::lock_guard lock(shared_->mutex);
    for (auto& control : shared_->active)
        if (control->ticket == ticket) control->Cancel();
}

void PluginManager::CancelAll() {
    std::lock_guard lock(shared_->mutex);
    for (auto& control : shared_->active)
        if (control->kind != PluginRequestKind::Command) control->Cancel();
}

PluginResult PluginManager::RunBlocking(const PluginCommand& cmd, PluginRequestKind kind, const std::wstring& query,
                                        const Json* item, const Json* action, PluginRunRecord* record) {
    const PluginLaunch launch = Prepare(*this, cmd, kind, query, item, action);
    const uint64_t ticket = nextTicket_++;
    PluginRunControl control;
    control.ticket = ticket;
    control.kind = kind;
    PluginRunRecord rec = NewRecord(cmd, kind, query, launch);
    Run(launch, control, rec);
    PluginResult result = MakeResult(ticket, kind, cmd.spec.id, query, launch.timeoutMs, rec);
    if (record) *record = rec;
    shared_->Add(std::move(rec));
    return result;
}

std::vector<PluginRunRecord> PluginManager::Records() const {
    std::lock_guard lock(shared_->mutex);
    return {shared_->records.begin(), shared_->records.end()};
}

std::wstring PluginManager::DescribeRecords() const {
    std::wstring text;
    if (!problems_.empty()) {
        text += L"載入問題\n";
        for (const auto& p : problems_) text += L"  • " + p + L"\n";
        text += L"\n";
    }
    const auto records = Records();
    if (records.empty()) text += L"還沒有外掛執行紀錄。\n";
    auto clip = [](const std::string& s, size_t max) {
        std::wstring w = Utf8ToWide(s);
        while (!w.empty() && (w.back() == L'\n' || w.back() == L'\r')) w.pop_back();
        if (w.size() > max) w = w.substr(0, max) + L"\n…（已截斷）";
        return w;
    };
    for (const auto& r : records) {
        text += L"━━ " + r.when + L"  " + r.commandName + L"  [" + r.kind + L"]  「" + r.query + L"」  " +
                std::to_wstring(r.durationMs) + L" ms";
        if (!r.launchError.empty()) text += L"  無法啟動";
        else if (r.timedOut) text += L"  逾時";
        else if (r.cancelled) text += L"  已取消";
        else text += L"  exit " + std::to_wstring(r.exitCode);
        text += L"\n> " + r.commandLine + L"\n";
        if (!r.launchError.empty()) text += L"錯誤：" + r.launchError + L"\n";
        text += L"stdin:  " + clip(r.request, 600) + L"\n";
        text += L"stdout: " + (r.stdoutText.empty() ? std::wstring(L"（空）") : clip(r.stdoutText, 3000)) + L"\n";
        if (!r.stderrText.empty()) text += L"stderr: " + clip(r.stderrText, 3000) + L"\n";
        text += L"\n";
    }
    return text;
}

// ---------------------------------------------------------------------------
// Loading

std::vector<PluginCommand> PluginManager::LoadFolder(const std::wstring& folder, std::vector<std::wstring>* problems) {
    std::vector<PluginCommand> out;
    const std::wstring manifest = FsJoin(folder, L"plugin.json");
    if (!FsIsFile(manifest)) return out;
    const std::wstring folderName = FsFileName(folder);
    std::string text;
    if (!ReadTextFile(FsPath(manifest), text)) {
        if (problems) problems->push_back(folderName + L": 無法讀取 plugin.json");
        return out;
    }
    std::vector<std::string> issues;
    const auto parsed = ParsePluginManifest(text, folderName, &issues);
    if (problems)
        for (const auto& issue : issues) problems->push_back(folderName + L": " + Utf8ToWide(issue));
    if (!parsed) return out;
    for (const auto& spec : parsed->commands) out.push_back(PluginCommand{spec, parsed->name, folder, manifest});
    return out;
}

std::wstring PluginManager::Signature() const {
    std::vector<std::wstring> parts;
    std::error_code ec;
    for (fs::directory_iterator it(FsPath(dir_), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_directory(e2)) continue;
        std::wstring part = FromFsPath(it->path().filename());
        const fs::path manifest = it->path() / "plugin.json";
        const auto time = fs::last_write_time(manifest, e2);
        if (!e2) {
            const auto size = fs::file_size(manifest, e2);
            part += L"|" + std::to_wstring(time.time_since_epoch().count()) + L"." +
                    std::to_wstring(e2 ? 0 : static_cast<unsigned long long>(size));
        }
        parts.push_back(std::move(part));
    }
    std::sort(parts.begin(), parts.end());
    std::wstring sig;
    for (const auto& p : parts) sig += p + L";";
    return sig;
}

void PluginManager::Reload() {
    commands_.clear();
    problems_.clear();
    for (const auto& name : SubFolders(dir_)) {
        auto cmds = LoadFolder(FsJoin(dir_, name), &problems_);
        for (auto& c : cmds) {
            if (!c.spec.keyword.empty()) {
                if (const PluginCommand* other = FindByKeyword(c.spec.keyword)) {
                    problems_.push_back(c.spec.id + L": 關鍵字「" + c.spec.keyword + L"」已被 " + other->spec.id +
                                        L" 使用，已停用");
                    c.spec.keyword.clear();
                }
            }
            commands_.push_back(std::move(c));
        }
    }
    signature_ = Signature();
    Log(L"Loaded " + std::to_wstring(commands_.size()) + L" plugin command(s)");
    for (const auto& p : problems_) Log(L"plugin: " + p);
}

bool PluginManager::ReloadIfChanged() {
    if (Signature() == signature_) return false;
    Reload();
    return true;
}

const PluginCommand* PluginManager::Find(const std::wstring& id) const {
    for (const auto& c : commands_)
        if (c.spec.id == id) return &c;
    return nullptr;
}

const PluginCommand* PluginManager::FindByKeyword(std::wstring_view keyword) const {
    if (keyword.empty()) return nullptr;
    for (const auto& c : commands_)
        if (!c.spec.keyword.empty() && EqualsIgnoreCase(c.spec.keyword, keyword)) return &c;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Templates

namespace {

std::wstring SanitizeFolderName(const std::wstring& name) {
    std::wstring out;
    for (const wchar_t c : Trim(name)) {
        if (c < 0x20 || std::wstring_view(L"<>:\"/\\|?*").find(c) != std::wstring_view::npos) continue;
        out.push_back(c == L' ' ? L'-' : c);
    }
    while (!out.empty() && (out.back() == L'.' || out.back() == L'-')) out.pop_back();
    while (!out.empty() && out.front() == L'.') out.erase(out.begin());  // no hidden folders
    if (out.size() > 60) out.resize(60);
    return out;
}

// ASCII letters/digits of the name ("GitHub Repos" -> "githubrepos"); empty for e.g. "匯率查詢".
std::wstring KeywordFrom(const std::wstring& name) {
    std::wstring kw;
    for (const wchar_t c : name) {
        const wchar_t f = FoldChar(c);
        if ((f >= L'a' && f <= L'z') || (f >= L'0' && f <= L'9')) kw.push_back(f);
        if (kw.size() >= 12) break;
    }
    return kw;
}

// Keywords a new plugin must not take: quicklink keywords (settings.json is read, never
// created here) and Beam's own "newplugin".
std::vector<std::wstring> ReservedKeywords(const std::wstring& pluginsDir) {
    std::vector<std::wstring> reserved{L"newplugin"};
    std::vector<Quicklink> links = DefaultQuicklinks();
    std::string text;
    if (ReadTextFile(FsPath(pluginsDir).parent_path() / "settings.json", text)) {
        if (const auto json = Json::Parse(text)) links = SettingsFromJson(*json).quicklinks;
    }
    for (const auto& link : links)
        if (!link.keyword.empty()) reserved.push_back(link.keyword);
    return reserved;
}

bool WriteUtf8File(const std::wstring& path, const std::string& content, bool bom) {
    return WriteTextFileAtomic(FsPath(path), bom ? "\xEF\xBB\xBF" + content : content);
}

}  // namespace

bool PluginManager::CreateFromTemplate(const std::wstring& name, PluginTemplate language, std::wstring* folder,
                                       std::wstring* keyword, std::wstring* error) {
    const std::wstring display = Trim(name).empty() ? std::wstring(L"My Plugin") : Trim(name);
    std::wstring base = SanitizeFolderName(display);
    if (base.empty()) base = L"my-plugin";
    std::error_code ec;
    fs::create_directories(FsPath(dir_), ec);
    std::wstring target = FsJoin(dir_, base);
    for (int n = 2; fs::exists(FsPath(target), ec); ++n) target = FsJoin(dir_, base + L"-" + std::to_wstring(n));

    // A keyword nothing else uses; names without ASCII letters get p1, p2, ... (a real word such as
    // "plugin" would capture ordinary searches like "plugin folder" once a space is typed).
    const std::vector<std::wstring> reserved = ReservedKeywords(dir_);
    auto taken = [&](const std::wstring& k) {
        if (FindByKeyword(k)) return true;
        for (const auto& r : reserved)
            if (EqualsIgnoreCase(r, k)) return true;
        return false;
    };
    std::wstring kw = KeywordFrom(display);
    if (kw.empty() || taken(kw)) {
        const std::wstring stem = kw.empty() ? std::wstring(L"p") : kw;
        for (int n = kw.empty() ? 1 : 2;; ++n) {
            if (!taken(stem + std::to_wstring(n))) {
                kw = stem + std::to_wstring(n);
                break;
            }
        }
    }
    if (!fs::create_directory(FsPath(target), ec)) {
        if (error) *error = Utf8ToWide(ec ? ec.message() : std::string("folder exists"));
        return false;
    }
    const std::wstring script = FsJoin(target, PluginScriptFileName(language));
    const bool ok = WriteUtf8File(FsJoin(target, L"plugin.json"), PluginManifestTemplate(language, display, kw), false) &&
                    WriteUtf8File(script, PluginScriptTemplate(language, display),
                                  language == PluginTemplate::PowerShell);  // Windows PowerShell 5.1 needs the BOM
    if (!ok) {
        if (error) *error = L"無法寫入外掛檔案";
        return false;
    }
    if (language == PluginTemplate::Shell) {
        fs::permissions(FsPath(script), fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                        fs::perm_options::add, ec);
    }
    EnsureDocumentation();
    if (folder) *folder = target;
    if (keyword) *keyword = kw;
    return true;
}

void PluginManager::EnsureDocumentation() const {
    const std::string doc = PluginDocumentation();
    if (doc.empty() || dir_.empty()) return;
    const std::wstring path = FsJoin(dir_, L"PLUGINS.md");
    std::string existing;
    if (ReadTextFile(FsPath(path), existing) && existing == doc) return;
    WriteUtf8File(path, doc, false);
}

}  // namespace beam
