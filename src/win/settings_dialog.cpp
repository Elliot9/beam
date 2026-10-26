#include "win/settings_dialog.hpp"

#include <algorithm>
#include <vector>

#include "core/query.hpp"
#include "core/settings.hpp"
#include "core/text.hpp"
#include "resource.h"
#include "win/app.hpp"
#include "win/log.hpp"

namespace beam {
namespace {

constexpr wchar_t kHotkeyBoxClass[] = L"BeamHotkeyBox";

struct HotkeyBoxState {
    std::optional<Hotkey> value;
    bool allowEmpty = true;
    bool recording = false;
    bool rejected = false;  // the last combination was not usable globally
    uint32_t pendingMods = 0;
    HFONT font = nullptr;
};

HotkeyBoxState* StateOf(HWND hwnd) { return reinterpret_cast<HotkeyBoxState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)); }

uint32_t CurrentModifiers() {
    uint32_t m = 0;
    if (GetKeyState(VK_CONTROL) < 0) m |= kModControl;
    if (GetKeyState(VK_MENU) < 0) m |= kModAlt;
    if (GetKeyState(VK_SHIFT) < 0) m |= kModShift;
    if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) m |= kModWin;
    return m;
}

void NotifyParent(HWND box, WORD code) {
    SendMessageW(GetParent(box), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(box), code), reinterpret_cast<LPARAM>(box));
}

void MoveFocusOn(HWND box, bool backwards = false) {
    PostMessageW(GetParent(box), WM_NEXTDLGCTL, backwards ? 1 : 0, FALSE);
}

void OnBoxKey(HWND hwnd, HotkeyBoxState* st, UINT vk) {
    if (!st || vk == VK_PROCESSKEY) return;
    const uint32_t mods = CurrentModifiers();
    if (IsModifierVirtualKey(vk)) {
        st->pendingMods = mods;
        st->rejected = false;
        InvalidateRect(hwnd, nullptr, TRUE);
        return;
    }
    if (mods == 0) {
        if (vk == VK_TAB || vk == VK_ESCAPE) {
            MoveFocusOn(hwnd);
            return;
        }
        if (vk == VK_BACK || vk == VK_DELETE) {
            if (st->allowEmpty && st->value) {
                st->value.reset();
                NotifyParent(hwnd, kHotkeyBoxChanged);
            }
            MoveFocusOn(hwnd);
            return;
        }
    }
    if (vk == VK_TAB && mods == kModShift) {
        MoveFocusOn(hwnd, true);
        return;
    }
    const Hotkey hk{mods, vk};
    if (!IsValidHotkey(hk)) {
        st->rejected = true;
        MessageBeep(MB_OK);
        InvalidateRect(hwnd, nullptr, TRUE);
        return;
    }
    st->value = hk;
    st->pendingMods = 0;
    NotifyParent(hwnd, kHotkeyBoxChanged);
    InvalidateRect(hwnd, nullptr, TRUE);
    // Done recording: park focus on Save instead of starting to record in the next box.
    const HWND parent = GetParent(hwnd);
    PostMessageW(parent, WM_NEXTDLGCTL, reinterpret_cast<WPARAM>(GetDlgItem(parent, IDOK)), TRUE);
}

