#include "win/launcher.hpp"

#include <imm.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

#include "core/fuzzy.hpp"
#include "core/text.hpp"
#include "resource.h"
#include "win/app.hpp"
#include "win/search.hpp"
#include "win/shell.hpp"

namespace beam {
namespace {

constexpr float kShadowMargin = 28.0f;
constexpr float kFrameWidth = 750.0f;
constexpr float kFrameHeight = 474.0f;
constexpr float kRadius = 12.0f;
constexpr float kSearchHeight = 56.0f;
constexpr float kFooterHeight = 40.0f;
constexpr float kRowHeight = 42.0f;
constexpr float kHeaderHeight = 28.0f;
constexpr float kListPadding = 6.0f;
constexpr float kDetailPadding = 20.0f;
constexpr float kDetailTop = 12.0f;
constexpr float kDetailStep = 34.0f;  // arrow-key scroll distance
constexpr ULONGLONG kPopToRootAfterMs = 90'000;
constexpr ULONGLONG kConfirmWindowMs = 4'000;
constexpr size_t kMaxDetailChars = 300'000;

constexpr UINT_PTR kCaretTimer = 1;
constexpr UINT_PTR kStatusTimer = 2;
constexpr UINT_PTR kWindowsTimer = 3;
constexpr UINT_PTR kPluginTimer = 4;  // debounced plugin query
constexpr UINT_PTR kBusyTimer = 5;    // "載入中…" only for queries slower than this

constexpr wchar_t kSectionResults[] = L"結果";

// Plugin and Detail are sub-views of Root; the window switcher is its own family.
Mode Family(Mode m) { return m == Mode::Windows ? Mode::Windows : Mode::Root; }

bool Contains(const D2D1_RECT_F& r, float x, float y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

bool IsEmpty(const D2D1_RECT_F& r) { return r.right <= r.left; }

// X offset of a caret position inside `text`. Uses DirectWrite hit-testing and
// falls back to measuring the prefix (identical for single-line LTR text).
float CaretOffset(const Renderer& r, IDWriteTextLayout* layout, const std::wstring& text, size_t pos) {
    if (pos == 0 || text.empty()) return 0.0f;
    pos = std::min(pos, text.size());
    float x = 0, y = 0;
    DWRITE_HIT_TEST_METRICS m{};
    if (layout && SUCCEEDED(layout->HitTestTextPosition(static_cast<UINT32>(pos - 1), TRUE, &x, &y, &m))) return x;
    return r.TextWidth(std::wstring_view(text).substr(0, pos), Font::Input);
}

}  // namespace

LauncherWindow::LauncherWindow(App& app) : app_(app) {}

LauncherWindow::~LauncherWindow() {
    if (hwnd_) DestroyWindow(hwnd_);
}

bool LauncherWindow::Create(HINSTANCE instance) {
    if (!renderer_.Initialize()) return false;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    wc.lpszClassName = kLauncherWindowClass;
    RegisterClassExW(&wc);
    CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kLauncherWindowClass, kAppName, WS_POPUP, 0, 0,
                    1, 1, nullptr, nullptr, instance, this);
    return hwnd_ != nullptr;
}

LRESULT CALLBACK LauncherWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<LauncherWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<LauncherWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    if (msg == WM_NCDESTROY && self) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        self->hwnd_ = nullptr;
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Showing / hiding

D2D1_RECT_F LauncherWindow::FrameRect() const {
    return {kShadowMargin, kShadowMargin, kShadowMargin + kFrameWidth, kShadowMargin + kFrameHeight};
}

D2D1_RECT_F LauncherWindow::ListRect() const {
    const auto f = FrameRect();
    return {f.left, f.top + kSearchHeight + 1, f.right, f.bottom - kFooterHeight};
}

D2D1_RECT_F LauncherWindow::FooterRect() const {
    const auto f = FrameRect();
    return {f.left, f.bottom - kFooterHeight, f.right, f.bottom};
}

int LauncherWindow::PixelWidth() const {
    return static_cast<int>(std::lround((kFrameWidth + 2 * kShadowMargin) * scale_));
}

int LauncherWindow::PixelHeight() const {
    return static_cast<int>(std::lround((kFrameHeight + 2 * kShadowMargin) * scale_));
}

void LauncherWindow::PositionOnCursorMonitor() {
    POINT cursor{};
    GetCursorPos(&cursor);
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return;
    UINT dpiX = 96, dpiY = 96;
    if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) || dpiX == 0) dpiX = 96;
    const float scale = dpiX / 96.0f;
    if (scale != scale_) detailLayout_.Reset();
    scale_ = scale;
    const RECT& work = info.rcWork;
    const int w = PixelWidth();
    const int h = PixelHeight();
    position_.x = work.left + (work.right - work.left - w) / 2;
    position_.y = work.top + static_cast<int>((work.bottom - work.top - h) * 0.22);
    position_.y = std::max<LONG>(position_.y, work.top - static_cast<LONG>(kShadowMargin * scale_));
}

void LauncherWindow::ForceForeground() {
    if (GetForegroundWindow() == hwnd_) return;
    if (SetForegroundWindow(hwnd_)) return;
    // The hotkey normally grants foreground rights; if Windows still refuses, attach briefly.
    HWND fg = GetForegroundWindow();
    const DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const DWORD self = GetCurrentThreadId();
    if (fgThread && fgThread != self && AttachThreadInput(self, fgThread, TRUE)) {
        BringWindowToTop(hwnd_);
        SetForegroundWindow(hwnd_);
        SetFocus(hwnd_);
        AttachThreadInput(self, fgThread, FALSE);
    }
}

void LauncherWindow::Show(Mode mode) {
    const bool wasVisible = visible_;
    if (!wasVisible) windows_ = EnumerateWindows();  // before we cover anything
    // Like Raycast: reopening shortly after continues where you were (also inside a plugin);
    // after a while it pops back to the root search.
    const bool reset =
        Family(mode) != Family(mode_) || (!wasVisible && GetTickCount64() - hiddenAt_ > kPopToRootAfterMs);
    if (reset) {
        LeaveSubViews();
        mode_ = mode;
        edit_.SetText(L"");
        composition_.clear();
        inputScroll_ = 0;
    } else if (!wasVisible && mode_ != Mode::Detail) {
        edit_.SelectAll();  // typing replaces the old query
    }
    if (!wasVisible) PositionOnCursorMonitor();
    RefreshResults();
    visible_ = true;
    caretOn_ = true;
    Render();
    ShowWindow(hwnd_, SW_SHOW);
    ForceForeground();
    SetFocus(hwnd_);
    // Windows sends a WM_MOUSEMOVE when a window appears under a resting cursor;
    // remember where it is so that message does not change the selection.
    POINT cursor{};
    GetCursorPos(&cursor);
    ScreenToClient(hwnd_, &cursor);
    lastMouse_ = cursor;
    CreateCaret(hwnd_, nullptr, 1, static_cast<int>(22 * scale_));  // hidden system caret, for IME/accessibility
    UpdateImeWindow();
    const UINT blink = GetCaretBlinkTime();
    if (blink != INFINITE && blink > 0) SetTimer(hwnd_, kCaretTimer, blink, nullptr);
}

void LauncherWindow::Toggle(Mode mode) {
    if (visible_ && IsActive() && Family(mode) == Family(mode_)) Hide();
    else Show(mode);
}

void LauncherWindow::Hide() {
    if (!visible_) return;
    visible_ = false;
    actionsOpen_ = false;
    Disarm();
    CancelComposition();
    DestroyCaret();
    if (GetCapture() == hwnd_) ReleaseCapture();
    draggingText_ = false;
    KillTimer(hwnd_, kCaretTimer);
    ShowWindow(hwnd_, SW_HIDE);
    hiddenAt_ = GetTickCount64();
}

// ---------------------------------------------------------------------------
// LauncherHost

