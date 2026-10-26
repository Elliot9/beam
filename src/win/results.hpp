// What the launcher list shows: items, their actions and keyboard shortcuts.
#pragma once

#include "win/common.hpp"

#include <functional>
#include <string>
#include <vector>

#include "core/json.hpp"

namespace beam {

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
    virtual HWND Window() const = 0;
};

struct Shortcut {
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    UINT vk = 0;

    bool Empty() const { return vk == 0; }
    bool Matches(UINT key, bool c, bool s, bool a) const {
        return vk != 0 && key == vk && c == ctrl && s == shift && a == alt;
    }
    std::vector<std::wstring> Parts() const;
};

inline Shortcut Keys(UINT vk, bool ctrl = false, bool shift = false, bool alt = false) {
    return Shortcut{ctrl, shift, alt, vk};
}

struct Action {
    std::wstring title;
    std::wstring glyph;      // Segoe MDL2 Assets code point
    Shortcut shortcut;       // the first action of an item always also runs on plain Enter
    bool dangerous = false;  // needs a second confirmation
    std::function<Outcome(LauncherHost&)> run;
    Shortcut alternate;        // optional second chord (not shown)
    bool recordsUsage = true;  // counts as using the item (ranking, 最近使用)
};

enum class IconType { None, Glyph, Shell };

struct ItemIcon {
    IconType type = IconType::None;
    std::wstring value;  // glyph text, or a shell path for IconCache
    uint32_t tint = 0;   // tile colour for glyph icons
};

struct ResultItem {
    std::wstring title;
    std::wstring subtitle;
    std::wstring accessory;  // right-aligned type label
    std::wstring section;
    std::wstring usageKey;   // empty: not tracked
    std::wstring autocomplete;  // text Tab puts into the search box
    ItemIcon icon;
    std::vector<Action> actions;
    double score = 0;
};

// Segoe MDL2 Assets glyphs used for built-in icons.
namespace glyph {
inline constexpr wchar_t kSearch[] = L"";
inline constexpr wchar_t kSettings[] = L"";
inline constexpr wchar_t kLock[] = L"";
inline constexpr wchar_t kPower[] = L"";
inline constexpr wchar_t kRestart[] = L"";
inline constexpr wchar_t kSleep[] = L"";
inline constexpr wchar_t kCalculator[] = L"";
inline constexpr wchar_t kGlobe[] = L"";
inline constexpr wchar_t kLink[] = L"";
inline constexpr wchar_t kCopy[] = L"";
inline constexpr wchar_t kFolder[] = L"";
inline constexpr wchar_t kFolderOpen[] = L"";
inline constexpr wchar_t kAdmin[] = L"";
inline constexpr wchar_t kDelete[] = L"";
inline constexpr wchar_t kTaskView[] = L"";
inline constexpr wchar_t kTerminal[] = L"";
inline constexpr wchar_t kClose[] = L"";
inline constexpr wchar_t kRefresh[] = L"";
inline constexpr wchar_t kKeyboard[] = L"";
inline constexpr wchar_t kApps[] = L"";
inline constexpr wchar_t kDiagnostic[] = L"";
inline constexpr wchar_t kEdit[] = L"";
inline constexpr wchar_t kSignOut[] = L"";
inline constexpr wchar_t kOpen[] = L"";
inline constexpr wchar_t kCode[] = L"";
inline constexpr wchar_t kNetwork[] = L"";
inline constexpr wchar_t kDevices[] = L"";
inline constexpr wchar_t kFile[] = L"";
inline constexpr wchar_t kHistory[] = L"";
inline constexpr wchar_t kExit[] = L"";
inline constexpr wchar_t kStar[] = L"\xE734";
inline constexpr wchar_t kStarFill[] = L"\xE735";
inline constexpr wchar_t kPuzzle[] = L"\xEA86";
inline constexpr wchar_t kUp[] = L"\xE70E";
inline constexpr wchar_t kDown[] = L"\xE70D";
inline constexpr wchar_t kBack[] = L"\xE72B";
inline constexpr wchar_t kPlay[] = L"\xE768";
inline constexpr wchar_t kWarning[] = L"\xE7BA";
inline constexpr wchar_t kAdd[] = L"\xE710";
inline constexpr wchar_t kList[] = L"\xE8FD";
}  // namespace glyph

// Tile colours (ARGB) for glyph icons.
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
