#include "core/hotkey.hpp"

#include <array>

#include "core/text.hpp"

namespace beam {
namespace {

struct KeyName {
    uint32_t vk;
    const wchar_t* name;
};

// Canonical display names. Letters, digits and F-keys are generated.
constexpr std::array kNamedKeys = {
    KeyName{0x08, L"Backspace"}, KeyName{0x09, L"Tab"},        KeyName{0x0D, L"Enter"},
    KeyName{0x13, L"Pause"},     KeyName{0x14, L"CapsLock"},   KeyName{0x1B, L"Esc"},
    KeyName{0x20, L"Space"},     KeyName{0x21, L"PageUp"},     KeyName{0x22, L"PageDown"},
    KeyName{0x23, L"End"},       KeyName{0x24, L"Home"},       KeyName{0x25, L"Left"},
    KeyName{0x26, L"Up"},        KeyName{0x27, L"Right"},      KeyName{0x28, L"Down"},
    KeyName{0x2C, L"PrintScreen"}, KeyName{0x2D, L"Insert"},   KeyName{0x2E, L"Delete"},
    KeyName{0x60, L"Num0"},      KeyName{0x61, L"Num1"},       KeyName{0x62, L"Num2"},
    KeyName{0x63, L"Num3"},      KeyName{0x64, L"Num4"},       KeyName{0x65, L"Num5"},
    KeyName{0x66, L"Num6"},      KeyName{0x67, L"Num7"},       KeyName{0x68, L"Num8"},
    KeyName{0x69, L"Num9"},      KeyName{0x6A, L"NumMultiply"}, KeyName{0x6B, L"NumAdd"},
    KeyName{0x6D, L"NumSubtract"}, KeyName{0x6E, L"NumDecimal"}, KeyName{0x6F, L"NumDivide"},
    KeyName{0x91, L"ScrollLock"}, KeyName{0xBA, L";"},         KeyName{0xBB, L"="},
    KeyName{0xBC, L","},         KeyName{0xBD, L"-"},          KeyName{0xBE, L"."},
    KeyName{0xBF, L"/"},         KeyName{0xC0, L"`"},          KeyName{0xDB, L"["},
    KeyName{0xDC, L"\\"},        KeyName{0xDD, L"]"},          KeyName{0xDE, L"'"},
};

// Extra spellings accepted when parsing.
constexpr std::array kAliases = {
    KeyName{0x0D, L"return"},   KeyName{0x1B, L"escape"},   KeyName{0x2E, L"del"},
    KeyName{0x2D, L"ins"},      KeyName{0x21, L"pgup"},     KeyName{0x22, L"pgdn"},
    KeyName{0x20, L"spacebar"}, KeyName{0xC0, L"backtick"}, KeyName{0xC0, L"backquote"},
    KeyName{0xC0, L"grave"},    KeyName{0xC0, L"tilde"},    KeyName{0xBD, L"minus"},
    KeyName{0xBB, L"equals"},   KeyName{0xBB, L"plus"},     KeyName{0xBC, L"comma"},
    KeyName{0xBE, L"period"},   KeyName{0xBE, L"dot"},      KeyName{0xBF, L"slash"},
    KeyName{0xDC, L"backslash"}, KeyName{0xBA, L"semicolon"}, KeyName{0xDE, L"quote"},
    KeyName{0xDE, L"apostrophe"}, KeyName{0xDB, L"lbracket"}, KeyName{0xDD, L"rbracket"},
    KeyName{0x2C, L"prtsc"},    KeyName{0x26, L"arrowup"},  KeyName{0x28, L"arrowdown"},
    KeyName{0x25, L"arrowleft"}, KeyName{0x27, L"arrowright"},
};

bool IsFunctionKey(uint32_t vk) { return vk >= 0x70 && vk <= 0x87; }

std::optional<uint32_t> ParseKey(std::wstring_view token) {
    if (token.size() == 1) {
        wchar_t c = token[0];
        if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - 32);
        if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')) return static_cast<uint32_t>(c);
    }
    if ((token[0] == L'f' || token[0] == L'F') && token.size() <= 3) {
        int n = 0;
        bool digits = token.size() > 1;
        for (size_t i = 1; i < token.size(); ++i) {
            if (!IsAsciiDigit(token[i])) digits = false;
            else n = n * 10 + (token[i] - L'0');
        }
        if (digits && n >= 1 && n <= 24) return 0x70u + static_cast<uint32_t>(n - 1);
    }
    for (const auto& k : kNamedKeys)
        if (EqualsIgnoreCase(token, k.name)) return k.vk;
    for (const auto& k : kAliases)
        if (EqualsIgnoreCase(token, k.name)) return k.vk;
    return std::nullopt;
}

}  // namespace