void LauncherWindow::CancelComposition() {
    if (composition_.empty()) return;
    if (HIMC himc = ImmGetContext(hwnd_)) {
        ImmNotifyIME(himc, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
        ImmReleaseContext(hwnd_, himc);
    }
    composition_.clear();
}

void LauncherWindow::SetQuery(const std::wstring& text) {
    CancelComposition();
    edit_.SetText(text);
    OnTextChanged();
}

void LauncherWindow::EnterMode(Mode mode) {
    if (mode == Mode::Windows) SwitchMode(Mode::Windows);
    else if (mode == Mode::Root) BackToRoot(L"");
}

void LauncherWindow::ShowStatus(const std::wstring& message, bool warning) {
    status_ = message;
    statusWarning_ = warning;
    SetTimer(hwnd_, kStatusTimer, warning ? 4000 : 2500, nullptr);
    RequestRender();
}

void LauncherWindow::RefreshWindows() { SetTimer(hwnd_, kWindowsTimer, 250, nullptr); }

void LauncherWindow::Refresh(const std::wstring& selectUsageKey) {
    RefreshResults(true);
    if (!selectUsageKey.empty()) {
        for (size_t i = 0; i < results_.size(); ++i) {
            if (results_[i].usageKey == selectUsageKey) {
                selected_ = static_cast<int>(i);
                EnsureVisible(selected_);
                break;
            }
        }
    }
    RequestRender();
}

void LauncherWindow::Report(const std::wstring& title, const std::wstring& message, bool warning) {
    if (visible_) ShowStatus(message, warning);
    else app_.Notify(title, message, warning);
}

void LauncherWindow::OnIndexChanged() {
    if (!visible_) return;
    RefreshResults(true);
    RequestRender();
}

void LauncherWindow::OnPluginsChanged() {
    const bool inPlugin = mode_ == Mode::Plugin || (mode_ == Mode::Detail && detailReturnMode_ == Mode::Plugin);
    if (inPlugin && !ActivePlugin()) {  // removed or renamed while we were looking at it
        BackToRoot(L"");
        if (visible_) ShowStatus(L"這個外掛已被移除或改名", true);
        return;
    }
    if (mode_ == Mode::Plugin) {
        const PluginCommand* cmd = ActivePlugin();
        if (cmd->spec.mode == PluginMode::List) {  // plugin.json was edited: query again with the new settings
            pluginLocal_ = cmd->spec.filtering == PluginFiltering::Beam;
            StartPluginQuery(pluginLocal_ ? std::wstring() : edit_.Text());
        }
        return;
    }
    if (visible_ && mode_ == Mode::Root) {
        RefreshResults(true);
        RequestRender();
    }
}

// ---------------------------------------------------------------------------
// Results & selection

void LauncherWindow::RefreshResults(bool keepSelection) {
    if (mode_ == Mode::Detail) return;  // results_ holds the detail view's own item
    const int previous = selected_;
    if (mode_ == Mode::Plugin) results_ = PluginView(edit_.Text());
    else results_ = app_.search().Search(edit_.Text(), mode_, windows_);
    BuildEntries();
    actionsOpen_ = false;
    const int count = static_cast<int>(results_.size());
    if (keepSelection && previous >= 0 && count > 0) {
        selected_ = std::min(previous, count - 1);
    } else {
        scroll_ = 0;
        selected_ = count == 0 ? -1 : 0;
        // Like Alt+Tab: with no filter the second window (the previous one) is pre-selected.
        if (mode_ == Mode::Windows && Trim(edit_.Text()).empty() && count > 1) selected_ = 1;
    }
    if (selected_ >= 0) EnsureVisible(selected_);
    else scroll_ = 0;
}

void LauncherWindow::BuildEntries() {
    entries_.clear();
    float y = kListPadding;
    std::wstring section;
    for (size_t i = 0; i < results_.size(); ++i) {
        if (i == 0 || results_[i].section != section) {
            section = results_[i].section;
            if (!section.empty()) {
                entries_.push_back({y, kHeaderHeight, -1, section});
                y += kHeaderHeight;
            }
        }
        entries_.push_back({y, kRowHeight, static_cast<int>(i), {}});
        y += kRowHeight;
    }
    contentHeight_ = y + kListPadding;
}

void LauncherWindow::OnTextChanged() {
    Disarm();
    caretOn_ = true;
    switch (mode_) {
        case Mode::Detail:
            return;
        case Mode::Plugin:
            OnPluginQueryChanged();
            return;
        case Mode::Root:
            if (TryEnterPluginByKeyword()) return;
            break;
        case Mode::Windows:
            break;
    }
    RefreshResults();
    RequestRender();
}

void LauncherWindow::SwitchMode(Mode mode) {
    CancelComposition();
    LeaveSubViews();
    mode_ = mode;
    edit_.SetText(L"");
    inputScroll_ = 0;
    if (mode == Mode::Windows) windows_ = EnumerateWindows();
    RefreshResults();
    RequestRender();
}

void LauncherWindow::LeaveSubViews() {
    if (pluginTicket_) app_.plugins().Cancel(pluginTicket_);  // only ever a query run
    pluginTicket_ = 0;
    pluginBusy_ = false;
    busyShown_ = false;
    if (hwnd_) {
        KillTimer(hwnd_, kPluginTimer);
        KillTimer(hwnd_, kBusyTimer);
    }
    pluginId_.clear();
    pluginReturnQuery_.clear();
    pluginPlaceholder_.clear();
    lastPluginQuery_.clear();
    pluginLocal_ = false;
    pluginFailed_ = false;
    pluginSilent_ = false;
    pluginAll_.clear();
    detailTitle_.clear();
    detailText_.clear();
    detailLayout_.Reset();
    detailScroll_ = 0;
    detailReturnMode_ = Mode::Root;
    detailReturnQuery_.clear();
    detailReturnResults_.clear();
}

void LauncherWindow::BackToRoot(std::wstring query) {
    CancelComposition();
    LeaveSubViews();
    mode_ = Mode::Root;
    status_.clear();  // a plugin's message does not belong to the root search
    statusWarning_ = false;
    edit_.SetText(query);
    inputScroll_ = 0;
    Disarm();
    RefreshResults();
    RequestRender();
}

void LauncherWindow::BackFromDetail() {
    if (mode_ != Mode::Detail) return;
    mode_ = detailReturnMode_;
    edit_.SetText(detailReturnQuery_);
    inputScroll_ = 0;
    results_ = std::move(detailReturnResults_);
    detailReturnResults_.clear();
    detailTitle_.clear();
    detailText_.clear();
    detailLayout_.Reset();
    detailScroll_ = 0;
    actionsOpen_ = false;
    Disarm();
    BuildEntries();
    const int count = static_cast<int>(results_.size());
    selected_ = count == 0 ? -1 : std::clamp(detailReturnSelected_, 0, count - 1);
    scroll_ = detailReturnScroll_;
    if (mode_ == Mode::Root || mode_ == Mode::Windows) RefreshResults(true);  // may have changed meanwhile
    else if (selected_ >= 0) EnsureVisible(selected_);
    RequestRender();
}

void LauncherWindow::GoBack() {
    switch (mode_) {
        case Mode::Detail: BackFromDetail(); break;
        case Mode::Plugin: BackToRoot(pluginReturnQuery_); break;
        case Mode::Windows: SwitchMode(Mode::Root); break;
        default: break;
    }
}

void LauncherWindow::Select(int index) {
    if (index == selected_) return;
    selected_ = index;
    Disarm();
    if (index >= 0) EnsureVisible(index);
    RequestRender();
}

void LauncherWindow::Move(int delta) {
    if (results_.empty()) return;
    const int count = static_cast<int>(results_.size());
    Select(std::clamp(selected_ + delta, 0, count - 1));
}

void LauncherWindow::EnsureVisible(int index) {
    const D2D1_RECT_F list = ListRect();
    const float view = list.bottom - list.top;
    for (size_t k = 0; k < entries_.size(); ++k) {
        if (entries_[k].item != index) continue;
        float top = entries_[k].top;
        const float bottom = top + entries_[k].height;
        if (k > 0 && entries_[k - 1].item < 0) top = entries_[k - 1].top;  // keep the section header in view
        if (k <= 1) top = 0;
        if (top < scroll_) scroll_ = top;
        else if (bottom + kListPadding > scroll_ + view) scroll_ = bottom + kListPadding - view;
        break;
    }
    scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, contentHeight_ - view));
}

