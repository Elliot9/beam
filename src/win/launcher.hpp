// The Raycast-style launcher: a borderless, per-pixel-alpha popup with a
// search field, a ranked result list, an action panel (Ctrl+K) and a footer.
//
// Views: Root (everything), Windows (window switcher), Plugin (one plugin's
// results, filled asynchronously) and Detail (scrollable text output).
#pragma once

#include "win/common.hpp"

#include <memory>
#include <string>
#include <vector>

#include "core/edit_buffer.hpp"
#include "plugin/plugin_manager.hpp"
#include "win/renderer.hpp"
#include "win/results.hpp"
#include "win/winlist.hpp"

namespace beam {

class App;

class LauncherWindow final : public LauncherHost {
public:
    explicit LauncherWindow(App& app);
    ~LauncherWindow() override;

    bool Create(HINSTANCE instance);
    void Show(Mode mode);
    void Toggle(Mode mode);
    void Hide();
    bool IsShown() const { return visible_; }
    bool IsActive() const { return visible_ && GetForegroundWindow() == hwnd_; }
    HWND hwnd() const { return hwnd_; }

    void OnIndexChanged();
    void OnPluginsChanged();
    void OnThemeChanged() { RequestRender(); }

    // LauncherHost
    void SetQuery(const std::wstring& text) override;
    void EnterMode(Mode mode) override;
    void ShowStatus(const std::wstring& message, bool warning) override;
    void RefreshWindows() override;
    void Refresh(const std::wstring& selectUsageKey) override;
    void EnterPlugin(const std::wstring& commandId, const std::wstring& query) override;
    void RunPluginCommand(const std::wstring& commandId, const std::wstring& argument) override;
    void PluginCallback(const std::wstring& commandId, const Json& item, const Json& action) override;
    void ShowDetail(const std::wstring& title, const std::wstring& text) override;
    HWND Window() const override { return hwnd_; }

private:
    struct ListEntry {
        float top = 0;
        float height = 0;
        int item = -1;  // -1: section header
        std::wstring header;
    };

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    // geometry (DIPs)
    D2D1_RECT_F FrameRect() const;
    D2D1_RECT_F ListRect() const;
    D2D1_RECT_F FooterRect() const;
    int PixelWidth() const;
    int PixelHeight() const;
    void PositionOnCursorMonitor();
    void ForceForeground();

    // state
    void RefreshResults(bool keepSelection = false);
    void BuildEntries();
    void OnTextChanged();
    void SwitchMode(Mode mode);
    void LeaveSubViews();
    void BackToRoot(std::wstring query);  // by value: callers pass members that LeaveSubViews clears
    void BackFromDetail();
    void GoBack();
    void Select(int index);
    void Move(int delta);
    void EnsureVisible(int index);
    void RunAction(int itemIndex, int actionIndex);
    bool TryShortcut(UINT vk, bool ctrl, bool shift, bool alt);
    void OpenActions();
    void CloseActions();
    void Disarm() { armedKey_.clear(); }
    void CancelComposition();
    std::wstring DefaultStatus() const;
    void Report(const std::wstring& title, const std::wstring& message, bool warning);

    // plugins
    PluginResultSink ResultSink() const;  // posts finished runs to this window
    const PluginCommand* ActivePlugin() const;
    bool TryEnterPluginByKeyword();
    void EnterPluginScope(const PluginCommand& cmd, const std::wstring& query, const std::wstring& returnQuery);
    void OnPluginQueryChanged();
    void StartPluginQuery(const std::wstring& query);
    void HandlePluginResult(std::unique_ptr<PluginResult> result);
    void ApplyScopeResult(const PluginResult& result);
    void ApplyCommandResult(const PluginResult& result);
    void ShowPluginItems(const PluginCommand& cmd, const PluginResponse& response);
    std::vector<ResultItem> PluginView(const std::wstring& query) const;
    std::vector<ResultItem> FilterLocal(const std::wstring& query) const;

    // detail view
    ResultItem MakeDetailItem() const;
    void EnsureDetailLayout();
    void ScrollDetail(float delta);