const wchar_t* VirtualKeyName(uint32_t vk) {
    static const std::array<std::wstring, 256> generated = [] {
        std::array<std::wstring, 256> names{};
        for (uint32_t c = '0'; c <= '9'; ++c) names[c] = std::wstring(1, static_cast<wchar_t>(c));
        for (uint32_t c = 'A'; c <= 'Z'; ++c) names[c] = std::wstring(1, static_cast<wchar_t>(c));
        for (uint32_t i = 0; i < 24; ++i) names[0x70 + i] = L"F" + std::to_wstring(i + 1);
        return names;
    }();
    if (vk < 256 && !generated[vk].empty()) return generated[vk].c_str();
    for (const auto& k : kNamedKeys)
        if (k.vk == vk) return k.name;
    return nullptr;
}

bool IsModifierVirtualKey(uint32_t vk) {
    switch (vk) {
        case 0x10: case 0x11: case 0x12:              // SHIFT, CONTROL, MENU
        case 0x5B: case 0x5C:                         // LWIN, RWIN
        case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4: case 0xA5:
            return true;
        default:
            return false;
    }
}

bool IsValidHotkey(const Hotkey& hk) {
    if (hk.vk == 0 || IsModifierVirtualKey(hk.vk) || VirtualKeyName(hk.vk) == nullptr) return false;
    return hk.mods != 0 || IsFunctionKey(hk.vk);
}

namespace {

std::optional<Hotkey> ParseChord(std::wstring_view text, bool requireGlobal) {
    Hotkey hk;
    bool haveKey = false;
    size_t start = 0;
    const std::wstring s = Trim(text);
    if (s.empty()) return std::nullopt;
    while (start <= s.size()) {
        size_t plus = s.find(L'+', start);
        if (plus == std::wstring::npos) plus = s.size();
        const std::wstring token = Trim(std::wstring_view(s).substr(start, plus - start));
        if (token.empty()) return std::nullopt;
        if (EqualsIgnoreCase(token, L"ctrl") || EqualsIgnoreCase(token, L"control")) {
            hk.mods |= kModControl;
        } else if (EqualsIgnoreCase(token, L"alt")) {
            hk.mods |= kModAlt;
        } else if (EqualsIgnoreCase(token, L"shift")) {
            hk.mods |= kModShift;
        } else if (EqualsIgnoreCase(token, L"win") || EqualsIgnoreCase(token, L"windows") ||
                   EqualsIgnoreCase(token, L"meta") || EqualsIgnoreCase(token, L"super")) {
            hk.mods |= kModWin;
        } else {
            if (haveKey) return std::nullopt;  // two non-modifier keys
            const auto vk = ParseKey(token);
            if (!vk) return std::nullopt;
            hk.vk = *vk;
            haveKey = true;
        }
        start = plus + 1;
        if (plus == s.size()) break;
    }
    if (!haveKey) return std::nullopt;
    if (requireGlobal && !IsValidHotkey(hk)) return std::nullopt;
    return hk;
}

}  // namespace

std::optional<Hotkey> ParseHotkey(std::wstring_view text) { return ParseChord(text, true); }
std::optional<Hotkey> ParseKeyChord(std::wstring_view text) { return ParseChord(text, false); }

std::vector<std::wstring> HotkeyParts(const Hotkey& hk) {
    std::vector<std::wstring> parts;
    if (hk.mods & kModWin) parts.emplace_back(L"Win");
    if (hk.mods & kModControl) parts.emplace_back(L"Ctrl");
    if (hk.mods & kModAlt) parts.emplace_back(L"Alt");
    if (hk.mods & kModShift) parts.emplace_back(L"Shift");
    if (const wchar_t* name = VirtualKeyName(hk.vk)) parts.emplace_back(name);
    return parts;
}

std::wstring FormatHotkey(const Hotkey& hk) {
    std::wstring out;
    for (const auto& part : HotkeyParts(hk)) {
        if (!out.empty()) out += L'+';
        out += part;
    }
    return out;
}

}  // namespace beam
