// What the launcher list shows on Linux: items, their actions and keyboard
// shortcuts (GDK key values), plus a small GObject reference holder.
#pragma once

#include <gtk/gtk.h>

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "core/json.hpp"
#include "core/text.hpp"

namespace beam {

// Owning reference to a GObject (g_object_ref / g_object_unref).
template <typename T>
class GRef {
public:
    GRef() = default;
    static GRef Take(T* p) {  // adopts an existing reference
        GRef r;
        r.p_ = p;
        return r;
    }
    static GRef Ref(T* p) {  // adds a reference
        GRef r;
        r.p_ = p ? static_cast<T*>(g_object_ref(p)) : nullptr;
        return r;
    }
    GRef(const GRef& o) : p_(o.p_ ? static_cast<T*>(g_object_ref(o.p_)) : nullptr) {}
    GRef(GRef&& o) noexcept : p_(std::exchange(o.p_, nullptr)) {}
    GRef& operator=(GRef o) noexcept {
        std::swap(p_, o.p_);
        return *this;
    }
    ~GRef() {
        if (p_) g_object_unref(p_);
    }
    T* get() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }

private:
    T* p_ = nullptr;
};

inline std::string U8(const std::wstring& s) { return WideToUtf8(s); }
inline std::wstring W(const char* s) { return s ? Utf8ToWide(s) : std::wstring(); }

// Root: search everything. Windows: window switcher. Plugin: one plugin's own
// list. Detail: scrollable text (plugin output, debug log).
enum class Mode { Root, Windows, Plugin, Detail };
enum class Outcome { Hide, Stay };

// Services the launcher exposes to actions.
class LauncherHost {
public:
    virtual ~LauncherHost() = default;
    virtual void SetQuery(const std::wstring& text) = 0;
    virtual void EnterMode(Mode mode) = 0;
    virtual void ShowStatus(const std::wstring& message, bool warning) = 0;
    virtual void RefreshWindows() = 0;
    // Re-runs the current search; selects the item with this usage key if present.
    virtual void Refresh(const std::wstring& selectUsageKey) = 0;
    virtual void EnterPlugin(const std::wstring& commandId, const std::wstring& query) = 0;
    virtual void RunPluginCommand(const std::wstring& commandId, const std::wstring& argument) = 0;
    virtual void PluginCallback(const std::wstring& commandId, const Json& item, const Json& action) = 0;
    virtual void ShowDetail(const std::wstring& title, const std::wstring& text) = 0;
};

struct Shortcut {
    guint keyval = 0;  // lower-case for letters
    bool ctrl = false;
    bool shift = false;
    bool alt = false;

    bool Empty() const { return keyval == 0; }
    bool Matches(guint key, bool c, bool s, bool a) const {
        return keyval != 0 && gdk_keyval_to_lower(key) == keyval && c == ctrl && s == shift && a == alt;
    }
    std::vector<std::wstring> Parts() const;  // {"Ctrl", "Shift", "C"}
};

inline Shortcut Keys(guint keyval, bool ctrl = false, bool shift = false, bool alt = false) {
    return Shortcut{gdk_keyval_to_lower(keyval), ctrl, shift, alt};
}

// Windows virtual-key code (as parsed by core/hotkey) -> GDK key value; 0 if unknown.
guint VkToKeyval(uint32_t vk);
// "Ctrl+Shift+C" (plugin action shortcuts) -> Shortcut; empty when invalid.
Shortcut ParseShortcut(const std::wstring& text);

struct Action {
    std::wstring title;
    const char* icon = "";   // symbolic icon name
    Shortcut shortcut;       // the first action of an item always also runs on plain Enter
    bool dangerous = false;  // needs a second confirmation
    std::function<Outcome(LauncherHost&)> run;
    Shortcut alternate;        // optional second chord (not shown)
    bool recordsUsage = true;  // counts as using the item (ranking, 最近使用)
};

enum class IconType { None, Symbolic, GIcon, File };

struct ItemIcon {
    IconType type = IconType::None;
    std::string name;   // symbolic icon name(s) ("a|b" = fallback), or an image file path
    uint32_t tint = 0;  // tile colour (ARGB) behind symbolic icons
    GRef<GIcon> gicon;  // application / file-type icon
};

inline ItemIcon SymbolicIcon(const char* name, uint32_t tint) { return ItemIcon{IconType::Symbolic, name, tint, {}}; }
inline ItemIcon GIconOf(GIcon* icon) {  // takes a new reference
    ItemIcon i;
    i.type = icon ? IconType::GIcon : IconType::None;
    i.gicon = GRef<GIcon>::Ref(icon);
    return i;
}