void PaintBox(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    const HotkeyBoxState* st = StateOf(hwnd);
    const bool focused = GetFocus() == hwnd;

    FillRect(dc, &rc, GetSysColorBrush(COLOR_WINDOW));
    HPEN border = CreatePen(PS_SOLID, 1, GetSysColor(focused ? COLOR_HIGHLIGHT : COLOR_BTNSHADOW));
    HGDIOBJ oldPen = SelectObject(dc, border);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
    if (focused) Rectangle(dc, rc.left + 1, rc.top + 1, rc.right - 1, rc.bottom - 1);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(border);

    HGDIOBJ oldFont = SelectObject(dc, st && st->font ? static_cast<HGDIOBJ>(st->font) : GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(dc, TRANSPARENT);
    std::vector<std::wstring> parts;
    std::wstring hint;
    if (st && st->recording) {
        if (st->pendingMods) parts = HotkeyParts(Hotkey{st->pendingMods, 0});
        hint = st->rejected ? L"需要搭配 Ctrl / Alt / Shift / Win" : (parts.empty() ? L"請按下新的快捷鍵…" : L"…");
    } else if (st && st->value) {
        parts = HotkeyParts(*st->value);
    } else {
        hint = L"未設定";
    }

    SIZE em{};
    GetTextExtentPoint32W(dc, L"Ag", 2, &em);
    const int pad = std::max<int>(4, em.cy / 3);
    const int capHeight = std::min<int>(rc.bottom - rc.top - 4, em.cy + 4);
    const int capTop = (rc.top + rc.bottom - capHeight) / 2;
    int x = rc.left + pad + 1;
    HBRUSH capBrush = CreateSolidBrush(RGB(236, 236, 241));
    HPEN capPen = CreatePen(PS_SOLID, 1, RGB(198, 198, 206));
    for (const auto& part : parts) {
        SIZE sz{};
        GetTextExtentPoint32W(dc, part.c_str(), static_cast<int>(part.size()), &sz);
        RECT cap{x, capTop, x + sz.cx + 2 * pad, capTop + capHeight};
        oldPen = SelectObject(dc, capPen);
        oldBrush = SelectObject(dc, capBrush);
        RoundRect(dc, cap.left, cap.top, cap.right, cap.bottom, pad + 2, pad + 2);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        SetTextColor(dc, RGB(40, 40, 46));
        DrawTextW(dc, part.c_str(), -1, &cap, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        x = cap.right + pad / 2 + 1;
    }
    DeleteObject(capBrush);
    DeleteObject(capPen);
    if (!hint.empty()) {
        RECT tr{x + (parts.empty() ? 0 : 2), rc.top, rc.right - pad, rc.bottom};
        SetTextColor(dc, GetSysColor(st && st->recording ? COLOR_HOTLIGHT : COLOR_GRAYTEXT));
        DrawTextW(dc, hint.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    SelectObject(dc, oldFont);
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK HotkeyBoxProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    HotkeyBoxState* st = StateOf(hwnd);
    switch (msg) {
        case WM_NCCREATE:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(new HotkeyBoxState()));
            break;
        case WM_NCDESTROY:
            delete st;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            break;
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS | DLGC_WANTARROWS | DLGC_WANTCHARS | DLGC_WANTTAB;
        case WM_SETFONT:
            if (st) st->font = reinterpret_cast<HFONT>(wp);
            if (LOWORD(lp)) InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        case WM_GETFONT:
            return reinterpret_cast<LRESULT>(st ? st->font : nullptr);
        case WM_LBUTTONDOWN:
            SetFocus(hwnd);
            return 0;
        case WM_SETFOCUS:
            if (st) {
                st->recording = true;
                st->rejected = false;
                st->pendingMods = 0;
            }
            NotifyParent(hwnd, kHotkeyBoxRecordingStarted);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        case WM_KILLFOCUS:
            if (st) {
                st->recording = false;
                st->pendingMods = 0;
            }
            NotifyParent(hwnd, kHotkeyBoxRecordingStopped);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            OnBoxKey(hwnd, st, static_cast<UINT>(wp));
            return 0;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (st && st->recording) {
                st->pendingMods = CurrentModifiers();
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;
        case WM_CHAR:
        case WM_SYSCHAR:
        case WM_DEADCHAR:
        case WM_SYSDEADCHAR:
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            PaintBox(hwnd);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

void RegisterHotkeyBoxClass(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HotkeyBoxProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    wc.lpszClassName = kHotkeyBoxClass;
    RegisterClassExW(&wc);
}

void SetHotkeyBoxValue(HWND box, std::optional<Hotkey> value, bool allowEmpty) {
    if (HotkeyBoxState* st = StateOf(box)) {
        st->value = value;
        st->allowEmpty = allowEmpty;
        InvalidateRect(box, nullptr, TRUE);
    }
}

std::optional<Hotkey> GetHotkeyBoxValue(HWND box) {
    const HotkeyBoxState* st = StateOf(box);
    return st ? st->value : std::nullopt;
}

// ---------------------------------------------------------------------------

SettingsDialog::~SettingsDialog() {
    if (hwnd_) DestroyWindow(hwnd_);
    if (bigIcon_) DestroyIcon(bigIcon_);
    if (smallIcon_) DestroyIcon(smallIcon_);
}

bool SettingsDialog::Create() {
    hwnd_ = CreateDialogParamW(app_.instance(), MAKEINTRESOURCEW(IDD_SETTINGS), nullptr, DialogProc,
                               reinterpret_cast<LPARAM>(this));
    if (!hwnd_) {
        log::Error(L"CreateDialogParam failed: " + log::ErrorText(GetLastError()));
        return false;
    }
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    return true;
}

void SettingsDialog::Activate() {
    if (!hwnd_) return;
    if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
    SetForegroundWindow(hwnd_);
}

INT_PTR CALLBACK SettingsDialog::DialogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SettingsDialog* self = nullptr;
    if (msg == WM_INITDIALOG) {
        self = reinterpret_cast<SettingsDialog*>(lp);
        SetWindowLongPtrW(hwnd, DWLP_USER, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<SettingsDialog*>(GetWindowLongPtrW(hwnd, DWLP_USER));
    }
    if (!self) return FALSE;
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, DWLP_USER, 0);
        self->hwnd_ = nullptr;
        self->app_.OnSettingsDialogClosed();  // deletes us later, never inside this call
        return FALSE;
    }
    return self->Handle(msg, wp, lp);
}

INT_PTR SettingsDialog::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_INITDIALOG:
            Initialize();
            return FALSE;  // focus was set explicitly
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDOK:
                    Save();
                    return TRUE;
                case IDCANCEL:
                    DestroyWindow(hwnd_);
                    return TRUE;
                case IDC_CLEAR_WINDOWS:
                    SetHotkeyBoxValue(GetDlgItem(hwnd_, IDC_HOTKEY_WINDOWS), std::nullopt, true);
                    return TRUE;
                case IDC_EDIT_JSON:
                    app_.OpenSettingsFile();
                    SetStatus(L"儲存 settings.json 後會自動套用", false);
                    return TRUE;
                case IDC_REINDEX:
                    app_.Reindex();
                    SetStatus(L"正在重新建立應用程式索引…", false);
                    return TRUE;
                case IDC_HOTKEY_MAIN:
                case IDC_HOTKEY_WINDOWS:
                    // While a box records, Beam's own hotkeys are released so they can be captured.
                    if (HIWORD(wp) == kHotkeyBoxRecordingStarted) app_.SetHotkeysSuspended(true);
                    else if (HIWORD(wp) == kHotkeyBoxRecordingStopped) app_.SetHotkeysSuspended(false);
                    else if (HIWORD(wp) == kHotkeyBoxChanged) SetStatus(L"", false);
                    return TRUE;
                default:
                    break;
            }
            break;
        case WM_CTLCOLORSTATIC:
            if (reinterpret_cast<HWND>(lp) == GetDlgItem(hwnd_, IDC_STATUS)) {
                HDC dc = reinterpret_cast<HDC>(wp);
                SetTextColor(dc, statusIsError_ ? RGB(196, 43, 28) : RGB(16, 124, 16));
                SetBkMode(dc, TRANSPARENT);
                return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_3DFACE));
            }
            break;
        case WM_CLOSE:
            DestroyWindow(hwnd_);
            return TRUE;
        default:
            break;
    }
    return FALSE;
}

