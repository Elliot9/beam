#include "linux/apps.hpp"

#include <gio/gdesktopappinfo.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <set>
#include <system_error>

#include "core/text.hpp"
#include "linux/system.hpp"

namespace beam {
namespace {

constexpr char kOwnDesktopId[] = "local.beam.Launcher.desktop";

std::wstring Shorten(const std::wstring& text, size_t max) {
    return text.size() > max ? text.substr(0, max - 1) + L"…" : text;
}

void AddAlias(std::vector<std::wstring>& aliases, const std::wstring& name, const std::wstring& alias) {
    const std::wstring a = Trim(alias);
    if (a.empty() || EqualsIgnoreCase(a, name)) return;
    for (const auto& existing : aliases)
        if (EqualsIgnoreCase(existing, a)) return;
    aliases.push_back(a);
}

AppEntry FromDesktopInfo(GDesktopAppInfo* info) {
    GAppInfo* app = G_APP_INFO(info);
    AppEntry e;
    e.desktopId = g_app_info_get_id(app) ? g_app_info_get_id(app) : "";
    e.key = L"app:" + Fold(Utf8ToWide(e.desktopId));
    e.name = W(g_app_info_get_display_name(app));
    if (const char* generic = g_desktop_app_info_get_generic_name(info)) e.subtitle = Shorten(W(generic), 40);
    if (const char* file = g_desktop_app_info_get_filename(info)) e.desktopFile = file;
    if (const char* cmd = g_app_info_get_commandline(app)) e.commandLine = W(cmd);
    // Searchable by the untranslated name too ("Text Editor" as well as "文字編輯器").
    if (gchar* raw = g_desktop_app_info_get_string(info, "Name")) {
        AddAlias(e.aliases, e.name, W(raw));
        g_free(raw);
    }
    if (const char* generic = g_desktop_app_info_get_generic_name(info)) AddAlias(e.aliases, e.name, W(generic));
    if (const char* const* keywords = g_desktop_app_info_get_keywords(info)) {
        for (int i = 0; keywords[i] && i < 12; ++i) AddAlias(e.aliases, e.name, W(keywords[i]));
    }
    if (const char* exe = g_app_info_get_executable(app)) {
        const std::wstring base = FileNameOf(W(exe));
        if (base != L"env" && base != L"flatpak" && base != L"snap") AddAlias(e.aliases, e.name, base);
    }
    e.icon = GRef<GIcon>::Ref(g_app_info_get_icon(app));
    if (const gchar* const* actions = g_desktop_app_info_list_actions(info)) {
        for (int i = 0; actions[i] && i < 8; ++i) {
            gchar* label = g_desktop_app_info_get_action_name(info, actions[i]);
            if (label && *label) e.actions.emplace_back(actions[i], W(label));
            g_free(label);
        }
    }
    return e;
}

void ScanFolder(const std::filesystem::path& folder, int depth, std::set<std::string>& seen,
                std::vector<AppEntry>& out) {
    std::error_code ec;
    for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        if (out.size() > 4000) return;
        const std::string name = it->path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        std::error_code e2;
        if (it->is_directory(e2)) {
            if (depth > 0) ScanFolder(it->path(), depth - 1, seen, out);
            continue;
        }
        if (!it->is_regular_file(e2) || access(it->path().c_str(), X_OK) != 0) continue;
        if (!seen.insert(it->path().string()).second) continue;
        AppEntry e;
        e.program = Utf8ToWide(it->path().string());
        e.key = L"app:" + e.program;
        e.name = Utf8ToWide(name);
        e.subtitle = Utf8ToWide(folder.string());
        e.commandLine = e.program;
        const size_t dot = name.rfind('.');
        if (dot != std::string::npos && dot > 0) AddAlias(e.aliases, e.name, Utf8ToWide(name.substr(0, dot)));
        out.push_back(std::move(e));
    }
}

}  // namespace

std::vector<AppEntry> ScanApplications(const std::vector<std::wstring>& extraFolders) {
    std::vector<AppEntry> apps;
    GList* all = g_app_info_get_all();
    for (GList* l = all; l; l = l->next) {
        GAppInfo* app = G_APP_INFO(l->data);
        if (!G_IS_DESKTOP_APP_INFO(app) || !g_app_info_should_show(app)) continue;
        const char* id = g_app_info_get_id(app);
        if (!id || std::string(id) == kOwnDesktopId) continue;
        apps.push_back(FromDesktopInfo(G_DESKTOP_APP_INFO(app)));
    }
    g_list_free_full(all, g_object_unref);

    std::set<std::string> seen;
    for (const auto& folder : extraFolders) {
        const std::wstring path = ExpandUserPath(folder);
        if (path.empty() || IsRemotePath(path) || !DirectoryExists(path)) continue;
        ScanFolder(std::filesystem::path(WideToUtf8(path)), 3, seen, apps);  // like Windows: 3 levels down
    }
    return apps;
}

}  // namespace beam
