#include "linux/hotkey.hpp"

#include <gio/gdesktopappinfo.h>
#include <gtk/gtk.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core/hotkey.hpp"
#include "core/text.hpp"
#include "linux/results.hpp"

namespace beam {
namespace {

constexpr char kMediaKeys[] = "org.gnome.settings-daemon.plugins.media-keys";
constexpr char kCustomBinding[] = "org.gnome.settings-daemon.plugins.media-keys.custom-keybinding";
constexpr char kLauncherPath[] = "/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings/beam-launcher/";
constexpr char kWindowsPath[] = "/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings/beam-windows/";
constexpr char kBeamPathPrefix[] = "/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings/beam-";

constexpr GdkModifierType kModMask =
    static_cast<GdkModifierType>(GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_ALT_MASK | GDK_SUPER_MASK | GDK_META_MASK |
                                 GDK_HYPER_MASK);

GSettingsSchema* LookupSchema(const char* id) {
    GSettingsSchemaSource* source = g_settings_schema_source_get_default();
    return source ? g_settings_schema_source_lookup(source, id, TRUE) : nullptr;
}

bool HasSchema(const char* id) {
    GSettingsSchema* schema = LookupSchema(id);
    if (schema) g_settings_schema_unref(schema);
    return schema != nullptr;
}

bool SameAccelerator(const char* a, guint keyval, GdkModifierType mods) {
    guint k = 0;
    GdkModifierType m{};
    if (!a || !*a || !gtk_accelerator_parse(a, &k, &m)) return false;
    return gdk_keyval_to_lower(k) == keyval && (m & kModMask) == mods;
}

std::string Quote(const std::string& path) {
    gchar* q = g_shell_quote(path.c_str());
    std::string out = path.find_first_of(" \t'\"\\$`") == std::string::npos ? path : std::string(q);
    g_free(q);
    return out;
}

// Whether one custom shortcut already has these values.
bool BindingIs(const char* path, const char* name, const std::string& command, const std::string& binding) {
    GSettings* kb = g_settings_new_with_path(kCustomBinding, path);
    bool same = true;
    for (const auto& [key, value] : {std::pair<const char*, std::string>{"name", name}, {"command", command}, {"binding", binding}}) {
        gchar* current = g_settings_get_string(kb, key);
        same = same && current && value == current;
        g_free(current);
    }
    g_object_unref(kb);
    return same;
}

// Points one custom shortcut at Beam.
void WriteBinding(const char* path, const char* name, const std::string& command, const std::string& binding) {
    GSettings* kb = g_settings_new_with_path(kCustomBinding, path);
    g_settings_set_string(kb, "name", name);
    g_settings_set_string(kb, "command", command.c_str());
    g_settings_set_string(kb, "binding", binding.c_str());
    g_object_unref(kb);
}

void ResetBinding(const char* path) {
    GSettings* kb = g_settings_new_with_path(kCustomBinding, path);
    for (const char* key : {"name", "command", "binding"}) g_settings_reset(kb, key);
    g_object_unref(kb);
}

// The string-list keys of `schema` holding an accelerator on these keys.
std::vector<std::string> KeysBoundTo(GSettingsSchema* schema, guint keyval, GdkModifierType mods) {
    std::vector<std::string> hits;
    if (!g_settings_schema_get_path(schema)) return hits;  // relocatable: no settings of its own
    GSettings* settings = g_settings_new_full(schema, nullptr, nullptr);
    gchar** keys = g_settings_schema_list_keys(schema);
    for (gchar** key = keys; key && *key; ++key) {
        GSettingsSchemaKey* info = g_settings_schema_get_key(schema, *key);
        const bool isList = g_variant_type_equal(g_settings_schema_key_get_value_type(info), G_VARIANT_TYPE_STRING_ARRAY);
        g_settings_schema_key_unref(info);
        if (!isList || std::strcmp(*key, "custom-keybindings") == 0) continue;
        gchar** values = g_settings_get_strv(settings, *key);
        bool hit = false;
        for (gchar** v = values; v && *v && !hit; ++v) hit = SameAccelerator(*v, keyval, mods);
        g_strfreev(values);
        if (hit) hits.emplace_back(*key);
    }
    g_strfreev(keys);
    g_object_unref(settings);
    return hits;
}

constexpr char kExtensionPrefix[] = "org.gnome.shell.extensions.";

// One installed GNOME Shell extension: its name and the schema it declares (metadata.json).
struct ExtensionInfo {
    std::string name;
    std::string schema;  // "settings-schema", may be empty
};

std::string JsonString(const std::string& json, const char* key) {
    // Good enough for metadata.json: "key": "value" with no escaped quotes in the value.
    const std::string needle = std::string("\"") + key + "\"";
    size_t at = json.find(needle);
    if (at == std::string::npos) return "";
    at = json.find(':', at + needle.size());
    if (at == std::string::npos) return "";
    const size_t open = json.find('"', at + 1);
    if (open == std::string::npos) return "";
    const size_t close = json.find('"', open + 1);
    return close == std::string::npos ? "" : json.substr(open + 1, close - open - 1);
}

// Keybindings of GNOME Shell extensions: their schemas are compiled into the system schemas, or
// sit in the extension's own "schemas" folder (any schema id there belongs to the extension).
void FindExtensionConflicts(guint keyval, GdkModifierType mods, std::vector<HotkeyConflict>& out) {
    std::set<std::string> seen;
    auto report = [&](GSettingsSchema* schema, const std::string& id, const std::string& extension) {
        for (const std::string& key : KeysBoundTo(schema, keyval, mods)) {
            if (!seen.insert(id + "/" + key).second) continue;
            const std::string name = !extension.empty() ? extension
                                     : g_str_has_prefix(id.c_str(), kExtensionPrefix) ? id.substr(std::strlen(kExtensionPrefix))
                                                                                      : id;
            out.push_back({false, L"GNOME 擴充功能「" + Utf8ToWide(name) + L"」的「" + Utf8ToWide(key) + L"」快捷鍵"});
        }
    };
    std::vector<std::string> roots{std::string(g_get_user_data_dir()) + "/gnome-shell/extensions"};
    for (const gchar* const* dir = g_get_system_data_dirs(); dir && *dir; ++dir)
        roots.push_back(std::string(*dir) + "/gnome-shell/extensions");
    std::map<std::string, std::string> schemaOwners;  // schema id -> extension name (metadata.json)
    for (const std::string& root : roots) {
        GDir* dir = g_dir_open(root.c_str(), 0, nullptr);
        if (!dir) continue;
        while (const gchar* uuid = g_dir_read_name(dir)) {
            const std::string folder = root + "/" + uuid;
            gchar* metadata = nullptr;
            std::string name = uuid;
            if (g_file_get_contents((folder + "/metadata.json").c_str(), &metadata, nullptr, nullptr)) {
                const std::string json = metadata;
                if (const std::string n = JsonString(json, "name"); !n.empty()) name = n;
                if (const std::string id = JsonString(json, "settings-schema"); !id.empty()) schemaOwners.emplace(id, name);
                g_free(metadata);
            }
            const std::string schemas = folder + "/schemas";
            if (!g_file_test((schemas + "/gschemas.compiled").c_str(), G_FILE_TEST_IS_REGULAR)) continue;
            GSettingsSchemaSource* source = g_settings_schema_source_new_from_directory(schemas.c_str(), nullptr, FALSE, nullptr);
            if (!source) continue;
            gchar** ids = nullptr;
            g_settings_schema_source_list_schemas(source, FALSE, &ids, nullptr);
            for (gchar** id = ids; id && *id; ++id) {
                if (GSettingsSchema* schema = g_settings_schema_source_lookup(source, *id, FALSE)) {
                    report(schema, *id, name);
                    g_settings_schema_unref(schema);
                }
            }
            g_strfreev(ids);
            g_settings_schema_source_unref(source);
        }
        g_dir_close(dir);
    }
    // Extensions installed system-wide usually compile their schemas into the system schemas.
    if (GSettingsSchemaSource* source = g_settings_schema_source_get_default()) {
        gchar** ids = nullptr;
        g_settings_schema_source_list_schemas(source, TRUE, &ids, nullptr);
        for (gchar** id = ids; id && *id; ++id) {
            const auto owner = schemaOwners.find(*id);
            if (owner == schemaOwners.end() && !g_str_has_prefix(*id, kExtensionPrefix)) continue;
            if (GSettingsSchema* schema = g_settings_schema_source_lookup(source, *id, TRUE)) {
                report(schema, *id, owner != schemaOwners.end() ? owner->second : std::string());
                g_settings_schema_unref(schema);
            }
        }
        g_strfreev(ids);
    }
}

// Shortcuts apps took through the global-shortcuts portal (approved in a GNOME dialog; listed
// under Settings → Apps). GNOME Shell holds them while the app runs.
void FindAppShortcutConflicts(guint keyval, GdkModifierType mods, std::vector<HotkeyConflict>& out) {
    constexpr char kGlobal[] = "org.gnome.settings-daemon.global-shortcuts";
    constexpr char kGlobalApp[] = "org.gnome.settings-daemon.global-shortcuts.application";
    if (!HasSchema(kGlobal) || !HasSchema(kGlobalApp)) return;
    GSettings* global = g_settings_new(kGlobal);
    gchar** apps = g_settings_get_strv(global, "applications");
    for (gchar** app = apps; app && *app; ++app) {
        const std::string path = std::string("/org/gnome/settings-daemon/global-shortcuts/") + *app + "/";
        GSettings* settings = g_settings_new_with_path(kGlobalApp, path.c_str());
        GVariant* shortcuts = g_settings_get_value(settings, "shortcuts");  // a(sa{sv})
        GVariantIter iter;
        g_variant_iter_init(&iter, shortcuts);
        const gchar* id = nullptr;
        GVariant* options = nullptr;
        while (g_variant_iter_next(&iter, "(&s@a{sv})", &id, &options)) {
            const gchar* description = nullptr;
            g_variant_lookup(options, "description", "&s", &description);
            GVariant* accels = g_variant_lookup_value(options, "shortcuts", G_VARIANT_TYPE_STRING_ARRAY);
            bool hit = false;
            if (accels) {
                const gchar** list = g_variant_get_strv(accels, nullptr);
                for (const gchar** a = list; a && *a && !hit; ++a) hit = SameAccelerator(*a, keyval, mods);
                g_free(list);
                g_variant_unref(accels);
            }
            if (hit) {
                std::string appName = *app;
                if (GDesktopAppInfo* info = g_desktop_app_info_new((std::string(*app) + ".desktop").c_str())) {
                    appName = g_app_info_get_display_name(G_APP_INFO(info));
                    g_object_unref(info);
                }
                out.push_back({false, L"App「" + Utf8ToWide(appName) + L"」的全域快捷鍵「" +
                                          Utf8ToWide(description && *description ? description : id) + L"」"});
            }
            g_variant_unref(options);
        }
        g_variant_unref(shortcuts);
        g_object_unref(settings);
    }
    g_strfreev(apps);
    g_object_unref(global);
}

// Frees the window menu's Alt+Space (Beam's default hotkey) and reports every other shortcut on
// the same keys: GNOME grabs a key combination only once, so only one of them would work.
void ResolveConflicts(const std::string& accelerator, const std::wstring& hotkey, HotkeySetup& setup) {
    guint keyval = 0;
    GdkModifierType mods{};
    if (!gtk_accelerator_parse(accelerator.c_str(), &keyval, &mods)) return;
    keyval = gdk_keyval_to_lower(keyval);
    mods = static_cast<GdkModifierType>(mods & kModMask);
    for (const HotkeyConflict& conflict : FindHotkeyConflicts(accelerator)) {
        if (!conflict.windowMenu) {
            if (!setup.problem.empty()) setup.problem += L"\n";
            const bool latin = !conflict.what.empty() && conflict.what[0] < 0x80;
            setup.problem += L"「" + hotkey + L"」也被" + (latin ? L" " : L"") + conflict.what + L"使用";
            continue;
        }
        GSettings* wm = g_settings_new("org.gnome.desktop.wm.keybindings");
        gchar** values = g_settings_get_strv(wm, "activate-window-menu");
        GStrvBuilder* builder = g_strv_builder_new();
        for (gchar** v = values; v && *v; ++v)
            if (!SameAccelerator(*v, keyval, mods)) g_strv_builder_add(builder, *v);
        gchar** kept = g_strv_builder_end(builder);
        g_settings_set_strv(wm, "activate-window-menu", kept);
        g_strfreev(kept);
        g_strv_builder_unref(builder);
        g_strfreev(values);
        g_object_unref(wm);
        setup.changed = true;
        setup.note = L"GNOME 原本用這組按鍵開啟「視窗選單」，已改給 Beam 使用";
    }
}

}  // namespace

bool GnomeShortcutsSupported() { return HasSchema(kMediaKeys) && HasSchema(kCustomBinding); }

std::string BeamShortcutCommand(const std::string& executable, bool windowSwitcher) {
    return Quote(executable) + (windowSwitcher ? " --windows" : " --toggle");
}

GnomeShortcut ReadBeamShortcut(bool windowSwitcher) {
    GnomeShortcut out;
    if (!GnomeShortcutsSupported()) return out;
    const char* path = windowSwitcher ? kWindowsPath : kLauncherPath;
    GSettings* media = g_settings_new(kMediaKeys);
    gchar** paths = g_settings_get_strv(media, "custom-keybindings");
    for (gchar** p = paths; p && *p; ++p) out.listed = out.listed || std::strcmp(*p, path) == 0;
    g_strfreev(paths);
    g_object_unref(media);
    GSettings* kb = g_settings_new_with_path(kCustomBinding, path);
    for (auto [key, value] : {std::pair<const char*, std::string*>{"name", &out.name}, {"command", &out.command},
                              {"binding", &out.binding}}) {
        gchar* text = g_settings_get_string(kb, key);
        *value = text ? text : "";
        g_free(text);
    }
    g_object_unref(kb);
    return out;
}

std::vector<HotkeyConflict> FindHotkeyConflicts(const std::string& accelerator) {
    std::vector<HotkeyConflict> out;
    guint keyval = 0;
    GdkModifierType mods{};
    if (accelerator.empty() || !gtk_accelerator_parse(accelerator.c_str(), &keyval, &mods)) return out;
    keyval = gdk_keyval_to_lower(keyval);
    mods = static_cast<GdkModifierType>(mods & kModMask);

    // GNOME's own shortcuts
    static const char* kSchemas[] = {"org.gnome.desktop.wm.keybindings", "org.gnome.shell.keybindings",
                                     "org.gnome.mutter.keybindings", "org.gnome.mutter.wayland.keybindings", kMediaKeys};
    for (const char* id : kSchemas) {
        GSettingsSchema* schema = LookupSchema(id);
        if (!schema) continue;
        for (const std::string& key : KeysBoundTo(schema, keyval, mods)) {
            const bool menu = std::strcmp(id, "org.gnome.desktop.wm.keybindings") == 0 && key == "activate-window-menu";
            out.push_back({menu, L"GNOME 的「" + Utf8ToWide(key) + L"」快捷鍵"});
        }
        g_settings_schema_unref(schema);
    }

    // other custom shortcuts (Settings → Keyboard → Custom Shortcuts)
    if (GnomeShortcutsSupported()) {
        GSettings* media = g_settings_new(kMediaKeys);
        gchar** paths = g_settings_get_strv(media, "custom-keybindings");
        for (gchar** p = paths; p && *p; ++p) {
            if (std::strcmp(*p, kLauncherPath) == 0 || std::strcmp(*p, kWindowsPath) == 0) continue;
            GSettings* kb = g_settings_new_with_path(kCustomBinding, *p);
            gchar* binding = g_settings_get_string(kb, "binding");
            if (SameAccelerator(binding, keyval, mods)) {
                gchar* name = g_settings_get_string(kb, "name");
                out.push_back({false, L"自訂快捷鍵「" + Utf8ToWide(name && *name ? name : *p) + L"」"});
                g_free(name);
            }
            g_free(binding);
            g_object_unref(kb);
        }
        g_strfreev(paths);
        g_object_unref(media);
    }

    FindExtensionConflicts(keyval, mods, out);
    FindAppShortcutConflicts(keyval, mods, out);
    return out;
}

std::string GnomeAccelerator(const std::wstring& hotkey) {
    const auto parsed = ParseHotkey(hotkey);
    if (!parsed) return "";
    const guint keyval = VkToKeyval(parsed->vk);
    if (!keyval) return "";
    int mods = 0;
    if (parsed->mods & kModControl) mods |= GDK_CONTROL_MASK;
    if (parsed->mods & kModAlt) mods |= GDK_ALT_MASK;
    if (parsed->mods & kModShift) mods |= GDK_SHIFT_MASK;
    if (parsed->mods & kModWin) mods |= GDK_SUPER_MASK;
    gchar* name = gtk_accelerator_name(keyval, static_cast<GdkModifierType>(mods));
    std::string out = name ? name : "";
    g_free(name);
    return out;
}

HotkeySetup ReleaseHotkeyConflicts(const std::wstring& launcherHotkey, const std::wstring& windowsHotkey) {
    HotkeySetup setup;
    if (!GnomeShortcutsSupported()) return setup;
    setup.supported = true;

    const std::string launcherAccel = GnomeAccelerator(launcherHotkey);
    const std::string windowsAccel = Trim(windowsHotkey).empty() ? std::string() : GnomeAccelerator(windowsHotkey);
    if (launcherAccel.empty()) {
        setup.problem = L"無法解析快捷鍵「" + launcherHotkey + L"」";
        return setup;
    }
    if (!Trim(windowsHotkey).empty() && windowsAccel.empty())
        setup.problem = L"無法解析快捷鍵「" + windowsHotkey + L"」";

    const std::wstring problemBefore = setup.problem;
    ResolveConflicts(launcherAccel, Trim(launcherHotkey), setup);
    if (!windowsAccel.empty()) ResolveConflicts(windowsAccel, Trim(windowsHotkey), setup);
    if (setup.problem != problemBefore)
        setup.problem += L"\n同一組按鍵只有一個會生效：請在 settings.json 換一組，或改掉另一邊的快捷鍵";
    g_settings_sync();
    return setup;
}

ShortcutWrite WriteGnomeShortcuts(const std::wstring& launcherHotkey, const std::wstring& windowsHotkey,
                                  const std::string& executable, bool regrab) {
    ShortcutWrite result;
    const std::string launcherAccel = GnomeAccelerator(launcherHotkey);
    const std::string windowsAccel = Trim(windowsHotkey).empty() ? std::string() : GnomeAccelerator(windowsHotkey);
    if (!GnomeShortcutsSupported() || launcherAccel.empty()) return result;

    // gsd-media-keys grabs a listed shortcut again on every change of its settings, and a quick
    // series of changes can leave it without a grab. So a shortcut that has to change is taken
    // out of the list, written completely, and listed again: one clean grab. Taking a shortcut
    // out and listing it again is also the only way to make gsd-media-keys grab it once more.
    struct Wanted {
        const char* path;
        const char* name;
        std::string command;
        std::string accel;
    };
    std::vector<Wanted> wanted{{kLauncherPath, "Beam", BeamShortcutCommand(executable, false), launcherAccel}};
    if (!windowsAccel.empty())
        wanted.push_back({kWindowsPath, "Beam：切換視窗", BeamShortcutCommand(executable, true), windowsAccel});

    GSettings* media = g_settings_new(kMediaKeys);
    std::vector<std::string> listed;
    {
        gchar** paths = g_settings_get_strv(media, "custom-keybindings");
        for (gchar** p = paths; p && *p; ++p) listed.emplace_back(*p);
        g_strfreev(paths);
    }
    auto isListed = [&listed](const char* path) { return std::find(listed.begin(), listed.end(), path) != listed.end(); };
    auto setList = [media](const std::vector<std::string>& list) {
        std::vector<const char*> strv;
        for (const auto& p : list) strv.push_back(p.c_str());
        strv.push_back(nullptr);
        g_settings_set_strv(media, "custom-keybindings", strv.data());
        g_settings_sync();
    };
    std::vector<const Wanted*> stale;  // missing, or with other values
    std::vector<const Wanted*> relist;  // stale ones, and (regrab) the rest
    for (const auto& w : wanted) {
        const bool current = isListed(w.path) && BindingIs(w.path, w.name, w.command, w.accel);
        if (!current) stale.push_back(&w);
        if (!current || regrab) relist.push_back(&w);
    }
    const bool dropWindows = windowsAccel.empty() && isListed(kWindowsPath);

    std::vector<std::string> list;
    for (const auto& p : listed) {
        const bool unlist = (dropWindows && p == kWindowsPath) ||
                            std::any_of(relist.begin(), relist.end(), [&p](const Wanted* w) { return p == w->path; });
        if (!unlist) list.push_back(p);
    }
    if (list != listed) setList(list);
    for (const Wanted* w : stale) WriteBinding(w->path, w->name, w->command, w->accel);
    if (dropWindows) ResetBinding(kWindowsPath);
    g_settings_sync();
    const size_t before = list.size();
    for (const Wanted* w : relist) list.emplace_back(w->path);
    if (list.size() != before) setList(list);
    g_object_unref(media);
    g_settings_sync();
    result.changed = !stale.empty() || dropWindows;
    result.asked = !relist.empty();
    return result;
}

void UnregisterGnomeShortcuts() {
    if (GnomeShortcutsSupported()) {
        GSettings* media = g_settings_new(kMediaKeys);
        gchar** paths = g_settings_get_strv(media, "custom-keybindings");
        GStrvBuilder* builder = g_strv_builder_new();
        for (gchar** p = paths; p && *p; ++p)
            if (std::strcmp(*p, kLauncherPath) != 0 && std::strcmp(*p, kWindowsPath) != 0) g_strv_builder_add(builder, *p);
        gchar** kept = g_strv_builder_end(builder);
        g_settings_set_strv(media, "custom-keybindings", kept);
        g_strfreev(kept);
        g_strv_builder_unref(builder);
        g_strfreev(paths);
        g_object_unref(media);
        ResetBinding(kLauncherPath);
        ResetBinding(kWindowsPath);
    }
    // Beam emptied the window-menu shortcut when it took Alt+Space: restore GNOME's default.
    if (HasSchema("org.gnome.desktop.wm.keybindings")) {
        GSettings* wm = g_settings_new("org.gnome.desktop.wm.keybindings");
        gchar** menu = g_settings_get_strv(wm, "activate-window-menu");
        if (!menu || !menu[0]) g_settings_reset(wm, "activate-window-menu");
        g_strfreev(menu);
        g_object_unref(wm);
    }
    g_settings_sync();
}

long MediaKeysPid() {
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!bus) return 0;
    GVariant* reply = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                                  "org.freedesktop.DBus", "GetConnectionUnixProcessID",
                                                  g_variant_new("(s)", "org.gnome.SettingsDaemon.MediaKeys"),
                                                  G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr);
    guint32 pid = 0;
    if (reply) {
        g_variant_get(reply, "(u)", &pid);
        g_variant_unref(reply);
    }
    g_object_unref(bus);
    return static_cast<long>(pid);
}

