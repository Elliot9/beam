#include "win/apps.hpp"

#include <functional>
#include <unordered_set>

#include "core/text.hpp"
#include "win/common.hpp"
#include "win/log.hpp"
#include "win/shell.hpp"

namespace beam {
namespace {

// Defined locally so we do not depend on which GUIDs a given SDK's uuid.lib exports.
const GUID kBhidEnumItems = {0x94f60519, 0x2850, 0x4924, {0xaa, 0x5a, 0xd1, 0x5e, 0x84, 0x86, 0x80, 0x39}};

bool IsJunkName(const std::wstring& name) {
    static const wchar_t* kWords[] = {L"uninstall", L"解除安裝", L"卸載", L"卸载", L"アンインストール", L"제거"};
    for (const wchar_t* w : kWords)
        if (FindIgnoreCase(name, w) != std::wstring::npos) return true;
    return false;
}

bool IsDocumentExtension(const std::wstring& ext) {
    static const wchar_t* kDocs[] = {L".txt", L".rtf", L".pdf", L".chm", L".hlp", L".htm", L".html", L".mht",
                                     L".md",  L".log", L".ini", L".xml", L".doc", L".docx", L".url", L".website",
                                     L".ico", L".png", L".jpg", L".nfo", L".lic"};
    for (const wchar_t* d : kDocs)
        if (ext == d) return true;
    return false;
}

bool IsWebUrl(const std::wstring& s) {
    return StartsWithIgnoreCase(s, L"http:") || StartsWithIgnoreCase(s, L"https:") ||
           StartsWithIgnoreCase(s, L"ftp:") || StartsWithIgnoreCase(s, L"file:") ||
           StartsWithIgnoreCase(s, L"mailto:");
}

std::wstring ShellDisplayName(const std::wstring& path) {
    SHFILEINFOW info{};
    if (SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info), SHGFI_DISPLAYNAME) && info.szDisplayName[0])
        return info.szDisplayName;
    return StemOf(path);
}

std::wstring ItemName(IShellItem* item, SIGDN kind) {
    PWSTR raw = nullptr;
    std::wstring name;
    if (SUCCEEDED(item->GetDisplayName(kind, &raw)) && raw) name = raw;
    if (raw) CoTaskMemFree(raw);
    return name;
}

std::wstring ResolveShortcut(const std::wstring& lnk) {
    ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, link.PutVoid())))
        return {};
    ComPtr<IPersistFile> file;
    if (FAILED(link->QueryInterface(IID_IPersistFile, file.PutVoid()))) return {};
    if (FAILED(file->Load(lnk.c_str(), STGM_READ))) return {};
    wchar_t path[MAX_PATH * 2] = {};
    if (link->GetPath(path, ARRAYSIZE(path), nullptr, 0) != S_OK) return {};  // S_FALSE: no file target
    return path;
}

std::wstring InternetShortcutUrl(const std::wstring& file) {
    wchar_t url[2048] = {};
    GetPrivateProfileStringW(L"InternetShortcut", L"URL", L"", url, ARRAYSIZE(url), file.c_str());
    return url;
}

const std::atomic<bool>* g_cancel = nullptr;  // set for the duration of one scan (scans never overlap)
bool Cancelled() { return g_cancel && g_cancel->load(); }

void Walk(const std::wstring& dir, int depth, int maxDepth, const std::function<void(const std::wstring&)>& onFile) {
    if (Cancelled()) return;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        const std::wstring full = dir + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && depth < maxDepth)
                Walk(full, depth + 1, maxDepth, onFile);
        } else if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM)) {
            onFile(full);
        }
    } while (!Cancelled() && FindNextFileW(h, &fd));
    FindClose(h);
}

class Scanner {
public:
    std::vector<AppEntry> Take() { return std::move(apps_); }

    void AddStartMenu(const std::wstring& root) {
        if (root.empty()) return;
        Walk(root, 0, 6, [this](const std::wstring& f) { AddShortcutLike(f); });
    }

    void AddDesktop(const std::wstring& root) {
        if (root.empty()) return;
        Walk(root, 0, 0, [this](const std::wstring& f) {
            const std::wstring ext = ExtensionOf(f);
            if (ext == L".lnk" || ext == L".url" || ext == L".appref-ms") AddShortcutLike(f);
        });
    }

    void AddAppsFolder() {
        ComPtr<IShellItem> folder;
        if (FAILED(SHCreateItemFromParsingName(L"shell:AppsFolder", nullptr, IID_IShellItem, folder.PutVoid())))
            return;
        ComPtr<IEnumShellItems> items;
        if (FAILED(folder->BindToHandler(nullptr, kBhidEnumItems, IID_IEnumShellItems, items.PutVoid()))) return;
        while (!Cancelled()) {
            ComPtr<IShellItem> item;
            ULONG fetched = 0;
            if (items->Next(1, item.Put(), &fetched) != S_OK || fetched == 0) break;
            const std::wstring name = ItemName(item.Get(), SIGDN_NORMALDISPLAY);
            const std::wstring parsing = ItemName(item.Get(), SIGDN_PARENTRELATIVEPARSING);
            if (name.empty() || parsing.empty() || IsJunkName(name) || IsWebUrl(parsing)) continue;
            if (IsDocumentExtension(ExtensionOf(parsing))) continue;
            if (!seenNames_.insert(Fold(name)).second) continue;  // already indexed from a shortcut

            AppEntry e;
            e.name = name;
            e.launch = L"shell:AppsFolder\\" + parsing;
            e.iconPath = e.launch;
            e.key = L"app:" + Fold(e.launch);
            const size_t bang = parsing.find(L'!');
            if (bang != std::wstring::npos) {
                e.kind = AppKind::Packaged;
                std::wstring family = parsing.substr(0, bang);
                family = family.substr(0, family.find(L'_'));           // drop publisher hash
                const size_t dot = family.find_last_of(L'.');
                AddAlias(e, dot == std::wstring::npos ? family : family.substr(dot + 1));
            } else {
                e.kind = AppKind::Shortcut;
                if (parsing.find(L'\\') != std::wstring::npos) AddAlias(e, StemOf(parsing));
            }
            apps_.push_back(std::move(e));
        }
    }