struct ResultItem {
    std::wstring title;
    std::wstring subtitle;
    std::wstring accessory;  // right-aligned type label
    std::wstring section;
    std::wstring usageKey;      // empty: not tracked
    std::wstring autocomplete;  // text Tab puts into the search box
    ItemIcon icon;
    std::vector<Action> actions;
    double score = 0;
};

// Symbolic icons (present in Yaru and Adwaita). "a|b": use b when the theme has no a.
namespace icon {
inline constexpr char kSearch[] = "system-search-symbolic";
inline constexpr char kSettings[] = "emblem-system-symbolic";
inline constexpr char kLock[] = "system-lock-screen-symbolic";
inline constexpr char kPower[] = "system-shutdown-symbolic";
inline constexpr char kRestart[] = "system-reboot-symbolic";
inline constexpr char kSleep[] = "weather-clear-night-symbolic";
inline constexpr char kSignOut[] = "system-log-out-symbolic";
inline constexpr char kCalculator[] = "accessories-calculator-symbolic";
inline constexpr char kGlobe[] = "globe-symbolic|web-browser-symbolic";
inline constexpr char kWeb[] = "web-browser-symbolic";
inline constexpr char kLink[] = "insert-link-symbolic";
inline constexpr char kCopy[] = "edit-copy-symbolic";
inline constexpr char kFolder[] = "folder-symbolic";
inline constexpr char kFolderOpen[] = "folder-open-symbolic";
inline constexpr char kTerminal[] = "utilities-terminal-symbolic";
inline constexpr char kClose[] = "window-close-symbolic";
inline constexpr char kRefresh[] = "view-refresh-symbolic";
inline constexpr char kKeyboard[] = "input-keyboard-symbolic";
inline constexpr char kApps[] = "view-app-grid-symbolic";
inline constexpr char kApp[] = "application-x-executable-symbolic";
inline constexpr char kMonitor[] = "utilities-system-monitor-symbolic|text-x-generic-symbolic";
inline constexpr char kEdit[] = "document-edit-symbolic";
inline constexpr char kFile[] = "text-x-generic-symbolic";
inline constexpr char kOpen[] = "document-open-symbolic";
inline constexpr char kCode[] = "applications-engineering-symbolic";
inline constexpr char kHistory[] = "document-open-recent-symbolic";
inline constexpr char kExit[] = "application-exit-symbolic";
inline constexpr char kStar[] = "non-starred-symbolic";
inline constexpr char kStarFill[] = "starred-symbolic";
inline constexpr char kPuzzle[] = "application-x-addon-symbolic";
inline constexpr char kUp[] = "go-up-symbolic";
inline constexpr char kDown[] = "go-down-symbolic";
inline constexpr char kBack[] = "go-previous-symbolic";
inline constexpr char kPlay[] = "media-playback-start-symbolic";
inline constexpr char kWarning[] = "dialog-warning-symbolic";
inline constexpr char kInfo[] = "dialog-information-symbolic";
inline constexpr char kAdd[] = "list-add-symbolic";
inline constexpr char kList[] = "view-list-symbolic";
inline constexpr char kTrash[] = "user-trash-symbolic";
inline constexpr char kTrashFull[] = "user-trash-full-symbolic";
inline constexpr char kWindows[] = "view-grid-symbolic";
inline constexpr char kRun[] = "system-run-symbolic";
inline constexpr char kAdmin[] = "dialog-password-symbolic";
}  // namespace icon

// Tile colours (ARGB) for symbolic icons, as on Windows.
namespace tint {
inline constexpr uint32_t kBeam = 0xFFFF5A5F;
inline constexpr uint32_t kSystem = 0xFF64748B;
inline constexpr uint32_t kSettings = 0xFF6366F1;
inline constexpr uint32_t kCalc = 0xFFF59E0B;
inline constexpr uint32_t kWeb = 0xFF3B82F6;
inline constexpr uint32_t kQuicklink = 0xFF8B5CF6;
inline constexpr uint32_t kShell = 0xFF10B981;
inline constexpr uint32_t kWindow = 0xFF0EA5E9;
inline constexpr uint32_t kDanger = 0xFFEF4444;
inline constexpr uint32_t kPlugin = 0xFF14B8A6;
inline constexpr uint32_t kFavorite = 0xFFEAB308;
}  // namespace tint

}  // namespace beam