void LauncherWindow::RunAction(int itemIndex, int actionIndex) {
    if (itemIndex < 0 || itemIndex >= static_cast<int>(results_.size())) return;
    const ResultItem item = results_[itemIndex];  // copy: the action may rebuild results_
    if (actionIndex < 0 || actionIndex >= static_cast<int>(item.actions.size())) return;
    const Action& action = item.actions[actionIndex];
    if (!action.run) return;
    if (action.dangerous) {
        const std::wstring key = item.title + L'\x1F' + action.title;
        const ULONGLONG now = GetTickCount64();
        if (armedKey_ != key || now - armedAt_ > kConfirmWindowMs) {
            armedKey_ = key;
            armedAt_ = now;
            ShowStatus(L"再按一次 Enter（或再點一次）確認：" + item.title, true);
            return;
        }
    }
    Disarm();
    actionsOpen_ = false;
    if (!item.usageKey.empty() && action.recordsUsage) app_.RecordUsage(item.usageKey, edit_.Text());
    suppressAutoHide_ = true;  // UAC prompts / activations must not hide us mid-action
    const Outcome outcome = action.run(*this);
    suppressAutoHide_ = false;
    if (outcome == Outcome::Hide || (visible_ && GetForegroundWindow() != hwnd_)) Hide();
    else RequestRender();
}

bool LauncherWindow::TryShortcut(UINT vk, bool ctrl, bool shift, bool alt) {
    if (selected_ < 0 || selected_ >= static_cast<int>(results_.size())) return false;
    const auto& actions = results_[selected_].actions;
    for (size_t i = 0; i < actions.size(); ++i) {
        if (actions[i].shortcut.Matches(vk, ctrl, shift, alt) || actions[i].alternate.Matches(vk, ctrl, shift, alt)) {
            RunAction(selected_, static_cast<int>(i));
            return true;
        }
    }
    return false;
}

void LauncherWindow::OpenActions() {
    if (selected_ < 0 || selected_ >= static_cast<int>(results_.size()) || results_[selected_].actions.empty())
        return;
    actionsOpen_ = true;
    actionSelected_ = 0;
    RequestRender();
}

void LauncherWindow::CloseActions() {
    if (!actionsOpen_) return;
    actionsOpen_ = false;
    actionRects_.clear();
    RequestRender();
}

std::wstring LauncherWindow::DefaultStatus() const {
    if (!commandBusyName_.empty()) return L"執行中：" + commandBusyName_ + L"…";
    switch (mode_) {
        case Mode::Windows:
            return std::to_wstring(windows_.size()) + L" 個視窗";
        case Mode::Plugin: {
            if (busyShown_) return L"載入中…";
            const PluginCommand* cmd = ActivePlugin();
            std::wstring text = cmd ? cmd->spec.name : std::wstring(L"外掛");
            if (!pluginFailed_ && !results_.empty()) text += L" · " + std::to_wstring(results_.size()) + L" 筆";
            return text;
        }
        case Mode::Detail:
            return L"Esc 返回";
        default:
            break;
    }
    if (app_.indexing()) return L"正在建立應用程式索引…";
    return L"Beam";
}

// ---------------------------------------------------------------------------
// Plugins

PluginResultSink LauncherWindow::ResultSink() const {
    const HWND hwnd = hwnd_;
    return [hwnd](std::unique_ptr<PluginResult> result) {
        PluginResult* raw = result.release();
        if (!PostMessageW(hwnd, WM_APP_PLUGIN_RESULT, 0, reinterpret_cast<LPARAM>(raw))) delete raw;
    };
}

const PluginCommand* LauncherWindow::ActivePlugin() const {
    return pluginId_.empty() ? nullptr : app_.plugins().Find(pluginId_);
}

// "keyword " (keyword + space) in the root search enters a list plugin, like Raycast aliases.
bool LauncherWindow::TryEnterPluginByKeyword() {
    const std::wstring& text = edit_.Text();
    size_t space = 0;
    while (space < text.size() && !IsSpace(text[space])) ++space;
    if (space == 0 || space == text.size()) return false;
    const std::wstring keyword = text.substr(0, space);
    const PluginCommand* cmd = app_.plugins().FindByKeyword(keyword);
    if (!cmd || cmd->spec.mode != PluginMode::List) return false;
    const std::wstring rest = TrimStart(std::wstring_view(text).substr(space));
    app_.RecordUsage(L"plugin:" + cmd->spec.id, keyword);
    EnterPluginScope(*cmd, rest, keyword);
    return true;
}

void LauncherWindow::EnterPlugin(const std::wstring& commandId, const std::wstring& query) {
    const PluginCommand* cmd = app_.plugins().Find(commandId);
    if (!cmd) {
        ShowStatus(L"找不到外掛「" + commandId + L"」（可能已被移除）", true);
        return;
    }
    if (cmd->spec.mode == PluginMode::Command) {
        RunPluginCommand(commandId, query);
        return;
    }
    std::wstring returnQuery;
    if (mode_ == Mode::Root) returnQuery = edit_.Text();
    else if (mode_ == Mode::Plugin && pluginId_ == commandId) returnQuery = pluginReturnQuery_;
    EnterPluginScope(*cmd, query, returnQuery);
}

void LauncherWindow::EnterPluginScope(const PluginCommand& cmd, const std::wstring& query,
                                      const std::wstring& returnQuery) {
    CancelComposition();
    LeaveSubViews();
    mode_ = Mode::Plugin;
    pluginId_ = cmd.spec.id;
    pluginReturnQuery_ = returnQuery;
    pluginLocal_ = cmd.spec.filtering == PluginFiltering::Beam;
    pluginPlaceholder_ = cmd.spec.placeholder;
    edit_.SetText(query);
    inputScroll_ = 0;
    actionsOpen_ = false;
    results_.clear();
    BuildEntries();
    selected_ = -1;
    scroll_ = 0;
    Disarm();
    // filtering "beam": fetch the full list once and filter it here as you type.
    StartPluginQuery(pluginLocal_ ? std::wstring() : query);
    RequestRender();
}

void LauncherWindow::OnPluginQueryChanged() {
    if (pluginLocal_) {
        RefreshResults();
        RequestRender();
        return;
    }
    const PluginCommand* cmd = ActivePlugin();
    if (!cmd) return;
    if (cmd->spec.debounceMs <= 0) StartPluginQuery(edit_.Text());
    else SetTimer(hwnd_, kPluginTimer, static_cast<UINT>(cmd->spec.debounceMs), nullptr);
    RequestRender();
}

void LauncherWindow::StartPluginQuery(const std::wstring& query) {
    KillTimer(hwnd_, kPluginTimer);
    const PluginCommand* cmd = ActivePlugin();
    if (!cmd) return;
    pluginTicket_ = app_.plugins().Start(ResultSink(), *cmd, PluginRequestKind::Query, query);
    pluginBusy_ = true;
    SetTimer(hwnd_, kBusyTimer, 150, nullptr);
}

void LauncherWindow::RunPluginCommand(const std::wstring& commandId, const std::wstring& argument) {
    const PluginCommand* cmd = app_.plugins().Find(commandId);
    if (!cmd) {
        ShowStatus(L"找不到外掛「" + commandId + L"」（可能已被移除）", true);
        return;
    }
    if (cmd->spec.mode == PluginMode::List) {
        EnterPlugin(commandId, argument);
        return;
    }
    commandTicket_ = app_.plugins().Start(ResultSink(), *cmd, PluginRequestKind::Command, argument);
    commandBusyName_ = cmd->spec.name;
    RequestRender();
}

void LauncherWindow::PluginCallback(const std::wstring& commandId, const Json& item, const Json& action) {
    const PluginCommand* cmd = app_.plugins().Find(commandId);
    if (!cmd) {
        ShowStatus(L"找不到外掛「" + commandId + L"」（可能已被移除）", true);
        return;
    }
    const std::wstring query = mode_ == Mode::Detail ? detailReturnQuery_ : edit_.Text();
    commandTicket_ = app_.plugins().Start(ResultSink(), *cmd, PluginRequestKind::Action, query, &item, &action);
    const Json* title = action.Find("title");
    commandBusyName_ = title && title->IsString() && !title->AsString().empty() ? Utf8ToWide(title->AsString())
                                                                              : cmd->spec.name;
    RequestRender();
}

void LauncherWindow::HandlePluginResult(std::unique_ptr<PluginResult> result) {
    if (!result) return;
    if (result->kind == PluginRequestKind::Query) {
        if (result->ticket != pluginTicket_ || result->commandId != pluginId_) return;  // superseded
        pluginBusy_ = false;
        busyShown_ = false;
        KillTimer(hwnd_, kBusyTimer);
        ApplyScopeResult(*result);
    } else {
        if (result->ticket != commandTicket_) return;  // a newer command was started meanwhile
        commandTicket_ = 0;
        commandBusyName_.clear();
        ApplyCommandResult(*result);
    }
    RequestRender();
}