    void AddExtraFolder(const std::wstring& folder) {
        const std::wstring root = ExpandUserPath(folder);
        if (!DirectoryExists(root)) {
            log::Info(L"extraFolders: not found: " + root);
            return;
        }
        Walk(root, 0, 3, [this](const std::wstring& f) {
            const std::wstring ext = ExtensionOf(f);
            if (ext == L".lnk" || ext == L".url" || ext == L".appref-ms") {
                AddShortcutLike(f, /*fromToolFolder=*/true);
                return;
            }
            const bool program = ext == L".exe" || ext == L".msc";
            const bool script = ext == L".bat" || ext == L".cmd" || ext == L".ps1";
            if (!program && !script) return;
            if (!seenPaths_.insert(Fold(f)).second) return;
            AppEntry e;
            e.name = StemOf(f);
            e.kind = script ? AppKind::Script : AppKind::Program;
            e.target = f;
            e.iconPath = f;
            e.subtitle = ParentOf(f);
            if (ext == L".ps1") {
                e.launch = L"powershell.exe";
                e.arguments = L"-NoExit -NoProfile -ExecutionPolicy Bypass -File \"" + f + L"\"";
            } else {
                e.launch = f;
            }
            e.key = L"app:" + Fold(f);
            apps_.push_back(std::move(e));
        });
    }

private:
    static void AddAlias(AppEntry& e, const std::wstring& alias) {
        if (alias.empty() || EqualsIgnoreCase(alias, e.name)) return;
        for (const auto& a : e.aliases)
            if (EqualsIgnoreCase(a, alias)) return;
        e.aliases.push_back(alias);
    }

    void AddShortcutLike(const std::wstring& path, bool fromToolFolder = false) {
        const std::wstring ext = ExtensionOf(path);
        AppEntry e;
        if (ext == L".lnk") {
            e.kind = AppKind::Shortcut;
            e.target = ResolveShortcut(path);
            if (!e.target.empty()) {
                if (IsDocumentExtension(ExtensionOf(e.target)) || DirectoryExists(e.target)) return;
                if (StartsWithIgnoreCase(StemOf(e.target), L"unins")) return;
            }
        } else if (ext == L".url") {
            const std::wstring url = InternetShortcutUrl(path);
            if (url.empty() || IsWebUrl(url)) return;  // keep steam://, epic:// ... launchers only
            e.kind = AppKind::Protocol;
        } else if (ext == L".appref-ms" || ext == L".exe") {
            e.kind = ext == L".exe" ? AppKind::Program : AppKind::Shortcut;
            if (ext == L".exe") e.target = path;
        } else {
            return;
        }
        e.name = ShellDisplayName(path);
        if (e.name.empty() || IsJunkName(e.name)) return;
        if (fromToolFolder) {
            if (!seenPaths_.insert(Fold(path)).second) return;
            e.subtitle = ParentOf(path);
        } else if (!seenNames_.insert(Fold(e.name)).second) {
            return;
        }
        e.launch = path;
        e.iconPath = path;
        e.key = L"app:" + Fold(path);
        AddAlias(e, StemOf(path));
        if (!e.target.empty() && ExtensionOf(e.target) == L".exe") {
            AddAlias(e, StemOf(e.target));
            if (e.subtitle.empty()) e.subtitle = FileNameOf(e.target);
        }
        apps_.push_back(std::move(e));
    }

    std::vector<AppEntry> apps_;
    std::unordered_set<std::wstring> seenNames_;
    std::unordered_set<std::wstring> seenPaths_;
};

}  // namespace

std::vector<AppEntry> ScanApplications(const std::vector<std::wstring>& extraFolders,
                                       const std::atomic<bool>* cancel) {
    const ULONGLONG start = GetTickCount64();
    g_cancel = cancel;
    Scanner scanner;
    scanner.AddStartMenu(KnownFolder(FOLDERID_Programs));
    scanner.AddStartMenu(KnownFolder(FOLDERID_CommonPrograms));
    scanner.AddAppsFolder();
    scanner.AddDesktop(KnownFolder(FOLDERID_Desktop));
    scanner.AddDesktop(KnownFolder(FOLDERID_PublicDesktop));
    for (const auto& folder : extraFolders) scanner.AddExtraFolder(folder);
    auto apps = scanner.Take();
    g_cancel = nullptr;
    log::Info(L"Indexed " + std::to_wstring(apps.size()) + L" apps in " +
              std::to_wstring(GetTickCount64() - start) + L" ms");
    return apps;
}

}  // namespace beam
