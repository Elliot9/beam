#include "linux/system.hpp"

#include <gio/gdesktopappinfo.h>
#include <gtk/gtk.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <mutex>
#include <sstream>

#include "core/query.hpp"
#include "core/text.hpp"
#include "linux/results.hpp"
#include "plugin/plugin_process.hpp"

namespace beam {
namespace {

constexpr char kAppId[] = "local.beam.Launcher";

std::string Narrow(const std::wstring& s) { return WideToUtf8(s); }

GAppLaunchContext* LaunchContext() {  // new reference or nullptr
    GdkDisplay* display = gdk_display_get_default();
    return display ? G_APP_LAUNCH_CONTEXT(gdk_display_get_app_launch_context(display)) : nullptr;
}

std::wstring ErrorMessage(GError* error) {
    std::wstring text = error ? Utf8ToWide(error->message) : std::wstring(L"未知的錯誤");
    if (error) g_error_free(error);
    return text;
}

bool SpawnSimple(const std::vector<std::string>& argv) {
    std::wstring ignored;
    return SpawnWithActivation(argv, L"", &ignored);
}

// Mount points of network / FUSE filesystems, from /proc/self/mounts (no I/O on the mounts).
std::vector<std::string> RemoteMounts() {
    static std::mutex mutex;
    static std::vector<std::string> cached;
    static gint64 loadedAt = 0;
    std::lock_guard lock(mutex);
    const gint64 now = g_get_monotonic_time();
    if (loadedAt && now - loadedAt < 30 * G_USEC_PER_SEC) return cached;
    loadedAt = now;
    cached.clear();
    std::ifstream mounts("/proc/self/mounts");
    std::string device, point, type;
    std::string rest;
    while (mounts >> device >> point >> type && std::getline(mounts, rest)) {
        static const char* kRemote[] = {"nfs", "nfs4", "cifs", "smb3", "smbfs", "sshfs", "fuse.sshfs", "fuse.gvfsd-fuse",
                                        "fuse.rclone", "9p", "davfs", "fuse.davfs", "ceph", "glusterfs", "afs", "fuse.s3fs"};
        if (std::find_if(std::begin(kRemote), std::end(kRemote), [&](const char* t) { return type == t; }) ==
            std::end(kRemote))
            continue;
        // "\040" escapes in mount points
        std::string decoded;
        for (size_t i = 0; i < point.size(); ++i) {
            if (point[i] == '\\' && i + 3 < point.size()) {
                decoded.push_back(static_cast<char>(std::strtol(point.substr(i + 1, 3).c_str(), nullptr, 8)));
                i += 3;
            } else {
                decoded.push_back(point[i]);
            }
        }
        cached.push_back(decoded);
    }
    return cached;
}

}  // namespace

std::wstring HomeDir() { return Utf8ToWide(g_get_home_dir()); }

std::wstring ExpandUserPath(const std::wstring& input) {
    std::wstring path = Trim(input);
    if (path == L"~") return HomeDir();
    if (path.rfind(L"~/", 0) == 0) path = HomeDir() + path.substr(1);
    std::wstring out;
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] != L'$' || i + 1 >= path.size()) {
            out.push_back(path[i]);
            continue;
        }
        size_t start = i + 1, end;
        bool braces = path[start] == L'{';
        if (braces) {
            end = path.find(L'}', start);
            if (end == std::wstring::npos) {
                out.push_back(path[i]);
                continue;
            }
            ++start;
        } else {
            end = start;
            while (end < path.size() && (std::iswalnum(path[end]) || path[end] == L'_')) ++end;
        }
        const std::wstring name = path.substr(start, end - start);
        const char* value = name.empty() ? nullptr : g_getenv(Narrow(name).c_str());
        if (!value) {
            out.append(path, i, (braces ? end + 1 : end) - i);
        } else {
            out += Utf8ToWide(value);
        }
        i = braces ? end : end - 1;
    }
    return out;
}