namespace {

// journalctl -b _PID=<gsd-media-keys>: its messages of this boot, "2026-10-16T13:05:12+08:00 host
// gsd-media-keys[1234]: Failed to grab accelerator for keybinding custom:/org/.../beam-launcher/".
std::vector<std::string> JournalArguments(long gsdPid) {
    return {"journalctl", "-b", "--no-pager", "-q", "-o", "short-iso", "_PID=" + std::to_string(gsdPid)};
}

int CountFailures(const std::string& text, std::string* lastTime) {
    const std::string needle = std::string("Failed to grab accelerator for keybinding custom:") + kBeamPathPrefix;
    int count = 0;
    size_t last = std::string::npos;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
        ++count;
        last = at;
    }
    if (lastTime && last != std::string::npos) {
        const size_t start = text.rfind('\n', last);
        const std::string line = text.substr(start == std::string::npos ? 0 : start + 1, 16);
        *lastTime = line.size() == 16 && line[10] == 'T' ? line.substr(11, 5) : std::string("?");
    }
    return count;
}

}  // namespace

int CountShortcutGrabFailures(long gsdPid, std::string* lastTime) {
    if (gsdPid <= 0) return -1;
    const std::vector<std::string> args = JournalArguments(gsdPid);
    std::vector<gchar*> argv;
    for (const auto& a : args) argv.push_back(const_cast<gchar*>(a.c_str()));
    argv.push_back(nullptr);
    gchar* out = nullptr;
    gint status = 0;
    if (!g_spawn_sync(nullptr, argv.data(), nullptr, static_cast<GSpawnFlags>(G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL),
                      nullptr, nullptr, &out, nullptr, &status, nullptr))
        return -1;
    const int count = CountFailures(out ? out : "", lastTime);
    g_free(out);
    return count;
}

