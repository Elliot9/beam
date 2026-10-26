#include "linux/windows.hpp"

#include <gio/gdesktopappinfo.h>
#include <glib/gstdio.h>
#include <unistd.h>

#include <cstring>
#include <string>

#include "core/json.hpp"
#include "core/text.hpp"

extern const unsigned char kBeamExtensionJs[];
extern const unsigned char kBeamExtensionMetadata[];

namespace beam {
namespace {

constexpr char kUuid[] = "beam-windows@local.beam";
constexpr char kObjectPath[] = "/local/beam/Windows";
constexpr char kInterface[] = "local.beam.Windows";

GDBusConnection* SessionBus() {
    static GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    return bus;
}

GVariant* Call(const char* method, GVariant* args, const GVariantType* reply, int timeoutMs = 600) {
    GDBusConnection* bus = SessionBus();
    if (!bus) return nullptr;
    return g_dbus_connection_call_sync(bus, "org.gnome.Shell", kObjectPath, kInterface, method, args, reply,
                                       G_DBUS_CALL_FLAGS_NO_AUTO_START, timeoutMs, nullptr, nullptr);
}

std::string JsonString(const Json& obj, const char* key) {
    const Json* v = obj.Find(key);
    return v && v->IsString() ? v->AsString() : std::string();
}

GSettings* ShellSettings() {  // nullptr outside GNOME
    GSettingsSchemaSource* source = g_settings_schema_source_get_default();
    GSettingsSchema* schema = source ? g_settings_schema_source_lookup(source, "org.gnome.shell", TRUE) : nullptr;
    if (!schema) return nullptr;
    g_settings_schema_unref(schema);
    return g_settings_new("org.gnome.shell");
}

bool StrvContains(gchar** list, const char* value) {
    for (gchar** p = list; p && *p; ++p)
        if (std::strcmp(*p, value) == 0) return true;
    return false;
}

std::string UserExtensionDir() { return std::string(g_get_user_data_dir()) + "/gnome-shell/extensions/" + kUuid; }

bool ExtensionFilesPresent() {
    if (g_file_test((UserExtensionDir() + "/metadata.json").c_str(), G_FILE_TEST_EXISTS)) return true;
    for (const gchar* const* dir = g_get_system_data_dirs(); dir && *dir; ++dir) {
        const std::string path = std::string(*dir) + "/gnome-shell/extensions/" + kUuid + "/metadata.json";
        if (g_file_test(path.c_str(), G_FILE_TEST_EXISTS)) return true;
    }
    return false;
}

}  // namespace

bool IsGnomeSession() {
    const char* desktop = g_getenv("XDG_CURRENT_DESKTOP");
    return desktop && std::strstr(desktop, "GNOME") != nullptr;
}

GIcon* WindowService::IconFor(const std::string& appId) {
    auto it = icons_.find(appId);
    if (it == icons_.end()) {
        GRef<GIcon> icon;
        if (!appId.empty()) {
            if (GDesktopAppInfo* info = g_desktop_app_info_new(appId.c_str())) {
                icon = GRef<GIcon>::Ref(g_app_info_get_icon(G_APP_INFO(info)));
                g_object_unref(info);
            }
        }
        it = icons_.emplace(appId, icon).first;
    }
    return it->second.get();
}

std::optional<std::vector<WindowInfo>> WindowService::List(bool includeSelf) {
    GVariant* reply = Call("List", nullptr, G_VARIANT_TYPE("(s)"));
    if (!reply) return std::nullopt;
    const gchar* text = nullptr;
    g_variant_get(reply, "(&s)", &text);
    std::vector<WindowInfo> out;
    if (const auto json = Json::Parse(text ? text : "[]"); json && json->IsArray()) {
        const long self = static_cast<long>(getpid());
        for (const auto& w : json->Items()) {
            if (!w.IsObject()) continue;
            const Json* pid = w.Find("pid");
            const long windowPid = pid && pid->IsNumber() ? static_cast<long>(pid->AsNumber()) : 0;
            if (windowPid == self && !includeSelf) continue;  // Beam itself
            WindowInfo info;
            info.pid = windowPid;
            info.id = JsonString(w, "id");
            info.title = Utf8ToWide(JsonString(w, "title"));
            info.appName = Utf8ToWide(JsonString(w, "app"));
            info.appId = JsonString(w, "appId");
            if (info.id.empty() || (Trim(info.title).empty() && windowPid != self)) continue;
            info.icon = GRef<GIcon>::Ref(IconFor(info.appId));
            out.push_back(std::move(info));
        }
    }
    g_variant_unref(reply);
    return out;
}

bool WindowService::Activate(const std::string& id) {
    GVariant* reply = Call("Activate", g_variant_new("(s)", id.c_str()), G_VARIANT_TYPE("(b)"));
    if (!reply) return false;
    gboolean ok = FALSE;
    g_variant_get(reply, "(b)", &ok);
    g_variant_unref(reply);
    return ok;
}

bool WindowService::Close(const std::string& id) {
    GVariant* reply = Call("Close", g_variant_new("(s)", id.c_str()), G_VARIANT_TYPE("(b)"));
    if (!reply) return false;
    gboolean ok = FALSE;
    g_variant_get(reply, "(b)", &ok);
    g_variant_unref(reply);
    return ok;
}

bool WindowService::ActivateOwnWindow() {
    const auto windows = List(true);
    if (!windows) return false;
    const long self = static_cast<long>(getpid());
    for (const auto& w : *windows)
        if (w.pid == self) return Activate(w.id);
    return false;
}

ExtensionState WindowService::State() {
    if (List()) return ExtensionState::Running;
    return StateWhenNotRunning();
}

ExtensionState WindowService::StateWhenNotRunning() {
    GSettings* settings = ShellSettings();
    if (!settings) return ExtensionState::Unsupported;
    gchar** enabled = g_settings_get_strv(settings, "enabled-extensions");
    const bool isEnabled = StrvContains(enabled, kUuid);
    g_strfreev(enabled);
    g_object_unref(settings);
    if (isEnabled && ExtensionFilesPresent()) return ExtensionState::NeedsRelogin;
    return IsGnomeSession() || ExtensionFilesPresent() ? ExtensionState::NotInstalled : ExtensionState::Unsupported;
}

void WindowService::DisableExtension() {
    GSettings* settings = ShellSettings();
    if (!settings) return;
    gchar** enabled = g_settings_get_strv(settings, "enabled-extensions");
    if (StrvContains(enabled, kUuid)) {
        GStrvBuilder* builder = g_strv_builder_new();
        for (gchar** p = enabled; *p; ++p)
            if (std::strcmp(*p, kUuid) != 0) g_strv_builder_add(builder, *p);
        gchar** updated = g_strv_builder_end(builder);
        g_settings_set_strv(settings, "enabled-extensions", updated);
        g_strfreev(updated);
        g_strv_builder_unref(builder);
    }
    g_strfreev(enabled);
    g_settings_sync();
    g_object_unref(settings);
    // A copy Beam put into the user's extension folder (the package's system-wide copy stays).
    const std::string dir = UserExtensionDir();
    for (const char* name : {"extension.js", "metadata.json"}) g_remove((dir + "/" + name).c_str());
    g_rmdir(dir.c_str());
}

bool WindowService::InstallExtension(std::wstring* message) {
    GSettings* settings = ShellSettings();
    if (!settings) {
        if (message) *message = L"這不是 GNOME 桌面，視窗切換需要 GNOME Shell";
        return false;
    }
    bool ok = true;
    if (!ExtensionFilesPresent()) {
        const std::string dir = UserExtensionDir();
        g_mkdir_with_parents(dir.c_str(), 0755);
        ok = g_file_set_contents((dir + "/extension.js").c_str(), reinterpret_cast<const char*>(kBeamExtensionJs), -1,
                                 nullptr) &&
             g_file_set_contents((dir + "/metadata.json").c_str(), reinterpret_cast<const char*>(kBeamExtensionMetadata),
                                 -1, nullptr);
    }
    if (!ok) {
        g_object_unref(settings);
        if (message) *message = L"無法寫入 " + Utf8ToWide(UserExtensionDir());
        return false;
    }
    // enabled-extensions += uuid, disabled-extensions -= uuid
    gchar** enabled = g_settings_get_strv(settings, "enabled-extensions");
    if (!StrvContains(enabled, kUuid)) {
        GStrvBuilder* builder = g_strv_builder_new();
        g_strv_builder_addv(builder, const_cast<const char**>(enabled));
        g_strv_builder_add(builder, kUuid);
        gchar** updated = g_strv_builder_end(builder);
        g_settings_set_strv(settings, "enabled-extensions", updated);
        g_strfreev(updated);
        g_strv_builder_unref(builder);
    }
    g_strfreev(enabled);
    gchar** disabled = g_settings_get_strv(settings, "disabled-extensions");
    if (StrvContains(disabled, kUuid)) {
        GStrvBuilder* builder = g_strv_builder_new();
        for (gchar** p = disabled; *p; ++p)
            if (std::strcmp(*p, kUuid) != 0) g_strv_builder_add(builder, *p);
        gchar** updated = g_strv_builder_end(builder);
        g_settings_set_strv(settings, "disabled-extensions", updated);
        g_strfreev(updated);
        g_strv_builder_unref(builder);
    }
    g_strfreev(disabled);
    const bool userExtensionsOff = g_settings_get_boolean(settings, "disable-user-extensions");
    g_settings_sync();
    g_object_unref(settings);

    // A running shell that already knows the extension (e.g. it was disabled) enables it right away.
    if (GDBusConnection* bus = SessionBus()) {
        // GNOME Shell owns this name in a GNOME session; never start the separate Extensions app.
        GVariant* r = g_dbus_connection_call_sync(bus, "org.gnome.Shell.Extensions", "/org/gnome/Shell/Extensions",
                                                  "org.gnome.Shell.Extensions", "EnableExtension",
                                                  g_variant_new("(s)", kUuid), nullptr, G_DBUS_CALL_FLAGS_NO_AUTO_START,
                                                  2000, nullptr, nullptr);
        if (r) g_variant_unref(r);
    }
    const bool runningNow = [] {
        GVariant* reply = Call("List", nullptr, G_VARIANT_TYPE("(s)"));
        if (reply) g_variant_unref(reply);
        return reply != nullptr;
    }();
    if (message) {
        if (runningNow)
            *message = L"視窗切換已啟用";
        else if (userExtensionsOff)
            *message = L"已安裝，但 GNOME 的「擴充功能」總開關是關閉的：請在「擴充功能」App 打開它";
        else
            *message = L"已安裝視窗切換擴充功能：登出再登入一次後就能使用";
    }
    return true;
}

}  // namespace beam
