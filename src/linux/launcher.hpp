// The Raycast-style launcher on Linux (GTK 4): a search field, a ranked result list,
// an action panel (Ctrl+K) and a footer, inside a transparent maximized window so the
// panel can sit at the same place as on Windows (Wayland does not let clients position
// their windows). Clicking outside the panel or switching away hides it.
//
// Views: Root (everything), Windows (window switcher), Plugin (one plugin's results,
// filled asynchronously) and Detail (scrollable text output). The state machine is the
// same as the Windows launcher (src/win/launcher.cpp).
#pragma once

#include <gtk/gtk.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "linux/results.hpp"
#include "linux/search.hpp"
#include "plugin/plugin_manager.hpp"

namespace beam {

class App;

class LauncherWindow final : public LauncherHost {
public:
    explicit LauncherWindow(App& app);
    ~LauncherWindow() override;
    LauncherWindow(const LauncherWindow&) = delete;
    LauncherWindow& operator=(const LauncherWindow&) = delete;

    void Create(GtkApplication* application);
    void Show(Mode mode);
    void Toggle(Mode mode);
    void Hide();
    bool IsShown() const { return visible_; }
    bool IsActive() const;

    void OnIndexChanged();
    void OnPluginsChanged();
    void OnThemeChanged();

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

    // Test hooks (BEAM_TEST_*): what the list currently shows.
    std::wstring DescribeState() const;

private:
    struct ListEntry {
        double top = 0;
        double height = 0;
        int item = -1;  // -1: section header
        std::wstring header;
    };
    struct SinkState {
        LauncherWindow* launcher = nullptr;
    };

    void BuildWidgets(GtkApplication* application);
    void LoadCss();

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
    bool TryShortcut(guint keyval, bool ctrl, bool shift, bool alt);
    void OpenActions();
    void CloseActions();
    void Disarm() { armedKey_.clear(); }
    std::wstring DefaultStatus() const;
    void Report(const std::wstring& title, const std::wstring& message, bool warning);
    void FetchWindows();
    std::wstring Text() const;
    void SetText(const std::wstring& text, bool selectAll = false);  // no change notification
    void EnsureFocus(int attempt);
    void OnFocusChanged();
    void DisconnectSurface();

    // plugins
    PluginResultSink ResultSink() const;
    void HandlePluginResult(std::unique_ptr<PluginResult> result);
    const PluginCommand* ActivePlugin() const;
    bool TryEnterPluginByKeyword();
    const PluginCommand* KeywordPlugin(std::wstring* keyword, std::wstring* rest) const;
    void FinishPendingScope();
    void EnterPluginScope(const PluginCommand& cmd, const std::wstring& query, const std::wstring& returnQuery);
    void OnPluginQueryChanged();
    void StartPluginQuery(const std::wstring& query);
    void ApplyScopeResult(const PluginResult& result);
    void ApplyCommandResult(const PluginResult& result);
    void ShowPluginItems(const PluginCommand& cmd, const PluginResponse& response);
    std::vector<ResultItem> PluginView(const std::wstring& query) const;
    std::vector<ResultItem> FilterLocal(const std::wstring& query) const;

    // detail view
    ResultItem MakeDetailItem() const;
    void ScrollDetail(double delta);

    // input
    bool OnKeyPressed(guint keyval, guint baseKeyval, GdkModifierType state);
    bool OnDetailKey(guint keyval, bool ctrl, bool shift, bool alt);
    int RowAt(double y) const;  // y in list content coordinates
    void OnListMotion(double x, double y);
    void OnListPressed(int button, double x, double y);
    void OnListReleased(int button, double x, double y);
    void OnActionsMotion(double x, double y);
    void OnActionsReleased(double y);
    void OnBackdropPressed(double x, double y);