void CountShortcutGrabFailuresAsync(long gsdPid, std::function<void(int)> done) {
    if (gsdPid <= 0) {
        done(-1);
        return;
    }
    const std::vector<std::string> args = JournalArguments(gsdPid);
    std::vector<const gchar*> argv;
    for (const auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    GSubprocess* process = g_subprocess_newv(argv.data(),
                                             static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                                                           G_SUBPROCESS_FLAGS_STDERR_SILENCE),
                                             nullptr);
    if (!process) {
        done(-1);
        return;
    }
    g_subprocess_communicate_async(
        process, nullptr, nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
            std::unique_ptr<std::function<void(int)>> callback(static_cast<std::function<void(int)>*>(data));
            GBytes* out = nullptr;
            int count = -1;
            if (g_subprocess_communicate_finish(G_SUBPROCESS(source), result, &out, nullptr, nullptr)) {
                gsize size = 0;
                const auto* bytes = static_cast<const char*>(out ? g_bytes_get_data(out, &size) : nullptr);
                count = CountFailures(bytes ? std::string(bytes, size) : std::string(), nullptr);
            }
            if (out) g_bytes_unref(out);
            g_object_unref(source);
            (*callback)(count);
        },
        new std::function<void(int)>(std::move(done)));
}

std::string KeyOwnerProbe(const std::wstring& hotkey) {
    const std::string accel = GnomeAccelerator(hotkey);
    guint keyval = 0;
    GdkModifierType mods{};
    GdkDisplay* display = gdk_display_get_default();
    if (accel.empty() || !display || !gtk_accelerator_parse(accel.c_str(), &keyval, &mods)) return "";
    GdkKeymapKey* keys = nullptr;
    int count = 0;
    if (!gdk_display_map_keyval(display, keyval, &keys, &count) || count == 0) return "";
    guint keycode = keys[0].keycode;
    for (int i = 0; i < count; ++i)
        if (keys[i].group == 0 && keys[i].level == 0) {
            keycode = keys[i].keycode;
            break;
        }
    g_free(keys);
    // GNOME Shell takes the modifiers as X11 bits: Shift 1, Control 4, Alt (Mod1) 8, Super (Mod4) 64.
    unsigned mask = 0;
    if (mods & GDK_SHIFT_MASK) mask |= 1;
    if (mods & GDK_CONTROL_MASK) mask |= 4;
    if (mods & GDK_ALT_MASK) mask |= 8;
    if (mods & GDK_SUPER_MASK) mask |= 64;
    std::string label;
    for (const char c : WideToUtf8(Trim(hotkey)))
        if (c != '\'' && c != '\\') label += c;
    std::string js = R"JS((() => { const k = '@LABEL@'; const a = global.display.get_keybinding_action(@KEYCODE@, @MASK@); if (!a) return k + '：沒有程式在用'; const b = Object.keys(Meta.KeyBindingAction).find(n => Meta.KeyBindingAction[n] === a); if (b) return k + '：GNOME 內建的 ' + b; const s = Main.shellDBusService._grabbedAccelerators.get(a); if (s) { const [pid] = Gio.DBus.session.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus', 'GetConnectionUnixProcessID', new GLib.Variant('(s)', [s]), null, 0, -1, null).deepUnpack(); const cmd = new TextDecoder().decode(GLib.file_get_contents('/proc/' + pid + '/cmdline')[1]).split(String.fromCharCode(0)).join(' ').trim(); const norm = x => (x.toLowerCase().match(/<[^>]+>/g) || []).map(t => ({'<mod1>': '<alt>', '<primary>': '<control>', '<ctrl>': '<control>', '<mod4>': '<super>'})[t] || t).sort().join('') + x.replace(/<[^>]+>/g, '').toLowerCase(); let apps = ''; try { const g = new Gio.Settings({schema_id: 'org.gnome.settings-daemon.global-shortcuts'}); apps = g.get_strv('applications').filter(app => Gio.Settings.new_with_path('org.gnome.settings-daemon.global-shortcuts.application', '/org/gnome/settings-daemon/global-shortcuts/' + app + '/').get_value('shortcuts').recursiveUnpack().some(([, o]) => (o.shortcuts || []).some(x => norm(x) === norm('@ACCEL@')))).join(', '); } catch (e) {} return k + '：程式 ' + cmd + ' [pid ' + pid + ']' + (apps ? '，App：' + apps : ''); } const n = Object.keys(Main.wm._allowedKeybindings).find(x => { try { return Meta.prefs_get_keybinding_action(x) === a; } catch (e) { return String(e).includes(a + ' is not'); } }); return k + '：快捷鍵 ' + (n || '#' + a); })())JS";
    auto put = [&js](const std::string& from, const std::string& to) {
        for (size_t at = js.find(from); at != std::string::npos; at = js.find(from, at + to.size())) js.replace(at, from.size(), to);
    };
    put("@LABEL@", label);
    put("@KEYCODE@", std::to_string(keycode));
    put("@MASK@", std::to_string(mask));
    put("@ACCEL@", accel);
    return js;
}

}  // namespace beam