    // input
    bool OnKeyDown(UINT vk, bool systemKey);
    bool OnDetailKey(UINT vk, bool ctrl, bool shift, bool alt);
    void OnChar(wchar_t ch);
    void OnImeComposition(LPARAM lp);
    void UpdateImeWindow();
    size_t TextPositionAt(float x) const;
    void OnMouseMove(float x, float y, bool leftDown);
    void OnLeftDown(float x, float y, bool doubleClick);
    void OnLeftUp(float x, float y);
    int RowAt(float x, float y) const;
    int ActionAt(float x, float y) const;

    // drawing
    void RequestRender();
    void Render();
    void Draw();
    void DrawSearch(const Palette& p, const D2D1_RECT_F& frame);
    void DrawList(const Palette& p);
    void DrawDetail(const Palette& p);
    void DrawRow(const Palette& p, const ResultItem& item, const D2D1_RECT_F& row, bool selected);
    void DrawIcon(const Palette& p, const ItemIcon& icon, const D2D1_RECT_F& rect);
    void DrawFooter(const Palette& p);
    void DrawActions(const Palette& p);
    float DrawKeycaps(const Palette& p, const std::vector<std::wstring>& keys, float right, float centerY);
    std::wstring ChipText() const;
    std::wstring Placeholder() const;

    App& app_;
    HWND hwnd_ = nullptr;
    Renderer renderer_;
    float scale_ = 1.0f;
    POINT position_{};
    bool visible_ = false;
    bool active_ = false;
    bool renderPending_ = false;
    bool suppressAutoHide_ = false;
    ULONGLONG hiddenAt_ = 0;

    Mode mode_ = Mode::Root;
    EditBuffer edit_;
    std::wstring composition_;
    size_t compositionCursor_ = 0;
    float inputScroll_ = 0;
    D2D1_RECT_F inputRect_{};
    D2D1_RECT_F chipRect_{};  // the "back" chip left of the search field (empty in Root)
    float caretX_ = 0;
    bool caretOn_ = true;

    std::vector<WindowInfo> windows_;
    std::vector<ResultItem> results_;
    std::vector<ListEntry> entries_;
    float contentHeight_ = 0;
    float scroll_ = 0;
    int selected_ = -1;
    bool actionsOpen_ = false;
    int actionSelected_ = 0;
    std::vector<D2D1_RECT_F> actionRects_;
    D2D1_RECT_F actionPanelRect_{};

    // plugin view
    std::wstring pluginId_;
    std::wstring pluginReturnQuery_;  // root text restored by Backspace on an empty field
    std::wstring pluginPlaceholder_;
    std::wstring lastPluginQuery_;
    bool pluginLocal_ = false;   // items fetched once and filtered by Beam
    bool pluginFailed_ = false;  // pluginAll_ holds the error item
    bool pluginSilent_ = false;  // the last query printed nothing at all (likely a plugin bug)
    std::vector<ResultItem> pluginAll_;
    uint64_t pluginTicket_ = 0;  // latest query run
    bool pluginBusy_ = false;
    bool busyShown_ = false;
    uint64_t commandTicket_ = 0;  // latest command / callback run
    std::wstring commandBusyName_;

    // detail view
    std::wstring detailTitle_;
    std::wstring detailText_;
    float detailScroll_ = 0;
    float detailHeight_ = 0;
    float detailLayoutWidth_ = 0;
    ComPtr<IDWriteTextLayout> detailLayout_;
    Mode detailReturnMode_ = Mode::Root;
    std::wstring detailReturnQuery_;
    std::vector<ResultItem> detailReturnResults_;
    int detailReturnSelected_ = -1;
    float detailReturnScroll_ = 0;

    std::wstring status_;
    bool statusWarning_ = false;
    std::wstring armedKey_;
    ULONGLONG armedAt_ = 0;

    POINT lastMouse_{-1, -1};
    int pressedRow_ = -1;
    int pressedAction_ = -1;
    bool draggingText_ = false;
};

}  // namespace beam