std::vector<ResultItem> LauncherWindow::PluginView(const std::wstring& query) const {
    if (!pluginLocal_ || pluginFailed_) return pluginAll_;
    return FilterLocal(query);
}

std::vector<ResultItem> LauncherWindow::FilterLocal(const std::wstring& raw) const {
    const std::wstring q = Trim(raw);
    if (q.empty()) return pluginAll_;
    std::vector<ResultItem> out;
    for (const auto& item : pluginAll_) {
        std::vector<std::wstring> aliases;
        if (!item.subtitle.empty()) aliases.push_back(item.subtitle);
        if (!item.accessory.empty()) aliases.push_back(item.accessory);
        const int s = BestScore(q, item.title, aliases);
        if (s == kNoMatch) continue;
        ResultItem copy = item;
        copy.score = s;
        copy.section = kSectionResults;
        out.push_back(std::move(copy));
    }
    std::stable_sort(out.begin(), out.end(), [](const ResultItem& a, const ResultItem& b) { return a.score > b.score; });
    return out;
}

void LauncherWindow::ApplyScopeResult(const PluginResult& result) {
    const PluginCommand* cmd = ActivePlugin();
    if (!cmd) return;
    const bool sameQuery = result.query == lastPluginQuery_;
    lastPluginQuery_ = result.query;
    if (!result.failure.empty()) {
        ResultItem error = app_.search().PluginErrorItem(*cmd, result.failure);
        error.actions.insert(error.actions.begin() + 1,
                             Action{L"重試", glyph::kRefresh, Keys('R', true), false, [this](LauncherHost&) {
                                        StartPluginQuery(pluginLocal_ ? std::wstring() : edit_.Text());
                                        return Outcome::Stay;
                                    }});
        pluginAll_.clear();
        pluginAll_.push_back(std::move(error));
        pluginFailed_ = true;
    } else {
        const PluginResponse& r = result.response;
        pluginFailed_ = false;
        pluginSilent_ = !r.json;  // exit code 0 but nothing on stdout ({"items": []} means "no results")
        if (!r.placeholder.empty()) pluginPlaceholder_ = r.placeholder;
        pluginAll_ = app_.search().PluginItems(*cmd, r);
        if (!r.error.empty()) ShowStatus(r.error, true);
        else if (!r.message.empty()) ShowStatus(r.message, false);
    }
    if (mode_ == Mode::Detail) {  // update the list waiting behind the detail view
        detailReturnResults_ = PluginView(detailReturnQuery_);
        return;
    }
    const int previous = selected_;
    results_ = PluginView(edit_.Text());
    BuildEntries();
    actionsOpen_ = false;
    const int count = static_cast<int>(results_.size());
    if (sameQuery && previous >= 0 && count > 0) {
        selected_ = std::min(previous, count - 1);  // a refresh of the same query keeps your place
    } else {
        selected_ = count == 0 ? -1 : 0;
        scroll_ = 0;
    }
    if (selected_ >= 0) EnsureVisible(selected_);
    else scroll_ = 0;
}

void LauncherWindow::ShowPluginItems(const PluginCommand& cmd, const PluginResponse& response) {
    if (mode_ == Mode::Detail) BackFromDetail();
    if (mode_ != Mode::Plugin || pluginId_ != cmd.spec.id) {
        const std::wstring returnQuery = mode_ == Mode::Root ? edit_.Text() : std::wstring();
        CancelComposition();
        LeaveSubViews();
        mode_ = Mode::Plugin;
        pluginId_ = cmd.spec.id;
        pluginReturnQuery_ = returnQuery;
        pluginPlaceholder_ = cmd.spec.placeholder;
        edit_.SetText(L"");
        inputScroll_ = 0;
    }
    // A list returned by a command (not by live queries) is filtered by Beam as you type.
    if (cmd.spec.mode == PluginMode::Command) pluginLocal_ = true;
    if (!response.placeholder.empty()) pluginPlaceholder_ = response.placeholder;
    pluginFailed_ = false;
    pluginSilent_ = false;
    pluginAll_ = app_.search().PluginItems(cmd, response);
    lastPluginQuery_.clear();
    results_ = PluginView(edit_.Text());
    BuildEntries();
    actionsOpen_ = false;
    selected_ = results_.empty() ? -1 : 0;
    scroll_ = 0;
    Disarm();
}

// Result of a command run ("mode": "command") or of a callback action.
void LauncherWindow::ApplyCommandResult(const PluginResult& result) {
    const PluginCommand* found = app_.plugins().Find(result.commandId);
    const std::wstring name = found ? found->spec.name : result.commandId;
    const bool callback = result.kind == PluginRequestKind::Action;
    if (!result.failure.empty()) {
        Report(name, result.failure, true);
        return;
    }
    PluginResponse r = result.response;
    if (!r.json) {  // plain text from a command: one line is a message, more is shown as output
        const std::wstring text = Trim(r.output);
        r.output.clear();
        if (text.find_first_of(L"\r\n") == std::wstring::npos) r.message = text;
        else r.output = text;
    }
    bool stay = callback;  // commands close Beam when done unless they show something
    bool showsSomething = false;
    if (r.hasItems && found) {
        const PluginCommand cmd = *found;
        ShowPluginItems(cmd, r);
        stay = true;
        showsSomething = true;
    }
    if (r.directive.kind != PluginActionKind::None && found) {
        const PluginCommand cmd = *found;
        const Action action = app_.search().PluginActionFor(cmd, nullptr, r.directive);
        if (action.run) {
            suppressAutoHide_ = true;
            const Outcome outcome = action.run(*this);
            suppressAutoHide_ = false;
            if (!r.hasItems) stay = outcome == Outcome::Stay;
        }
    }
    if (!r.output.empty()) {
        ShowDetail(name, r.output);
        stay = true;
        showsSomething = true;
    }
    if (r.hide) stay = !*r.hide;
    const bool warning = !r.error.empty();
    const std::wstring message = warning ? r.error : r.message;
    if (!visible_) {
        // Finished while Beam was closed: the view waits for the next open (the 90 s
        // "pop to root" timer starts now) and a notification says so.
        if (showsSomething) hiddenAt_ = GetTickCount64();
        if (!message.empty()) app_.Notify(name, message, warning);
        else if (showsSomething) app_.Notify(name, L"已完成，開啟 Beam 就能看到結果");
        return;
    }
    if (stay) {
        if (!message.empty()) ShowStatus(message, warning);
        return;
    }
    Hide();
    if (!message.empty()) app_.Notify(name, message, warning);
}

// ---------------------------------------------------------------------------
// Detail view

void LauncherWindow::ShowDetail(const std::wstring& title, const std::wstring& text) {
    CancelComposition();
    if (mode_ != Mode::Detail) {
        detailReturnMode_ = mode_;
        detailReturnQuery_ = edit_.Text();
        detailReturnResults_ = std::move(results_);
        detailReturnSelected_ = selected_;
        detailReturnScroll_ = scroll_;
    }
    mode_ = Mode::Detail;
    detailTitle_ = title;
    detailText_ = text.size() > kMaxDetailChars ? text.substr(0, kMaxDetailChars) + L"\n…（內容太長，已截斷）" : text;
    detailScroll_ = 0;
    detailLayout_.Reset();
    edit_.SetText(L"");
    inputScroll_ = 0;
    actionsOpen_ = false;
    results_.clear();
    results_.push_back(MakeDetailItem());
    BuildEntries();
    selected_ = 0;
    scroll_ = 0;
    Disarm();
    RequestRender();
}

ResultItem LauncherWindow::MakeDetailItem() const {
    ResultItem item;
    item.title = detailTitle_;
    const std::wstring text = detailText_;
    item.actions.push_back(Action{L"複製全部", glyph::kCopy, Keys(VK_RETURN), false,
                                  [text](LauncherHost& host) {
                                      CopyToClipboard(host.Window(), text);
                                      return Outcome::Hide;
                                  },
                                  Keys('C', true, true)});
    return item;
}

void LauncherWindow::EnsureDetailLayout() {
    const D2D1_RECT_F list = ListRect();
    const float width = (list.right - list.left) - 2 * kDetailPadding - 8;
    if (detailLayout_ && detailLayoutWidth_ == width) return;
    detailLayout_ = renderer_.Layout(detailText_, Font::Mono, width, 1.0e7f);
    detailLayoutWidth_ = width;
    detailHeight_ = 0;
    DWRITE_TEXT_METRICS m{};
    if (detailLayout_ && SUCCEEDED(detailLayout_->GetMetrics(&m))) detailHeight_ = m.height;
}