bool FileExists(const std::wstring& path) {
    struct stat st {};
    return !path.empty() && stat(Narrow(path).c_str(), &st) == 0;
}

bool DirectoryExists(const std::wstring& path) {
    struct stat st {};
    return !path.empty() && stat(Narrow(path).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool IsExecutableFile(const std::wstring& path) {
    struct stat st {};
    const std::string p = Narrow(path);
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(p.c_str(), X_OK) == 0;
}

std::wstring FileNameOf(const std::wstring& path) {
    std::wstring p = path;
    while (p.size() > 1 && p.back() == L'/') p.pop_back();
    const size_t slash = p.rfind(L'/');
    return slash == std::wstring::npos ? p : p.substr(slash + 1);
}

std::wstring ParentOf(const std::wstring& path) {
    std::wstring p = path;
    while (p.size() > 1 && p.back() == L'/') p.pop_back();
    const size_t slash = p.rfind(L'/');
    if (slash == std::wstring::npos) return L"";
    return slash == 0 ? L"/" : p.substr(0, slash);
}

std::wstring ExtensionOf(const std::wstring& path) {
    const std::wstring name = FileNameOf(path);
    const size_t dot = name.rfind(L'.');
    if (dot == std::wstring::npos || dot == 0) return L"";
    return Fold(name.substr(dot));
}

std::wstring FindProgram(const std::wstring& name) {
    gchar* found = g_find_program_in_path(Narrow(name).c_str());
    std::wstring result = found ? Utf8ToWide(found) : std::wstring();
    g_free(found);
    return result;
}

bool IsRemotePath(const std::wstring& path) {
    const std::string p = Narrow(ExpandUserPath(path));
    if (p.find("/gvfs/") != std::string::npos) return true;
    for (const auto& mount : RemoteMounts()) {
        if (mount == "/") continue;
        if (p == mount || (p.rfind(mount, 0) == 0 && p.size() > mount.size() && p[mount.size()] == '/')) return true;
    }
    return false;
}

std::string ExecutablePath() {
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    std::string path = n > 0 ? std::string(buf, static_cast<size_t>(n)) : std::string("beam");
    // After a package upgrade replaced the running binary, the kernel reports "/usr/bin/beam (deleted)".
    constexpr char kDeleted[] = " (deleted)";
    if (path.size() > sizeof kDeleted - 1 && path.compare(path.size() - (sizeof kDeleted - 1), std::string::npos, kDeleted) == 0)
        path.resize(path.size() - (sizeof kDeleted - 1));
    return path;
}

bool SpawnWithActivation(const std::vector<std::string>& argv, const std::wstring& directory, std::wstring* error) {
    if (argv.empty()) return false;
    if (g_getenv("BEAM_DEBUG")) {
        std::string line;
        for (const auto& a : argv) line += (line.empty() ? "" : " ") + a;
        g_printerr("beam: spawn [%s] in %s\n", line.c_str(), Narrow(directory).c_str());
    }
    gchar** envp = g_get_environ();
    // An activation token from the compositor lets the new window take focus (Wayland), or a
    // startup-notification id on X11.
    if (GAppLaunchContext* ctx = LaunchContext()) {
        GAppInfo* info = g_app_info_create_from_commandline(argv[0].c_str(), nullptr,
                                                            G_APP_INFO_CREATE_SUPPORTS_STARTUP_NOTIFICATION, nullptr);
        if (info) {
            if (gchar* token = g_app_launch_context_get_startup_notify_id(ctx, info, nullptr)) {
                envp = g_environ_setenv(envp, "XDG_ACTIVATION_TOKEN", token, TRUE);
                envp = g_environ_setenv(envp, "DESKTOP_STARTUP_ID", token, TRUE);
                g_free(token);
            }
            g_object_unref(info);
        }
        g_object_unref(ctx);
    }
    std::vector<gchar*> args;
    for (const auto& a : argv) args.push_back(const_cast<gchar*>(a.c_str()));
    args.push_back(nullptr);
    const std::string dir = Narrow(directory);
    GError* err = nullptr;
    const gboolean ok =
        g_spawn_async(dir.empty() || !DirectoryExists(directory) ? nullptr : dir.c_str(), args.data(), envp,
                      static_cast<GSpawnFlags>(G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                                               G_SPAWN_STDERR_TO_DEV_NULL),
                      nullptr, nullptr, nullptr, &err);
    g_strfreev(envp);
    if (!ok) {
        const std::wstring message = ErrorMessage(err);
        if (error) *error = message;
        return false;
    }
    return true;
}

bool OpenTarget(const std::wstring& target, std::wstring* error) {
    std::string uri;
    const std::wstring t = Trim(target);
    const size_t colon = t.find(L':');
    const bool hasScheme = colon != std::wstring::npos && colon > 1 &&
                           std::all_of(t.begin(), t.begin() + static_cast<long>(colon), [](wchar_t c) {
                               return std::iswalnum(c) || c == L'+' || c == L'-' || c == L'.';
                           });
    if (hasScheme) {
        uri = Narrow(t);
    } else {
        const std::wstring path = ExpandUserPath(t);
        if (!FileExists(path)) {
            if (error) *error = L"找不到 " + path;
            return false;
        }
        gchar* u = g_filename_to_uri(Narrow(path).c_str(), nullptr, nullptr);
        uri = u ? u : "";
        g_free(u);
    }
    GAppLaunchContext* ctx = LaunchContext();
    GError* err = nullptr;
    const gboolean ok = g_app_info_launch_default_for_uri(uri.c_str(), ctx, &err);
    if (ctx) g_object_unref(ctx);
    if (!ok) {
        const std::wstring message = ErrorMessage(err);
        if (error) *error = message;
        return false;
    }
    return true;
}

bool OpenInTextEditor(const std::wstring& path, std::wstring* error) {
    GAppInfo* editor = g_app_info_get_default_for_type("text/plain", FALSE);
    if (!editor) return OpenTarget(path, error);
    GFile* file = g_file_new_for_path(Narrow(ExpandUserPath(path)).c_str());
    GList* files = g_list_prepend(nullptr, file);
    GAppLaunchContext* ctx = LaunchContext();
    GError* err = nullptr;
    const gboolean ok = g_app_info_launch(editor, files, ctx, &err);
    if (ctx) g_object_unref(ctx);
    g_list_free(files);
    g_object_unref(file);
    g_object_unref(editor);
    if (!ok) {
        const std::wstring message = ErrorMessage(err);
        if (error) *error = message;
        return false;
    }
    return true;
}

bool RunProgram(const std::wstring& program, const std::wstring& argumentsTemplate, const std::wstring& query,
                std::wstring* error) {
    std::vector<std::string> argv{Narrow(program)};
    if (!Trim(argumentsTemplate).empty()) {
        gint argc = 0;
        gchar** parsed = nullptr;
        GError* err = nullptr;
        if (!g_shell_parse_argv(Narrow(argumentsTemplate).c_str(), &argc, &parsed, &err)) {
            const std::wstring message = ErrorMessage(err);
            if (error) *error = L"參數格式錯誤：" + message;
            return false;
        }
        for (gint i = 0; i < argc; ++i)
            argv.emplace_back(Narrow(FillTemplate(ExpandUserPath(Utf8ToWide(parsed[i])), query, false)));
        g_strfreev(parsed);
    }
    return SpawnWithActivation(argv, HomeDir(), error);
}

bool LaunchApp(const std::string& desktopId, const std::string& action, std::wstring* error) {
    GDesktopAppInfo* info = g_desktop_app_info_new(desktopId.c_str());
    if (!info) {
        if (error) *error = L"找不到應用程式 " + Utf8ToWide(desktopId);
        return false;
    }
    GAppLaunchContext* ctx = LaunchContext();
    GError* err = nullptr;
    gboolean ok = TRUE;
    if (action.empty()) ok = g_app_info_launch(G_APP_INFO(info), nullptr, ctx, &err);
    else g_desktop_app_info_launch_action(info, action.c_str(), ctx);
    if (ctx) g_object_unref(ctx);
    g_object_unref(info);
    if (!ok) {
        const std::wstring message = ErrorMessage(err);
        if (error) *error = message;
        return false;
    }
    return true;
}

void RevealInFileManager(const std::wstring& path) {
    GFile* file = g_file_new_for_path(Narrow(path).c_str());
    GtkFileLauncher* launcher = gtk_file_launcher_new(file);
    gtk_file_launcher_open_containing_folder(
        launcher, nullptr, nullptr,
        [](GObject* source, GAsyncResult* result, gpointer) {
            gtk_file_launcher_open_containing_folder_finish(GTK_FILE_LAUNCHER(source), result, nullptr);
            g_object_unref(source);
        },
        nullptr);
    g_object_unref(file);
}

bool CopyText(const std::wstring& text) {
    GdkDisplay* display = gdk_display_get_default();
    if (!display) return false;
    gdk_clipboard_set_text(gdk_display_get_clipboard(display), Narrow(text).c_str());
    return true;
}

bool RunInTerminal(const std::wstring& command, bool asRoot, const std::wstring& directory, std::wstring* error) {
    // Newlines, not ";": the command may end with "&" or a "# comment". With sudo, the whole
    // line runs as root ("apt update && apt upgrade"), as with "以系統管理員身分執行" on Windows.
    std::string line = Narrow(command);
    if (asRoot) {
        gchar* quoted = g_shell_quote(line.c_str());
        line = std::string("sudo bash -c ") + quoted;
        g_free(quoted);
    }
    const std::string script = line + "\necho\nexec \"${SHELL:-bash}\"";
    const std::vector<std::string> argv = TerminalArgv({"bash", "-c", script}, Narrow(directory));
    if (argv.empty()) {
        if (error) *error = L"找不到終端機程式（xdg-terminal-exec、ptyxis、gnome-terminal…）";
        return false;
    }
    return SpawnWithActivation(argv, directory.empty() ? HomeDir() : directory, error);
}

bool RunInBackground(const std::wstring& command, std::wstring* error) {
    return SpawnWithActivation({"bash", "-c", Narrow(command)}, HomeDir(), error);
}

bool OpenTerminalAt(const std::wstring& directory, std::wstring* error) {
    const std::vector<std::string> argv = TerminalArgv({}, Narrow(directory));
    if (argv.empty()) {
        if (error) *error = L"找不到終端機程式";
        return false;
    }
    return SpawnWithActivation(argv, directory, error);
}

void Notify(const std::wstring& title, const std::wstring& body, bool warning) {
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!bus) return;
    GVariantBuilder actions;
    g_variant_builder_init(&actions, G_VARIANT_TYPE("as"));
    GVariantBuilder hints;
    g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&hints, "{sv}", "desktop-entry", g_variant_new_string(kAppId));
    g_variant_builder_add(&hints, "{sv}", "urgency", g_variant_new_byte(1));
    g_dbus_connection_call(bus, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                           "org.freedesktop.Notifications", "Notify",
                           g_variant_new("(susssasa{sv}i)", "Beam", 0u, warning ? "dialog-warning" : kAppId,
                                         Narrow(title).c_str(), Narrow(body).c_str(), &actions, &hints, -1),
                           G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
    g_object_unref(bus);
}

bool PrefersDarkTheme() {
    GSettingsSchemaSource* source = g_settings_schema_source_get_default();
    GSettingsSchema* schema = source ? g_settings_schema_source_lookup(source, "org.gnome.desktop.interface", TRUE)
                                     : nullptr;
    if (!schema) return true;  // no GNOME settings: Beam's own default
    bool dark = true;
    if (g_settings_schema_has_key(schema, "color-scheme")) {
        GSettings* settings = g_settings_new("org.gnome.desktop.interface");
        gchar* scheme = g_settings_get_string(settings, "color-scheme");
        dark = scheme && std::strcmp(scheme, "prefer-dark") == 0;
        g_free(scheme);
        g_object_unref(settings);
    }
    g_settings_schema_unref(schema);
    return dark;
}

bool SetAutostart(bool enabled) {
    const std::string dir = std::string(g_get_user_config_dir()) + "/autostart";
    const std::string file = dir + "/" + kAppId + ".desktop";
    if (!enabled) {
        return unlink(file.c_str()) == 0 || errno == ENOENT;
    }
    g_mkdir_with_parents(dir.c_str(), 0700);
    // Desktop-entry Exec quoting: reserved characters need "..." with \" \` \$ \\ escaped, then
    // every backslash is doubled once more (string escape rule) and % becomes %%.
    std::string exe = ExecutablePath();
    std::string arg = exe;
    if (exe.find_first_of(" \t\n\"'\\><~|&;$*?#()`") != std::string::npos) {
        arg = "\"";
        for (const char c : exe) {
            if (c == '"' || c == '`' || c == '$' || c == '\\') arg.push_back('\\');
            arg.push_back(c);
        }
        arg += "\"";
    }
    exe.clear();
    for (const char c : arg) {
        if (c == '\\') exe += "\\\\";
        else if (c == '%') exe += "%%";
        else exe.push_back(c);
    }
    std::ostringstream content;
    content << "[Desktop Entry]\nType=Application\nName=Beam\nComment=Raycast-style launcher\n"
            << "Exec=" << exe << " --background\nIcon=" << kAppId << "\nTerminal=false\nNoDisplay=true\n"
            << "X-GNOME-Autostart-enabled=true\n";
    return g_file_set_contents(file.c_str(), content.str().c_str(), -1, nullptr);
}

void LockScreen() {
    // GNOME Shell's screen shield; loginctl where that is not available.
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!bus) {
        SpawnSimple({"loginctl", "lock-session"});
        return;
    }
    g_dbus_connection_call(
        bus, "org.gnome.ScreenSaver", "/org/gnome/ScreenSaver", "org.gnome.ScreenSaver", "Lock", nullptr, nullptr,
        G_DBUS_CALL_FLAGS_NO_AUTO_START, 3000, nullptr,
        [](GObject* source, GAsyncResult* result, gpointer) {
            GError* error = nullptr;
            GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
            if (reply) g_variant_unref(reply);
            if (error) {
                g_error_free(error);
                SpawnSimple({"loginctl", "lock-session"});
            }
        },
        nullptr);
    g_object_unref(bus);
}
void Suspend() { SpawnSimple({"systemctl", "suspend"}); }

void PowerOff() {
    if (!FindProgram(L"gnome-session-quit").empty()) SpawnSimple({"gnome-session-quit", "--power-off", "--no-prompt"});
    else SpawnSimple({"systemctl", "poweroff"});
}

void Reboot() {
    if (!FindProgram(L"gnome-session-quit").empty()) SpawnSimple({"gnome-session-quit", "--reboot", "--no-prompt"});
    else SpawnSimple({"systemctl", "reboot"});
}

void LogOut() {
    if (!FindProgram(L"gnome-session-quit").empty()) {
        SpawnSimple({"gnome-session-quit", "--logout", "--no-prompt"});
    } else if (const char* session = g_getenv("XDG_SESSION_ID")) {
        SpawnSimple({"loginctl", "terminate-session", session});
    }
}

void EmptyTrash() { SpawnSimple({"gio", "trash", "--empty"}); }

}  // namespace beam