void SettingsDialog::Initialize() {
    const Settings& s = app_.settings();
    SetWindowTextW(hwnd_, L"Beam 設定");
    struct Label {
        int id;
        const wchar_t* text;
    };
    const Label labels[] = {
        {IDC_GROUP_HOTKEYS, L"快捷鍵"},
        {IDC_LABEL_MAIN, L"呼叫 Beam"},
        {IDC_LABEL_WINDOWS, L"直接切換視窗"},
        {IDC_CLEAR_WINDOWS, L"清除"},
        {IDC_GROUP_GENERAL, L"一般"},
        {IDC_STARTUP, L"開機時自動啟動 Beam"},
        {IDC_INCLUDE_WINDOWS, L"搜尋結果包含已開啟的視窗"},
        {IDC_LABEL_THEME, L"主題"},
        {IDC_LABEL_ENGINE, L"搜尋引擎網址"},
        {IDC_EDIT_JSON, L"編輯 settings.json…"},
        {IDC_REINDEX, L"重新建立索引"},
        {IDOK, L"儲存"},
        {IDCANCEL, L"取消"},
    };
    for (const auto& l : labels) SetDlgItemTextW(hwnd_, l.id, l.text);

    const auto mainHotkey = ParseHotkey(s.hotkey);
    std::wstring welcome;
    if (welcome_) {
        welcome = L"歡迎使用 Beam！之後隨時按 " + (mainHotkey ? FormatHotkey(*mainHotkey) : s.hotkey) +
                  L" 呼叫。可以在這裡改成習慣的快捷鍵。";
    } else {
        welcome = L"點一下快捷鍵欄位，直接按下新的組合鍵；Backspace 清除，Esc 取消。";
    }
    SetDlgItemTextW(hwnd_, IDC_WELCOME, welcome.c_str());

    SetHotkeyBoxValue(GetDlgItem(hwnd_, IDC_HOTKEY_MAIN), mainHotkey, false);
    SetHotkeyBoxValue(GetDlgItem(hwnd_, IDC_HOTKEY_WINDOWS), ParseHotkey(s.windowSwitcherHotkey), true);
    CheckDlgButton(hwnd_, IDC_STARTUP, s.launchAtStartup ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hwnd_, IDC_INCLUDE_WINDOWS, s.includeWindowsInSearch ? BST_CHECKED : BST_UNCHECKED);

    HWND theme = GetDlgItem(hwnd_, IDC_THEME);
    for (const wchar_t* option : {L"跟隨 Windows", L"深色", L"淺色"})
        SendMessageW(theme, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(option));
    SendMessageW(theme, CB_SETCURSEL, s.theme == L"dark" ? 1 : s.theme == L"light" ? 2 : 0, 0);
    SetDlgItemTextW(hwnd_, IDC_ENGINE, s.searchEngineUrl.c_str());

    bigIcon_ = static_cast<HICON>(LoadImageW(app_.instance(), MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                             GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
    smallIcon_ = static_cast<HICON>(LoadImageW(app_.instance(), MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(bigIcon_));
    SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon_));
    SendMessageW(hwnd_, WM_NEXTDLGCTL, reinterpret_cast<WPARAM>(GetDlgItem(hwnd_, IDOK)), TRUE);
}

void SettingsDialog::SetStatus(const std::wstring& text, bool error) {
    statusIsError_ = error;
    SetDlgItemTextW(hwnd_, IDC_STATUS, text.c_str());
    InvalidateRect(GetDlgItem(hwnd_, IDC_STATUS), nullptr, TRUE);
}

void SettingsDialog::Save() {
    Settings s = app_.settings();
    const auto mainHotkey = GetHotkeyBoxValue(GetDlgItem(hwnd_, IDC_HOTKEY_MAIN));
    const auto windowsHotkey = GetHotkeyBoxValue(GetDlgItem(hwnd_, IDC_HOTKEY_WINDOWS));
    if (!mainHotkey) {
        SetStatus(L"請設定呼叫 Beam 的快捷鍵", true);
        return;
    }
    if (windowsHotkey && *windowsHotkey == *mainHotkey) {
        SetStatus(L"兩個快捷鍵不能相同", true);
        return;
    }
    s.hotkey = FormatHotkey(*mainHotkey);
    s.windowSwitcherHotkey = windowsHotkey ? FormatHotkey(*windowsHotkey) : L"";
    s.launchAtStartup = IsDlgButtonChecked(hwnd_, IDC_STARTUP) == BST_CHECKED;
    s.includeWindowsInSearch = IsDlgButtonChecked(hwnd_, IDC_INCLUDE_WINDOWS) == BST_CHECKED;
    const LRESULT theme = SendDlgItemMessageW(hwnd_, IDC_THEME, CB_GETCURSEL, 0, 0);
    s.theme = theme == 1 ? L"dark" : theme == 2 ? L"light" : L"auto";

    HWND engineBox = GetDlgItem(hwnd_, IDC_ENGINE);
    std::wstring engine(static_cast<size_t>(GetWindowTextLengthW(engineBox)) + 1, L'\0');
    engine.resize(static_cast<size_t>(GetWindowTextW(engineBox, engine.data(), static_cast<int>(engine.size()))));
    engine = Trim(engine);
    if (!HasQueryPlaceholder(engine)) {
        SetStatus(L"搜尋引擎網址需要包含 {query}", true);
        return;
    }
    s.searchEngineUrl = engine;

    const std::wstring error = app_.ApplySettings(s, true);
    if (!error.empty()) {
        SetStatus(error, true);
        return;
    }
    DestroyWindow(hwnd_);
}

}  // namespace beam