void LauncherWindow::ScrollDetail(float delta) {
    EnsureDetailLayout();
    const D2D1_RECT_F list = ListRect();
    const float view = list.bottom - list.top;
    const float maxScroll = std::max(0.0f, detailHeight_ + 2 * kDetailTop - view);
    const float next = std::clamp(detailScroll_ + delta, 0.0f, maxScroll);
    if (next == detailScroll_) return;
    detailScroll_ = next;
    RequestRender();
}

// ---------------------------------------------------------------------------
// Keyboard & IME

bool LauncherWindow::OnKeyDown(UINT vk, bool systemKey) {
    if (vk == VK_PROCESSKEY) return false;  // the IME is composing
    if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN) return true;
    const bool ctrl = GetKeyState(VK_CONTROL) < 0;
    const bool shift = GetKeyState(VK_SHIFT) < 0;
    const bool alt = GetKeyState(VK_MENU) < 0;
    const bool plain = !ctrl && !shift && !alt;
    if (systemKey && vk == VK_F4) return false;  // Alt+F4 -> WM_CLOSE -> hide
    caretOn_ = true;

    if (actionsOpen_) {
        const int count = selected_ >= 0 ? static_cast<int>(results_[selected_].actions.size()) : 0;
        switch (vk) {
            case VK_ESCAPE:
                CloseActions();
                return true;
            case VK_UP:
                actionSelected_ = std::max(0, actionSelected_ - 1);
                RequestRender();
                return true;
            case VK_DOWN:
                actionSelected_ = std::min(count - 1, actionSelected_ + 1);
                RequestRender();
                return true;
            case VK_RETURN:
                if (plain) {
                    RunAction(selected_, actionSelected_);
                    return true;
                }
                break;
            case 'K':
                if (ctrl && !shift && !alt) {
                    CloseActions();
                    return true;
                }
                break;
            case VK_TAB:
                return true;
            default:
                break;
        }
        if (TryShortcut(vk, ctrl, shift, alt)) return true;
        CloseActions();  // anything else goes back to typing
    }

    if (mode_ == Mode::Detail) return OnDetailKey(vk, ctrl, shift, alt);

    switch (vk) {
        case VK_ESCAPE:
            if (mode_ == Mode::Windows) SwitchMode(Mode::Root);
            else if (!edit_.Text().empty()) SetQuery(L"");
            else if (mode_ == Mode::Plugin) BackToRoot(L"");
            else Hide();
            return true;
        case VK_UP:
        case VK_DOWN:
            if (ctrl || alt) break;  // Ctrl+Alt+Up/Down reorder favorites (action shortcuts)
            Move(vk == VK_UP ? -1 : 1);
            return true;
        case VK_PRIOR:
        case VK_NEXT: {
            const D2D1_RECT_F list = ListRect();
            const int page = std::max(1, static_cast<int>((list.bottom - list.top) / kRowHeight) - 1);
            Move(vk == VK_PRIOR ? -page : page);
            return true;
        }
        case VK_TAB:
            if (selected_ >= 0 && !results_[selected_].autocomplete.empty()) SetQuery(results_[selected_].autocomplete);
            return true;
        case 'K':
            if (ctrl && !shift && !alt) {
                OpenActions();
                return true;
            }
            break;
        case 'N':
        case 'P':
            if (ctrl && !shift && !alt) {
                Move(vk == 'N' ? 1 : -1);
                return true;
            }
            break;
        case VK_RETURN:
            if (plain) {
                RunAction(selected_, 0);
                return true;
            }
            break;
        case VK_BACK:
            if (edit_.Text().empty() && composition_.empty() && mode_ != Mode::Root) {
                GoBack();  // Plugin: back to "keyword" in the root search
                return true;
            }
            break;
        default:
            break;
    }
    if (TryShortcut(vk, ctrl, shift, alt)) return true;

    switch (vk) {
        case VK_LEFT:
            edit_.MoveLeft(ctrl, shift);
            break;
        case VK_RIGHT:
            edit_.MoveRight(ctrl, shift);
            break;
        case VK_HOME:
            edit_.Home(shift);
            break;
        case VK_END:
            edit_.End(shift);
            break;
        case VK_BACK:
            if (edit_.Backspace(ctrl)) OnTextChanged();
            return true;
        case VK_DELETE:
            if (edit_.Delete(ctrl)) OnTextChanged();
            return true;
        case 'A':
            if (!ctrl) return false;
            edit_.SelectAll();
            break;
        case 'C':
        case 'X':
            if (!ctrl) return false;
            if (edit_.HasSelection()) {
                CopyToClipboard(hwnd_, edit_.SelectedText());
                if (vk == 'X' && edit_.DeleteSelection()) OnTextChanged();
            }
            return true;
        case 'V':
            if (!ctrl) return false;
            edit_.Insert(ReadClipboardText(hwnd_));
            OnTextChanged();
            return true;
        default:
            return false;
    }
    RequestRender();
    return true;
}

bool LauncherWindow::OnDetailKey(UINT vk, bool ctrl, bool shift, bool alt) {
    const D2D1_RECT_F list = ListRect();
    const float page = (list.bottom - list.top) - kDetailStep;
    switch (vk) {
        case VK_ESCAPE:
        case VK_BACK:
            BackFromDetail();
            return true;
        case VK_UP:
            ScrollDetail(-kDetailStep);
            return true;
        case VK_DOWN:
            ScrollDetail(kDetailStep);
            return true;
        case VK_PRIOR:
            ScrollDetail(-page);
            return true;
        case VK_NEXT:
            ScrollDetail(page);
            return true;
        case VK_SPACE:
            ScrollDetail(shift ? -page : page);
            return true;
        case VK_HOME:
            ScrollDetail(-1.0e9f);
            return true;
        case VK_END:
            ScrollDetail(1.0e9f);
            return true;
        case VK_RETURN:
            if (!ctrl && !shift && !alt) {
                RunAction(0, 0);
                return true;
            }
            break;
        case 'K':
            if (ctrl && !shift && !alt) {
                OpenActions();
                return true;
            }
            break;
        default:
            break;
    }
    TryShortcut(vk, ctrl, shift, alt);
    return true;  // nothing is typed while reading
}

void LauncherWindow::OnChar(wchar_t ch) {
    if (ch < 0x20 || ch == 0x7F) return;  // control characters are handled in OnKeyDown
    if (mode_ == Mode::Detail) return;
    if (actionsOpen_) CloseActions();
    edit_.Insert(std::wstring(1, ch));
    OnTextChanged();
}

