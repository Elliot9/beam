// Hotkey text <-> (modifiers, virtual-key) used by RegisterHotKey.
// Modifier bits intentionally equal MOD_ALT / MOD_CONTROL / MOD_SHIFT / MOD_WIN.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace beam {

enum HotkeyModifier : uint32_t {
    kModAlt = 0x1,
    kModControl = 0x2,
    kModShift = 0x4,
    kModWin = 0x8,
};

struct Hotkey {
    uint32_t mods = 0;
    uint32_t vk = 0;
    bool operator==(const Hotkey&) const = default;
};

// Accepts "Alt+Space", "ctrl + shift + k", "Win+`", "F13". Returns nullopt for
// unknown keys and for keys that would be unusable globally (no modifier and
// not a function key).
std::optional<Hotkey> ParseHotkey(std::wstring_view text);
// Same syntax without the global-hotkey rule ("Enter", "Ctrl+Shift+C", "Alt+Up").
// Used for keyboard shortcuts of actions inside the launcher.
std::optional<Hotkey> ParseKeyChord(std::wstring_view text);
std::wstring FormatHotkey(const Hotkey& hotkey);                // "Ctrl+Alt+K"
std::vector<std::wstring> HotkeyParts(const Hotkey& hotkey);    // {"Ctrl","Alt","K"}
const wchar_t* VirtualKeyName(uint32_t vk);                     // nullptr if unsupported
bool IsModifierVirtualKey(uint32_t vk);
bool IsValidHotkey(const Hotkey& hotkey);

}  // namespace beam