    // drawing (widgets are updated from the state in an idle callback)
    void RequestRender();
    void Render();
    void RebuildRows();
    void UpdateSelection();
    void UpdateChrome();
    void UpdateActionsPanel();
    void ApplyScroll();
    GtkWidget* MakeRow(const ResultItem& item);
    GtkWidget* MakeIcon(const ItemIcon& icon, int size);
    GtkWidget* MakeKeycaps(const std::vector<std::wstring>& keys);
    std::string TintClass(uint32_t argb);
    std::wstring ChipText() const;
    std::wstring Placeholder() const;
    double ListViewHeight() const;

    // timers
    void StartTimer(guint& id, guint ms, void (LauncherWindow::*fn)());
    void StopTimer(guint& id);
    void OnStatusTimer();
    void OnWindowsTimer();
    void OnPluginTimer();
    void OnBusyTimer();
    void OnBlurTimer();

    App& app_;
    std::shared_ptr<SinkState> sink_;

    // widgets (owned by the window)
    GtkWidget* window_ = nullptr;
    GtkWidget* backdrop_ = nullptr;
    GtkWidget* panel_ = nullptr;
    GtkWidget* chip_ = nullptr;
    GtkWidget* chipLabel_ = nullptr;
    GtkWidget* text_ = nullptr;
    GtkWidget* stack_ = nullptr;
    GtkWidget* listScroll_ = nullptr;
    GtkWidget* rows_ = nullptr;
    GtkWidget* emptyLabel_ = nullptr;
    GtkWidget* detailScroll_ = nullptr;
    GtkWidget* detailView_ = nullptr;
    GtkWidget* actionsPanel_ = nullptr;
    GtkWidget* actionsHeader_ = nullptr;
    GtkWidget* actionsScroll_ = nullptr;
    GtkWidget* actionsBox_ = nullptr;
    GtkWidget* statusLabel_ = nullptr;
    GtkWidget* primaryBox_ = nullptr;
    GtkWidget* primaryLabel_ = nullptr;
    GtkWidget* mark_ = nullptr;
    uint32_t accent_ = 0xFFFF5A5F;
    GdkSurface* surface_ = nullptr;  // the window's surface, for its "activated" state
    gulong surfaceHandler_ = 0;
    GtkCssProvider* css_ = nullptr;
    GtkCssProvider* tintCss_ = nullptr;
    std::set<uint32_t> tints_;
    std::vector<GtkWidget*> itemRows_;  // row widget of each result (null when not built)
    std::vector<GtkWidget*> actionRows_;

    bool visible_ = false;
    bool active_ = false;    // keyboard focus
    bool hadFocus_ = false;  // had the keyboard since it was shown
    bool suppressAutoHide_ = false;
    bool settingText_ = false;
    bool scopePending_ = false;  // entering a plugin by its keyword (see TryEnterPluginByKeyword)
    bool preedit_ = false;
    bool renderQueued_ = false;
    bool rowsDirty_ = true;
    bool actionsDirty_ = true;
    bool detailDirty_ = false;
    bool scrollPending_ = false;
    bool applyingScroll_ = false;
    int64_t hiddenAt_ = 0;  // monotonic ms
    guint renderSource_ = 0;
    guint focusSource_ = 0;
    double pointerX_ = -1, pointerY_ = -1;

    Mode mode_ = Mode::Root;
    WindowState windows_;
    std::vector<ResultItem> results_;
    std::vector<ListEntry> entries_;
    double contentHeight_ = 0;
    double scroll_ = 0;
    int selected_ = -1;
    bool actionsOpen_ = false;
    int actionSelected_ = 0;
    int pressedRow_ = -1;
    guint shortcutKey_ = 0;  // unshifted key of the key press being handled

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
    Mode detailReturnMode_ = Mode::Root;
    std::wstring detailReturnQuery_;
    std::vector<ResultItem> detailReturnResults_;
    int detailReturnSelected_ = -1;
    double detailReturnScroll_ = 0;

    std::wstring status_;
    bool statusWarning_ = false;
    std::wstring armedKey_;
    int64_t armedAt_ = 0;

    guint statusTimer_ = 0;
    guint windowsTimer_ = 0;
    guint pluginTimer_ = 0;
    guint busyTimer_ = 0;
    guint blurTimer_ = 0;
};

}  // namespace beam