void LauncherWindow::OnImeComposition(LPARAM lp) {
    HIMC himc = ImmGetContext(hwnd_);
    if (!himc) return;
    if (mode_ == Mode::Detail) {  // no text input while reading
        ImmNotifyIME(himc, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
        ImmReleaseContext(hwnd_, himc);
        composition_.clear();
        return;
    }
    auto read = [&](DWORD kind) {
        const LONG bytes = ImmGetCompositionStringW(himc, kind, nullptr, 0);
        std::wstring s;
        if (bytes > 0) {
            s.resize(static_cast<size_t>(bytes) / sizeof(wchar_t));
            ImmGetCompositionStringW(himc, kind, s.data(), static_cast<DWORD>(bytes));
        }
        return s;
    };
    bool textChanged = false;
    if (lp & GCS_RESULTSTR) {
        const std::wstring result = read(GCS_RESULTSTR);
        composition_.clear();
        if (!result.empty()) {
            edit_.Insert(result);
            textChanged = true;
        }
    }
    // Re-read the live composition every time (empty means it ended or was cancelled).
    composition_ = read(GCS_COMPSTR);
    const LONG cursor = ImmGetCompositionStringW(himc, GCS_CURSORPOS, nullptr, 0);
    compositionCursor_ = std::min<size_t>(cursor < 0 ? composition_.size() : static_cast<size_t>(cursor),
                                          composition_.size());
    ImmReleaseContext(hwnd_, himc);
    if (textChanged) OnTextChanged();
    Render();  // synchronously, so the candidate window can follow the caret
    UpdateImeWindow();
}

void LauncherWindow::UpdateImeWindow() {
    HIMC himc = ImmGetContext(hwnd_);
    if (!himc) return;
    const float cy = FrameRect().top + kSearchHeight / 2;
    const LONG x = std::lround(caretX_ * scale_);
    const LONG top = std::lround((cy - 13) * scale_);
    const LONG bottom = std::lround((cy + 15) * scale_);
    SetCaretPos(x, top);  // TSF-based IMEs (e.g. Microsoft Bopomofo) follow the system caret
    COMPOSITIONFORM cf{};
    cf.dwStyle = CFS_POINT;
    cf.ptCurrentPos = {x, top};
    ImmSetCompositionWindow(himc, &cf);
    CANDIDATEFORM cand{};
    cand.dwIndex = 0;
    const LANGID lang = LOWORD(reinterpret_cast<UINT_PTR>(GetKeyboardLayout(0)));
    if (PRIMARYLANGID(lang) == LANG_CHINESE) {
        cand.dwStyle = CFS_CANDIDATEPOS;
        cand.ptCurrentPos = {x, bottom};
    } else {
        cand.dwStyle = CFS_EXCLUDE;
        cand.ptCurrentPos = {x, bottom};
        cand.rcArea = {x, top, x + 1, bottom};
    }
    ImmSetCandidateWindow(himc, &cand);
    ImmReleaseContext(hwnd_, himc);
}

// ---------------------------------------------------------------------------
// Mouse

size_t LauncherWindow::TextPositionAt(float x) const {
    const std::wstring& text = edit_.Text();
    if (text.empty()) return 0;
    const float target = x - (inputRect_.left - inputScroll_);
    auto layout = renderer_.Layout(text, Font::Input, 100000.0f, kSearchHeight);
    BOOL trailing = FALSE, inside = FALSE;
    DWRITE_HIT_TEST_METRICS m{};
    if (layout && SUCCEEDED(layout->HitTestPoint(target, kSearchHeight / 2, &trailing, &inside, &m)))
        return std::min<size_t>(text.size(), m.textPosition + (trailing ? m.length : 0));
    // Fallback: nearest caret position by measuring prefixes.
    size_t best = 0;
    float bestDistance = target < 0 ? -target : target;
    for (size_t pos = 1; pos <= text.size(); ++pos) {
        const float d = std::fabs(CaretOffset(renderer_, nullptr, text, pos) - target);
        if (d < bestDistance) {
            bestDistance = d;
            best = pos;
        }
    }
    return best;
}

int LauncherWindow::RowAt(float x, float y) const {
    if (mode_ == Mode::Detail) return -1;
    const D2D1_RECT_F list = ListRect();
    if (!Contains(list, x, y)) return -1;
    for (const auto& e : entries_) {
        const float top = list.top + e.top - scroll_;
        if (e.item >= 0 && y >= top && y < top + e.height) return e.item;
    }
    return -1;
}

int LauncherWindow::ActionAt(float x, float y) const {
    for (size_t i = 0; i < actionRects_.size(); ++i)
        if (Contains(actionRects_[i], x, y)) return static_cast<int>(i);
    return -1;
}

void LauncherWindow::OnMouseMove(float x, float y, bool leftDown) {
    if (draggingText_ && leftDown) {
        edit_.SetCaret(TextPositionAt(x), true);
        RequestRender();
        return;
    }
    if (actionsOpen_) {
        const int a = ActionAt(x, y);
        if (a >= 0 && a != actionSelected_) {
            actionSelected_ = a;
            RequestRender();
        }
        return;
    }
    const int row = RowAt(x, y);
    if (row >= 0) Select(row);
}

void LauncherWindow::OnLeftDown(float x, float y, bool doubleClick) {
    SetFocus(hwnd_);
    if (!Contains(FrameRect(), x, y)) {  // the transparent shadow margin
        Hide();
        return;
    }
    if (actionsOpen_) {
        pressedAction_ = ActionAt(x, y);
        if (pressedAction_ < 0 && !Contains(actionPanelRect_, x, y)) CloseActions();
        return;
    }
    if (!IsEmpty(chipRect_) && Contains(chipRect_, x, y)) {  // the chip doubles as a back button
        GoBack();
        return;
    }
    if (y < FrameRect().top + kSearchHeight) {
        if (mode_ == Mode::Detail) return;
        if (doubleClick) {
            edit_.SelectWordAt(TextPositionAt(x));
        } else {
            edit_.SetCaret(TextPositionAt(x), GetKeyState(VK_SHIFT) < 0);
            draggingText_ = true;
            SetCapture(hwnd_);
        }
        caretOn_ = true;
        RequestRender();
        return;
    }
    pressedRow_ = RowAt(x, y);
    if (pressedRow_ >= 0) Select(pressedRow_);
}

void LauncherWindow::OnLeftUp(float x, float y) {
    if (draggingText_) {
        draggingText_ = false;
        ReleaseCapture();
        return;
    }
    if (pressedAction_ >= 0) {
        const int pressed = std::exchange(pressedAction_, -1);
        if (ActionAt(x, y) == pressed) RunAction(selected_, pressed);
        return;
    }
    if (pressedRow_ >= 0) {
        const int pressed = std::exchange(pressedRow_, -1);
        if (RowAt(x, y) == pressed) RunAction(pressed, 0);
    }
}

// ---------------------------------------------------------------------------
// Window procedure

LRESULT LauncherWindow::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) {
                active_ = false;
                if (visible_ && !suppressAutoHide_) Hide();
            } else {
                active_ = true;
                caretOn_ = true;
                RequestRender();
            }
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (OnKeyDown(static_cast<UINT>(wp), msg == WM_SYSKEYDOWN)) return 0;
            break;
        case WM_CHAR:
            OnChar(static_cast<wchar_t>(wp));
            return 0;
        case WM_SYSCHAR:
        case WM_SYSKEYUP:
            return 0;  // no menu bar: swallow Alt+letter beeps and Alt menu activation
        case WM_IME_SETCONTEXT:
            lp &= ~ISC_SHOWUICOMPOSITIONWINDOW;  // we draw the composition inline
            return DefWindowProcW(hwnd_, msg, wp, lp);
        case WM_IME_STARTCOMPOSITION:
            if (mode_ != Mode::Detail && edit_.DeleteSelection()) OnTextChanged();
            composition_.clear();
            UpdateImeWindow();
            return 0;
        case WM_IME_COMPOSITION:
            OnImeComposition(lp);
            return 0;
        case WM_IME_ENDCOMPOSITION:
            composition_.clear();
            RequestRender();
            return 0;
        case WM_MOUSEMOVE: {
            const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (pt.x == lastMouse_.x && pt.y == lastMouse_.y) return 0;  // synthetic moves when the list changes
            lastMouse_ = pt;
            OnMouseMove(pt.x / scale_, pt.y / scale_, (wp & MK_LBUTTON) != 0);
            return 0;
        }
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            OnLeftDown(GET_X_LPARAM(lp) / scale_, GET_Y_LPARAM(lp) / scale_, msg == WM_LBUTTONDBLCLK);
            return 0;
        case WM_LBUTTONUP:
            OnLeftUp(GET_X_LPARAM(lp) / scale_, GET_Y_LPARAM(lp) / scale_);
            return 0;
        case WM_RBUTTONUP: {
            const int row = RowAt(GET_X_LPARAM(lp) / scale_, GET_Y_LPARAM(lp) / scale_);
            if (row >= 0) {
                Select(row);
                OpenActions();
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            if (actionsOpen_) return 0;
            const float delta = GET_WHEEL_DELTA_WPARAM(wp) / static_cast<float>(WHEEL_DELTA);
            if (mode_ == Mode::Detail) {
                ScrollDetail(-delta * kDetailStep * 2);
                return 0;
            }
            const D2D1_RECT_F list = ListRect();
            scroll_ = std::clamp(scroll_ - delta * kRowHeight * 2, 0.0f,
                                 std::max(0.0f, contentHeight_ - (list.bottom - list.top)));
            RequestRender();
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                POINT pt{};
                GetCursorPos(&pt);
                ScreenToClient(hwnd_, &pt);
                const float x = pt.x / scale_, y = pt.y / scale_;
                LPCWSTR cursor = IDC_ARROW;
                if (!actionsOpen_ && !IsEmpty(chipRect_) && Contains(chipRect_, x, y)) cursor = IDC_HAND;
                else if (!actionsOpen_ && mode_ != Mode::Detail && Contains(inputRect_, x, y)) cursor = IDC_IBEAM;
                SetCursor(LoadCursorW(nullptr, cursor));
                return TRUE;
            }
            break;
        case WM_TIMER:
            switch (wp) {
                case kCaretTimer:
                    caretOn_ = !caretOn_;
                    if (visible_ && mode_ != Mode::Detail) Render();
                    break;
                case kStatusTimer:
                    KillTimer(hwnd_, kStatusTimer);
                    status_.clear();
                    statusWarning_ = false;
                    Disarm();
                    RequestRender();
                    break;
                case kWindowsTimer:
                    KillTimer(hwnd_, kWindowsTimer);
                    windows_ = EnumerateWindows();
                    RefreshResults(true);
                    RequestRender();
                    break;
                case kPluginTimer:
                    KillTimer(hwnd_, kPluginTimer);
                    if (mode_ == Mode::Plugin) StartPluginQuery(edit_.Text());
                    break;
                case kBusyTimer:
                    KillTimer(hwnd_, kBusyTimer);
                    if (pluginBusy_) {
                        busyShown_ = true;
                        RequestRender();
                    }
                    break;
                default:
                    break;
            }
            return 0;
        case WM_APP_PLUGIN_RESULT:
            HandlePluginResult(std::unique_ptr<PluginResult>(reinterpret_cast<PluginResult*>(lp)));
            return 0;
        case WM_APP_RENDER:
            renderPending_ = false;
            if (visible_) Render();
            return 0;
        case WM_APP_ICONS:
            app_.icons().AcknowledgeNotification();
            if (visible_) Render();
            return 0;
        case WM_DPICHANGED:
            scale_ = HIWORD(wp) / 96.0f;  // we position ourselves; ignore the suggested rect
            detailLayout_.Reset();
            if (visible_) Render();
            return 0;
        case WM_DISPLAYCHANGE:
            if (visible_) {
                PositionOnCursorMonitor();
                Render();
            }
            return 0;
        case WM_CLOSE:
            Hide();
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Drawing

void LauncherWindow::RequestRender() {
    if (!visible_ || renderPending_ || !hwnd_) return;
    renderPending_ = true;
    PostMessageW(hwnd_, WM_APP_RENDER, 0, 0);
}

void LauncherWindow::Render() {
    if (!hwnd_) return;
    for (int attempt = 0; attempt < 2; ++attempt) {  // second try after a lost device
        if (!renderer_.Begin(PixelWidth(), PixelHeight(), 96.0f * scale_)) return;
        Draw();
        if (renderer_.End(hwnd_, position_)) return;
    }
}

void LauncherWindow::Draw() {
    const Palette& p = app_.palette();
    const D2D1_RECT_F frame = FrameRect();
    renderer_.Shadow(frame, kRadius, 22.0f, p.dark ? 0xFF000000 : 0x80000000);
    renderer_.FillRoundRect(frame, kRadius, p.background);
    renderer_.PushRoundClip(frame, kRadius);
    DrawSearch(p, frame);
    const float sep = frame.top + kSearchHeight + 0.5f;
    renderer_.Line({frame.left, sep}, {frame.right, sep}, p.separator);
    if (mode_ == Mode::Detail) DrawDetail(p);
    else DrawList(p);
    DrawFooter(p);
    if (actionsOpen_) DrawActions(p);
    renderer_.PopRoundClip();
    renderer_.StrokeRoundRect(frame, kRadius, p.border);
}

std::wstring LauncherWindow::ChipText() const {
    switch (mode_) {
        case Mode::Windows:
            return L"切換視窗";
        case Mode::Plugin: {
            const PluginCommand* cmd = ActivePlugin();
            return cmd ? cmd->spec.name : std::wstring(L"外掛");
        }
        case Mode::Detail:
            return detailTitle_.empty() ? std::wstring(L"詳細內容") : detailTitle_;
        default:
            return L"";
    }
}

std::wstring LauncherWindow::Placeholder() const {
    switch (mode_) {
        case Mode::Windows:
            return L"搜尋已開啟的視窗…";
        case Mode::Plugin:
            if (!pluginPlaceholder_.empty()) return pluginPlaceholder_;
            return L"在「" + ChipText() + L"」中搜尋…";
        case Mode::Detail:
            return L"按 Esc 返回";
        default:
            return L"搜尋應用程式、視窗、指令與外掛…";
    }
}

void LauncherWindow::DrawSearch(const Palette& p, const D2D1_RECT_F& frame) {
    float left = frame.left + 20.0f;
    const float cy = frame.top + kSearchHeight / 2;
    chipRect_ = {};
    const std::wstring chip = ChipText();
    if (!chip.empty()) {
        const float glyphWidth = 14.0f;
        const float textWidth = std::min(renderer_.TextWidth(chip, Font::Small) + 1, 220.0f);
        const D2D1_RECT_F r{left, cy - 12, left + 8 + glyphWidth + 4 + textWidth + 10, cy + 12};
        renderer_.FillRoundRect(r, 6, p.chip);
        renderer_.Text(glyph::kBack, {r.left + 8, r.top, r.left + 8 + glyphWidth, r.bottom}, Font::Glyph, p.chipText,
                       DWRITE_TEXT_ALIGNMENT_CENTER);
        renderer_.Text(chip, {r.left + 8 + glyphWidth + 4, r.top, r.right - 8, r.bottom}, Font::Small, p.chipText);
        chipRect_ = r;
        left = r.right + 10;
    }
    inputRect_ = {left, frame.top, frame.right - 20.0f, frame.top + kSearchHeight};
    const float width = inputRect_.right - inputRect_.left;

    std::wstring display = edit_.Text();
    size_t caret = edit_.Caret();
    const bool composing = !composition_.empty();
    if (composing) {
        display.insert(edit_.Caret(), composition_);
        caret = edit_.Caret() + compositionCursor_;
    }

    if (display.empty()) {
        inputScroll_ = 0;
        renderer_.Text(Placeholder(), inputRect_, Font::Input, p.textTertiary);
        caretX_ = inputRect_.left;
    } else {
        auto layout = renderer_.Layout(display, Font::Input, 100000.0f, kSearchHeight);
        if (!layout) return;
        DWRITE_TEXT_METRICS tm{};
        layout->GetMetrics(&tm);
        const float cx = CaretOffset(renderer_, layout.Get(), display, caret);
        const float margin = 4.0f;
        if (cx - inputScroll_ > width - margin) inputScroll_ = cx - width + margin;
        if (cx - inputScroll_ < 0) inputScroll_ = cx;
        inputScroll_ = std::clamp(inputScroll_, 0.0f, std::max(0.0f, tm.widthIncludingTrailingWhitespace - width + margin));
        const D2D1_POINT_2F origin{inputRect_.left - inputScroll_, inputRect_.top};
        renderer_.PushClip(inputRect_);
        if (!composing && edit_.HasSelection()) {
            const float x0 = origin.x + CaretOffset(renderer_, layout.Get(), display, edit_.SelStart());
            const float x1 = origin.x + CaretOffset(renderer_, layout.Get(), display, edit_.SelEnd());
            renderer_.FillRect({x0, cy - 13, x1, cy + 13}, p.textSelection);
        }
        if (composing) {
            const DWRITE_TEXT_RANGE range{static_cast<UINT32>(edit_.Caret()), static_cast<UINT32>(composition_.size())};
            layout->SetUnderline(TRUE, range);
        }
        renderer_.rt()->DrawTextLayout(origin, layout.Get(), renderer_.Brush(p.text));
        renderer_.PopClip();
        caretX_ = origin.x + cx;
    }
    if (mode_ != Mode::Detail && caretOn_ && active_ && (composing || !edit_.HasSelection()))
        renderer_.FillRect({caretX_, cy - 11, caretX_ + 1.5f, cy + 11}, p.caret);
}

void LauncherWindow::DrawList(const Palette& p) {
    const D2D1_RECT_F list = ListRect();
    renderer_.PushClip(list);
    if (results_.empty()) {
        std::wstring text = L"沒有結果";
        if (mode_ == Mode::Windows) text = L"找不到符合的視窗";
        else if (mode_ == Mode::Plugin && pluginBusy_) text = busyShown_ ? L"載入中…" : L"";
        else if (mode_ == Mode::Plugin && pluginSilent_) text = L"外掛沒有輸出任何內容（可在「外掛偵錯紀錄」查看 stderr）";
        else if (mode_ == Mode::Root && app_.indexing()) text = L"正在建立應用程式索引…";
        renderer_.Text(text, list, Font::Title, p.textTertiary, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    for (const auto& e : entries_) {
        const float top = list.top + e.top - scroll_;
        const float bottom = top + e.height;
        if (bottom < list.top || top > list.bottom) continue;
        if (e.item < 0) {
            renderer_.Text(e.header, {list.left + 20, top + 6, list.right - 20, bottom}, Font::SmallBold,
                           p.textTertiary);
        } else {
            DrawRow(p, results_[e.item], {list.left + 8, top, list.right - 8, bottom}, e.item == selected_);
        }
    }
    renderer_.PopClip();
}

void LauncherWindow::DrawDetail(const Palette& p) {
    const D2D1_RECT_F list = ListRect();
    renderer_.PushClip(list);
    EnsureDetailLayout();
    if (Trim(detailText_).empty()) {
        renderer_.Text(L"（沒有內容）", list, Font::Title, p.textTertiary, DWRITE_TEXT_ALIGNMENT_CENTER);
    } else if (detailLayout_) {
        const float view = list.bottom - list.top;
        const float content = detailHeight_ + 2 * kDetailTop;
        detailScroll_ = std::clamp(detailScroll_, 0.0f, std::max(0.0f, content - view));
        const D2D1_POINT_2F origin{list.left + kDetailPadding, list.top + kDetailTop - detailScroll_};
        renderer_.rt()->DrawTextLayout(origin, detailLayout_.Get(), renderer_.Brush(p.text));
        if (content > view) {  // scroll indicator
            const float trackTop = list.top + 4, track = view - 8;
            const float thumb = std::max(24.0f, track * view / content);
            const float t = detailScroll_ / (content - view);
            const float top = trackTop + (track - thumb) * t;
            renderer_.FillRoundRect({list.right - 9, top, list.right - 5, top + thumb}, 2, p.keycap);
        }
    }
    renderer_.PopClip();
}

void LauncherWindow::DrawRow(const Palette& p, const ResultItem& item, const D2D1_RECT_F& row, bool selected) {
    if (selected) renderer_.FillRoundRect(row, 8, p.selection);
    const float cy = (row.top + row.bottom) / 2;
    DrawIcon(p, item.icon, {row.left + 12, cy - 12, row.left + 36, cy + 12});
    float right = row.right - 14;
    if (!item.accessory.empty()) {
        const float aw = std::min(renderer_.TextWidth(item.accessory, Font::Small), 200.0f);
        renderer_.Text(item.accessory, {right - aw - 2, row.top, right, row.bottom}, Font::Small, p.textTertiary,
                       DWRITE_TEXT_ALIGNMENT_TRAILING);
        right -= aw + 18;
    }
    const float x = row.left + 48;
    if (right <= x) return;
    const float titleWidth = std::min(renderer_.TextWidth(item.title, Font::Title) + 1, right - x);
    renderer_.Text(item.title, {x, row.top, x + titleWidth, row.bottom}, Font::Title, p.text);
    const float sx = x + titleWidth + 10;
    if (!item.subtitle.empty() && right - sx > 40)
        renderer_.Text(item.subtitle, {sx, row.top, right, row.bottom}, Font::Subtitle, p.textSecondary);
}

void LauncherWindow::DrawIcon(const Palette& p, const ItemIcon& icon, const D2D1_RECT_F& r) {
    if (icon.type == IconType::Glyph) {
        renderer_.FillRoundRect(r, 6, icon.tint);
        renderer_.Text(icon.value, r, Font::Glyph, 0xFFFFFFFF, DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }
    if (icon.type == IconType::Shell) {
        if (auto pixels = app_.icons().Get(icon.value)) {
            renderer_.DrawIcon(icon.value, pixels, r);
            return;
        }
    }
    renderer_.FillRoundRect(r, 6, p.selection);  // placeholder while the icon loads
    renderer_.Text(glyph::kApps, r, Font::Glyph, p.textTertiary, DWRITE_TEXT_ALIGNMENT_CENTER);
}

float LauncherWindow::DrawKeycaps(const Palette& p, const std::vector<std::wstring>& keys, float right, float cy) {
    for (auto it = keys.rbegin(); it != keys.rend(); ++it) {
        const float w = std::max(20.0f, renderer_.TextWidth(*it, Font::Keycap) + 10.0f);
        const D2D1_RECT_F r{right - w, cy - 9, right, cy + 9};
        renderer_.FillRoundRect(r, 4, p.keycap);
        renderer_.Text(*it, r, Font::Keycap, p.keycapText, DWRITE_TEXT_ALIGNMENT_CENTER);
        right -= w + 4;
    }
    return right + 4;
}

void LauncherWindow::DrawFooter(const Palette& p) {
    const D2D1_RECT_F f = FooterRect();
    renderer_.FillRect(f, p.footer);
    renderer_.Line({f.left, f.top + 0.5f}, {f.right, f.top + 0.5f}, p.separator);
    const float cy = (f.top + f.bottom) / 2;

    const D2D1_RECT_F mark{f.left + 16, cy - 8, f.left + 32, cy + 8};
    renderer_.FillRoundRect(mark, 4, p.accent);
    renderer_.Line({mark.left + 4.5f, mark.bottom - 4.5f}, {mark.right - 4.5f, mark.top + 4.5f}, 0xFFFFFFFF, 2.0f);
    const bool custom = !status_.empty();
    renderer_.Text(custom ? status_ : DefaultStatus(), {mark.right + 10, f.top, f.right - 320, f.bottom}, Font::Small,
                   custom && statusWarning_ ? p.warning : p.textSecondary);

    float x = DrawKeycaps(p, {L"Ctrl", L"K"}, f.right - 16, cy) - 8;
    float w = renderer_.TextWidth(L"動作", Font::Small);
    renderer_.Text(L"動作", {x - w - 1, f.top, x, f.bottom}, Font::Small, p.textSecondary,
                   DWRITE_TEXT_ALIGNMENT_TRAILING);
    x -= w + 14;
    if (selected_ >= 0 && selected_ < static_cast<int>(results_.size()) && !results_[selected_].actions.empty()) {
        renderer_.Line({x, cy - 8}, {x, cy + 8}, p.separator);
        x = DrawKeycaps(p, {L"Enter"}, x - 14, cy) - 8;
        const std::wstring& primary = results_[selected_].actions[0].title;
        w = std::min(renderer_.TextWidth(primary, Font::Small) + 1, 200.0f);
        renderer_.Text(primary, {x - w, f.top, x, f.bottom}, Font::Small, p.text, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
}

void LauncherWindow::DrawActions(const Palette& p) {
    actionRects_.clear();
    if (selected_ < 0 || selected_ >= static_cast<int>(results_.size())) return;
    const ResultItem& item = results_[selected_];
    const D2D1_RECT_F list = ListRect();
    const float width = 330, rowHeight = 34, header = 30;
    const float height = header + rowHeight * item.actions.size() + 8;
    D2D1_RECT_F panel{list.right - 12 - width, list.bottom - 10 - height, list.right - 12, list.bottom - 10};
    panel.top = std::max(panel.top, list.top + 8);
    actionPanelRect_ = panel;
    renderer_.Shadow(panel, 10, 12, p.dark ? 0xFF000000 : 0x60000000);
    renderer_.FillRoundRect(panel, 10, p.panel);
    renderer_.StrokeRoundRect(panel, 10, p.panelBorder);
    renderer_.Text(item.title, {panel.left + 14, panel.top + 4, panel.right - 14, panel.top + header}, Font::SmallBold,
                   p.textTertiary);
    float y = panel.top + header;
    for (size_t i = 0; i < item.actions.size(); ++i) {
        const Action& a = item.actions[i];
        const D2D1_RECT_F row{panel.left + 6, y, panel.right - 6, y + rowHeight};
        if (row.bottom > panel.bottom) break;
        actionRects_.push_back(row);
        if (static_cast<int>(i) == actionSelected_) renderer_.FillRoundRect(row, 6, p.selection);
        const float cy = (row.top + row.bottom) / 2;
        renderer_.Text(a.glyph, {row.left + 8, row.top, row.left + 28, row.bottom}, Font::Glyph,
                       a.dangerous ? p.warning : p.textSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
        const std::vector<std::wstring> keys = i == 0 ? std::vector<std::wstring>{L"Enter"} : a.shortcut.Parts();
        const float keysLeft = keys.empty() ? row.right - 8 : DrawKeycaps(p, keys, row.right - 8, cy) - 8;
        renderer_.Text(a.title, {row.left + 36, row.top, keysLeft, row.bottom}, Font::Title, p.text);
        y += rowHeight;
    }
}

}  // namespace beam
