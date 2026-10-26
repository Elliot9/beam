// Key values: Windows virtual-key codes (used by the portable hotkey parser) to GDK key
// values, plugin shortcut strings, and keycap labels.
#include <gtk/gtk.h>

#include "core/hotkey.hpp"
#include "linux/results.hpp"

namespace beam {

guint VkToKeyval(uint32_t vk) {
    if (vk >= 'A' && vk <= 'Z') return GDK_KEY_a + (vk - 'A');
    if (vk >= '0' && vk <= '9') return GDK_KEY_0 + (vk - '0');
    if (vk >= 0x70 && vk <= 0x87) return GDK_KEY_F1 + (vk - 0x70);  // F1..F24
    if (vk >= 0x60 && vk <= 0x69) return GDK_KEY_KP_0 + (vk - 0x60);
    switch (vk) {
        case 0x08: return GDK_KEY_BackSpace;
        case 0x09: return GDK_KEY_Tab;
        case 0x0D: return GDK_KEY_Return;
        case 0x13: return GDK_KEY_Pause;
        case 0x14: return GDK_KEY_Caps_Lock;
        case 0x1B: return GDK_KEY_Escape;
        case 0x20: return GDK_KEY_space;
        case 0x21: return GDK_KEY_Page_Up;
        case 0x22: return GDK_KEY_Page_Down;
        case 0x23: return GDK_KEY_End;
        case 0x24: return GDK_KEY_Home;
        case 0x25: return GDK_KEY_Left;
        case 0x26: return GDK_KEY_Up;
        case 0x27: return GDK_KEY_Right;
        case 0x28: return GDK_KEY_Down;
        case 0x2C: return GDK_KEY_Print;
        case 0x2D: return GDK_KEY_Insert;
        case 0x2E: return GDK_KEY_Delete;
        case 0x6A: return GDK_KEY_KP_Multiply;
        case 0x6B: return GDK_KEY_KP_Add;
        case 0x6D: return GDK_KEY_KP_Subtract;
        case 0x6E: return GDK_KEY_KP_Decimal;
        case 0x6F: return GDK_KEY_KP_Divide;
        case 0x91: return GDK_KEY_Scroll_Lock;
        case 0xBA: return GDK_KEY_semicolon;
        case 0xBB: return GDK_KEY_equal;
        case 0xBC: return GDK_KEY_comma;
        case 0xBD: return GDK_KEY_minus;
        case 0xBE: return GDK_KEY_period;
        case 0xBF: return GDK_KEY_slash;
        case 0xC0: return GDK_KEY_grave;
        case 0xDB: return GDK_KEY_bracketleft;
        case 0xDC: return GDK_KEY_backslash;
        case 0xDD: return GDK_KEY_bracketright;
        case 0xDE: return GDK_KEY_apostrophe;
        default: return 0;
    }
}

Shortcut ParseShortcut(const std::wstring& text) {
    const auto chord = ParseKeyChord(text);
    if (!chord || (chord->mods & kModWin)) return {};
    const guint keyval = VkToKeyval(chord->vk);
    if (!keyval) return {};
    return Keys(keyval, (chord->mods & kModControl) != 0, (chord->mods & kModShift) != 0, (chord->mods & kModAlt) != 0);
}

std::vector<std::wstring> Shortcut::Parts() const {
    std::vector<std::wstring> parts;
    if (keyval == 0) return parts;
    if (ctrl) parts.emplace_back(L"Ctrl");
    if (shift) parts.emplace_back(L"Shift");
    if (alt) parts.emplace_back(L"Alt");
    switch (keyval) {
        case GDK_KEY_Return: parts.emplace_back(L"Enter"); break;
        case GDK_KEY_Up: parts.emplace_back(L"↑"); break;
        case GDK_KEY_Down: parts.emplace_back(L"↓"); break;
        case GDK_KEY_Left: parts.emplace_back(L"←"); break;
        case GDK_KEY_Right: parts.emplace_back(L"→"); break;
        case GDK_KEY_Escape: parts.emplace_back(L"Esc"); break;
        case GDK_KEY_BackSpace: parts.emplace_back(L"Backspace"); break;
        case GDK_KEY_Delete: parts.emplace_back(L"Delete"); break;
        case GDK_KEY_space: parts.emplace_back(L"Space"); break;
        case GDK_KEY_Tab: parts.emplace_back(L"Tab"); break;
        default: {
            const guint32 ch = gdk_keyval_to_unicode(gdk_keyval_to_upper(keyval));
            if (ch > 0x20 && ch < 0x7F) {
                parts.emplace_back(1, static_cast<wchar_t>(ch));
            } else if (const char* name = gdk_keyval_name(keyval)) {
                parts.emplace_back(Utf8ToWide(name));
            }
            break;
        }
    }
    return parts;
}

}  // namespace beam
