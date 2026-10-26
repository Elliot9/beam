#include "linux/launcher.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <sstream>

#include "core/fuzzy.hpp"
#include "core/text.hpp"
#include "linux/app.hpp"
#include "linux/system.hpp"

namespace beam {
namespace {

constexpr int kPanelWidth = 750;
constexpr int kPanelHeight = 474;
constexpr double kRowHeight = 42;
constexpr double kHeaderHeight = 28;
constexpr double kListPadding = 6;
constexpr double kListHeight = kPanelHeight - 57 - 40;  // search 56 + separator 1, footer 40
constexpr double kDetailStep = 34;                      // arrow-key scroll distance
constexpr int kActionsWidth = 330;
constexpr int64_t kPopToRootAfterMs = 90'000;
constexpr int64_t kConfirmWindowMs = 4'000;
constexpr size_t kMaxDetailChars = 300'000;

constexpr wchar_t kSectionResults[] = L"結果";

int64_t NowMs() { return g_get_monotonic_time() / 1000; }

// BEAM_DEBUG=1: show / hide / focus events on stderr (focus problems are compositor-specific).
void Debug(const char* format, ...) G_GNUC_PRINTF(1, 2);
void Debug(const char* format, ...) {
    static const bool enabled = g_getenv("BEAM_DEBUG") != nullptr;
    if (!enabled) return;
    va_list args;
    va_start(args, format);
    gchar* text = g_strdup_vprintf(format, args);
    va_end(args);
    g_printerr("beam[%lld]: %s\n", static_cast<long long>(NowMs() % 100000), text);
    g_free(text);
}

// Plugin and Detail are sub-views of Root; the window switcher is its own family.
Mode Family(Mode m) { return m == Mode::Windows ? Mode::Windows : Mode::Root; }

const char* ModeName(Mode m) {
    switch (m) {
        case Mode::Root: return "root";
        case Mode::Windows: return "windows";
        case Mode::Plugin: return "plugin";
        case Mode::Detail: return "detail";
    }
    return "?";
}

struct Palette {
    bool dark;
    uint32_t background, footer, border, separator, text, textSecondary, textTertiary, selection, panel, panelBorder,
        keycap, keycapText, chip, chipText, warning, accent, textSelection, caret;
};

// Same colours as the Windows renderer (src/win/renderer.cpp).
Palette MakePalette(bool dark) {
    if (dark)
        return {true,       0xFF1E1E21, 0xFF232327, 0xFF3A3A41, 0xFF2E2E34, 0xFFEDEDEF, 0xFF9B9BA4,
                0xFF6F6F79, 0xFF34343B, 0xFF27272C, 0xFF3F3F47, 0xFF34343B, 0xFFB4B4BD, 0xFF3A3A42,
                0xFFEDEDEF, 0xFFFF9F43, 0xFFFF5A5F, 0x804C8DFF, 0xFFEDEDEF};
    return {false,      0xFFFBFBFC, 0xFFF2F2F5, 0xFFD5D5DC, 0xFFE6E6EB, 0xFF1D1D1F, 0xFF696972,
            0xFF9B9BA4, 0xFFE7E7EC, 0xFFFFFFFF, 0xFFD5D5DC, 0xFFE6E6EB, 0xFF55555E, 0xFFE7E7EC,
            0xFF1D1D1F, 0xFFD9480F, 0xFFE5484D, 0x662F6FEB, 0xFF1D1D1F};
}

std::string Css(uint32_t argb) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "rgba(%u,%u,%u,%.3f)", (argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF,
                  ((argb >> 24) & 0xFF) / 255.0);
    return buf;
}

std::string StyleSheet(const Palette& p) {
    std::ostringstream css;
    auto rule = [&css](const char* selector, const std::string& body) { css << selector << " { " << body << " }\n"; };
    rule("window.beam-window, window.beam-window.background",
         "background: none; background-color: transparent; box-shadow: none; border: none; border-radius: 0;");
    rule(".beam-backdrop", "background: none;");
    rule(".beam-panel", "background-color: " + Css(p.background) + "; border: 1px solid " + Css(p.border) +
                            "; border-radius: 12px; color: " + Css(p.text) +
                            "; box-shadow: 0 14px 44px rgba(0,0,0," + (p.dark ? "0.55" : "0.28") + ");");
    rule(".beam-search", "min-height: 56px; padding: 0 20px; border-bottom: 1px solid " + Css(p.separator) + ";");
    rule(".beam-chip", "background-color: " + Css(p.chip) + "; color: " + Css(p.chipText) +
                           "; border-radius: 6px; padding: 0 10px 0 8px; min-height: 24px; margin-right: 10px;");
    rule(".beam-chip label", "font-size: 12px; color: " + Css(p.chipText) + ";");
    rule(".beam-chip image", "color: " + Css(p.chipText) + "; margin-right: 4px;");
    rule("text.beam-query", "font-size: 18px; color: " + Css(p.text) + "; caret-color: " + Css(p.caret) +
                                "; background: none; padding: 0; margin: 0;");
    rule("text.beam-query.readonly", "caret-color: transparent;");
    rule("text.beam-query > placeholder", "color: " + Css(p.textTertiary) + "; opacity: 1;");
    rule("text.beam-query > selection", "background-color: " + Css(p.textSelection) + "; color: " + Css(p.text) + ";");
    rule(".beam-rows", "padding: 6px 0;");
    rule(".beam-section", "font-size: 12px; font-weight: 600; color: " + Css(p.textTertiary) +
                              "; padding: 6px 20px 0 20px; min-height: 22px;");
    rule(".beam-row", "min-height: 42px; margin: 0 8px; padding: 0 14px 0 12px; border-radius: 8px;");
    rule(".beam-row.selected", "background-color: " + Css(p.selection) + ";");
    rule(".beam-title", "font-size: 14px; color: " + Css(p.text) + ";");
    rule(".beam-subtitle", "font-size: 13px; color: " + Css(p.textSecondary) + ";");
    rule(".beam-accessory", "font-size: 12px; color: " + Css(p.textTertiary) + ";");
    rule(".beam-tile", "border-radius: 6px;");
    rule(".beam-tile image", "color: #ffffff;");
    rule(".beam-tile.placeholder", "background-color: " + Css(p.selection) + ";");
    rule(".beam-tile.placeholder image", "color: " + Css(p.textTertiary) + ";");
    rule(".beam-empty", "font-size: 14px; color: " + Css(p.textTertiary) + ";");
    rule(".beam-footer", "background-color: " + Css(p.footer) + "; border-top: 1px solid " + Css(p.separator) +
                             "; min-height: 39px; padding: 0 16px;");
    rule(".beam-status", "font-size: 12px; color: " + Css(p.textSecondary) + ";");
    rule(".beam-status.warning", "color: " + Css(p.warning) + ";");
    rule(".beam-primary", "font-size: 12px; color: " + Css(p.text) + ";");
    rule(".beam-hint", "font-size: 12px; color: " + Css(p.textSecondary) + ";");
    rule(".beam-vsep", "background-color: " + Css(p.separator) + "; min-width: 1px; min-height: 16px;");
    rule(".beam-keycap", "background-color: " + Css(p.keycap) + "; color: " + Css(p.keycapText) +
                             "; border-radius: 4px; padding: 0 5px; min-width: 10px; min-height: 18px;"
                             " font-size: 11px; font-weight: 600;");
    rule(".beam-actions", "background-color: " + Css(p.panel) + "; border: 1px solid " + Css(p.panelBorder) +
                              "; border-radius: 10px; padding: 0 6px 8px 6px; box-shadow: 0 8px 26px rgba(0,0,0," +
                              (p.dark ? "0.5" : "0.22") + ");");
    rule(".beam-actions-header", "font-size: 12px; font-weight: 600; color: " + Css(p.textTertiary) +
                                     "; padding: 0 8px; min-height: 30px;");
    rule(".beam-action", "min-height: 34px; border-radius: 6px; padding: 0 8px;");
    rule(".beam-action.selected", "background-color: " + Css(p.selection) + ";");
    rule(".beam-action image", "color: " + Css(p.textSecondary) + ";");
    rule(".beam-action.dangerous image", "color: " + Css(p.warning) + ";");
    rule(".beam-action-title", "font-size: 14px; color: " + Css(p.text) + ";");
    rule("textview.beam-detail, textview.beam-detail > text",
         "background: none; background-color: transparent; color: " + Css(p.text) +
             "; font-family: monospace; font-size: 12.5px;");
    rule("textview.beam-detail > text > selection", "background-color: " + Css(p.textSelection) + ";");
    return css.str();
}

std::vector<std::string> SplitNames(const std::string& names) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= names.size()) {
        const size_t bar = names.find('|', start);
        const std::string part = names.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
        if (!part.empty()) out.push_back(part);
        if (bar == std::string::npos) break;
        start = bar + 1;
    }
    return out;
}

// Symbolic icon by name(s); "a|b" falls back to b when the icon theme has no a.
GtkWidget* SymbolicImage(const std::string& names, int size) {
    std::vector<std::string> list = SplitNames(names);
    if (list.empty()) list.push_back(icon::kPuzzle);
    std::vector<const char*> c;
    for (const auto& n : list) c.push_back(n.c_str());
    GIcon* gicon = g_themed_icon_new_from_names(const_cast<char**>(c.data()), static_cast<int>(c.size()));
    GtkWidget* image = gtk_image_new_from_gicon(gicon);
    g_object_unref(gicon);
    gtk_image_set_pixel_size(GTK_IMAGE(image), size);
    return image;
}

GtkWidget* Label(const std::wstring& text, const char* cssClass, float xalign = 0.0f) {
    GtkWidget* label = gtk_label_new(U8(text).c_str());
    gtk_label_set_xalign(GTK_LABEL(label), xalign);
    gtk_label_set_single_line_mode(GTK_LABEL(label), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    if (cssClass) gtk_widget_add_css_class(label, cssClass);
    return label;
}

void RemoveChildren(GtkWidget* box) {
    while (GtkWidget* child = gtk_widget_get_first_child(box)) gtk_box_remove(GTK_BOX(box), child);
}

// The direct child of `parent` under (x, y) (parent coordinates).
GtkWidget* ChildAt(GtkWidget* parent, double x, double y) {
    GtkWidget* w = gtk_widget_pick(parent, x, y, GTK_PICK_DEFAULT);
    while (w && gtk_widget_get_parent(w) != parent) w = gtk_widget_get_parent(w);
    return w == parent ? nullptr : w;
}

int IndexOf(const std::vector<GtkWidget*>& widgets, GtkWidget* w) {
    if (!w) return -1;
    const auto it = std::find(widgets.begin(), widgets.end(), w);
    return it == widgets.end() ? -1 : static_cast<int>(it - widgets.begin());
}

guint NormalizeKey(guint keyval) {
    switch (keyval) {
        case GDK_KEY_KP_Enter:
        case GDK_KEY_ISO_Enter: return GDK_KEY_Return;
        case GDK_KEY_KP_Up: return GDK_KEY_Up;
        case GDK_KEY_KP_Down: return GDK_KEY_Down;
        case GDK_KEY_KP_Page_Up: return GDK_KEY_Page_Up;
        case GDK_KEY_KP_Page_Down: return GDK_KEY_Page_Down;
        case GDK_KEY_KP_Home: return GDK_KEY_Home;
        case GDK_KEY_KP_End: return GDK_KEY_End;
        case GDK_KEY_ISO_Left_Tab: return GDK_KEY_Tab;
        default: return gdk_keyval_to_lower(keyval);
    }
}

// The key's unshifted value on the current layout; when that layout is not Latin (Cyrillic,
// Greek...), the value on the first layout that is, so Ctrl+K works on any layout.
guint BaseKeyval(GdkDisplay* display, guint keycode, guint layout) {
    if (!display) return 0;
    guint base = 0;
    if (!gdk_display_translate_key(display, keycode, static_cast<GdkModifierType>(0), static_cast<int>(layout), &base,
                                   nullptr, nullptr, nullptr))
        base = 0;
    const guint32 ch = gdk_keyval_to_unicode(base);
    if (base && (ch == 0 || ch < 0x80)) return base;  // function keys, or ASCII
    GdkKeymapKey* keys = nullptr;
    guint* keyvals = nullptr;
    int n = 0;
    if (gdk_display_map_keycode(display, keycode, &keys, &keyvals, &n)) {
        for (int i = 0; i < n; ++i) {
            const guint32 c = gdk_keyval_to_unicode(keyvals[i]);
            if (keys[i].level == 0 && c >= 0x20 && c < 0x80) {
                base = keyvals[i];
                break;
            }
        }
        g_free(keys);
        g_free(keyvals);
    }
    return base;
}

bool IsModifierKey(guint keyval) {
    switch (keyval) {
        case GDK_KEY_Shift_L: case GDK_KEY_Shift_R: case GDK_KEY_Control_L: case GDK_KEY_Control_R:
        case GDK_KEY_Alt_L: case GDK_KEY_Alt_R: case GDK_KEY_Meta_L: case GDK_KEY_Meta_R:
        case GDK_KEY_Super_L: case GDK_KEY_Super_R: case GDK_KEY_Hyper_L: case GDK_KEY_Hyper_R:
        case GDK_KEY_Caps_Lock: case GDK_KEY_ISO_Level3_Shift: case GDK_KEY_ISO_Level5_Shift:
            return true;
        default:
            return false;
    }
}

// The panel sits horizontally centred, 22% down the window (like Windows on the work area).
void BackdropMeasure(GtkWidget* widget, GtkOrientation orientation, int, int* minimum, int* natural, int* minBaseline,
                     int* natBaseline) {
    int childMin = 0;
    if (GtkWidget* child = gtk_widget_get_first_child(widget))
        gtk_widget_measure(child, orientation, -1, &childMin, nullptr, nullptr, nullptr);
    *minimum = *natural = childMin;
    *minBaseline = *natBaseline = -1;
}

void BackdropAllocate(GtkWidget* widget, int width, int height, int) {
    GtkWidget* child = gtk_widget_get_first_child(widget);
    if (!child) return;
    int w = 0, h = 0;
    gtk_widget_measure(child, GTK_ORIENTATION_HORIZONTAL, -1, &w, nullptr, nullptr, nullptr);
    gtk_widget_measure(child, GTK_ORIENTATION_VERTICAL, w, &h, nullptr, nullptr, nullptr);
    GtkAllocation a{std::max(0, (width - w) / 2), std::max(0, static_cast<int>((height - h) * 0.22)), w, h};
    gtk_widget_size_allocate(child, &a, -1);
}

// G_CALLBACK for lambdas (a macro cannot take a lambda whose body contains commas).
template <typename Fn>
GCallback Callback(Fn* fn) {
    return reinterpret_cast<GCallback>(fn);
}

struct TimerData {
    LauncherWindow* self;
    void (LauncherWindow::*fn)();
    guint* id;
};

// Runs fn on the main loop after the current event (e.g. a click that rebuilds the list).
void Later(std::function<void()> fn) {
    auto* p = new std::function<void()>(std::move(fn));
    g_idle_add_full(
        G_PRIORITY_DEFAULT,
        [](gpointer data) -> gboolean {
            (*static_cast<std::function<void()>*>(data))();
            return G_SOURCE_REMOVE;
        },
        p, [](gpointer data) { delete static_cast<std::function<void()>*>(data); });
}

}  // namespace

LauncherWindow::LauncherWindow(App& app) : app_(app), sink_(std::make_shared<SinkState>()) { sink_->launcher = this; }

LauncherWindow::~LauncherWindow() {
    sink_->launcher = nullptr;
    for (guint* id : {&statusTimer_, &windowsTimer_, &pluginTimer_, &busyTimer_, &blurTimer_, &focusSource_, &renderSource_}) {
        if (*id) g_source_remove(*id);
        *id = 0;
    }
    DisconnectSurface();
    if (window_) gtk_window_destroy(GTK_WINDOW(window_));
    if (css_) {
        gtk_style_context_remove_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css_));
        g_object_unref(css_);
    }
    if (tintCss_) {
        gtk_style_context_remove_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(tintCss_));
        g_object_unref(tintCss_);
    }
}

// ---------------------------------------------------------------------------
// Widgets

void LauncherWindow::Create(GtkApplication* application) {
    LoadCss();
    BuildWidgets(application);
}

void LauncherWindow::LoadCss() {
    GdkDisplay* display = gdk_display_get_default();
    if (!display) return;
    const std::wstring theme = app_.settings().theme;
    const bool dark = theme == L"dark" ? true : theme == L"light" ? false : app_.darkTheme();
    const Palette p = MakePalette(dark);
    accent_ = p.accent;
    if (!css_) {
        css_ = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(css_),
                                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    gtk_css_provider_load_from_string(css_, StyleSheet(p).c_str());
    if (!tintCss_) {
        tintCss_ = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(tintCss_),
                                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    if (mark_) gtk_widget_queue_draw(mark_);
}

std::string LauncherWindow::TintClass(uint32_t argb) {
    char name[32];
    std::snprintf(name, sizeof name, "beam-tint-%08x", argb);
    if (tints_.insert(argb).second) {
        std::string css;
        for (const uint32_t t : tints_) {
            char rule[96];
            std::snprintf(rule, sizeof rule, ".beam-tint-%08x { background-color: %s; }\n", t, Css(t).c_str());
            css += rule;
        }
        gtk_css_provider_load_from_string(tintCss_, css.c_str());
    }
    return name;
}

void LauncherWindow::BuildWidgets(GtkApplication* application) {
    window_ = gtk_window_new();
    gtk_window_set_application(GTK_WINDOW(window_), application);
    gtk_window_set_title(GTK_WINDOW(window_), "Beam");
    gtk_window_set_icon_name(GTK_WINDOW(window_), "local.beam.Launcher");
    gtk_window_set_decorated(GTK_WINDOW(window_), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(window_), 1000, 700);
    gtk_widget_add_css_class(window_, "beam-window");
    gtk_widget_remove_css_class(window_, "background");
    gtk_window_set_hide_on_close(GTK_WINDOW(window_), FALSE);

    backdrop_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(backdrop_, "beam-backdrop");
    gtk_widget_set_layout_manager(backdrop_, gtk_custom_layout_new(nullptr, BackdropMeasure, BackdropAllocate));
    gtk_window_set_child(GTK_WINDOW(window_), backdrop_);

    panel_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(panel_, "beam-panel");
    gtk_widget_set_size_request(panel_, kPanelWidth, kPanelHeight);
    gtk_widget_set_overflow(panel_, GTK_OVERFLOW_HIDDEN);
    gtk_box_append(GTK_BOX(backdrop_), panel_);

    // search row: [chip] [text]
    GtkWidget* search = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(search, "beam-search");
    gtk_box_append(GTK_BOX(panel_), search);
    chip_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(chip_, "beam-chip");
    gtk_widget_set_valign(chip_, GTK_ALIGN_CENTER);
    gtk_widget_set_cursor_from_name(chip_, "pointer");
    gtk_box_append(GTK_BOX(chip_), SymbolicImage(icon::kBack, 12));
    chipLabel_ = Label(L"", nullptr);
    gtk_label_set_max_width_chars(GTK_LABEL(chipLabel_), 26);
    gtk_box_append(GTK_BOX(chip_), chipLabel_);
    gtk_widget_set_visible(chip_, FALSE);
    gtk_box_append(GTK_BOX(search), chip_);
    {
        GtkGesture* click = gtk_gesture_click_new();
        g_signal_connect(click, "released", Callback(+[](GtkGestureClick*, int, double, double, gpointer self) {
                             auto* l = static_cast<LauncherWindow*>(self);
                             Later([l] { l->GoBack(); });
                         }),
                         this);
        gtk_widget_add_controller(chip_, GTK_EVENT_CONTROLLER(click));
    }

    text_ = gtk_text_new();
    gtk_widget_add_css_class(text_, "beam-query");
    gtk_widget_set_hexpand(text_, TRUE);
    gtk_widget_set_valign(text_, GTK_ALIGN_CENTER);
    gtk_text_set_input_hints(GTK_TEXT(text_), GTK_INPUT_HINT_NO_EMOJI);
    gtk_box_append(GTK_BOX(search), text_);
    g_signal_connect(text_, "changed", Callback(+[](GtkEditable*, gpointer self) {
                         auto* l = static_cast<LauncherWindow*>(self);
                         if (!l->settingText_) l->OnTextChanged();
                     }),
                     this);
    g_signal_connect(text_, "preedit-changed", Callback(+[](GtkText*, char* preedit, gpointer self) {
                         static_cast<LauncherWindow*>(self)->preedit_ = preedit && *preedit;
                     }),
                     this);

    // content: list or detail, with the empty-state label and the action panel on top
    GtkWidget* content = gtk_overlay_new();
    gtk_widget_set_vexpand(content, TRUE);
    gtk_box_append(GTK_BOX(panel_), content);
    stack_ = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(stack_), GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_overlay_set_child(GTK_OVERLAY(content), stack_);

    listScroll_ = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(listScroll_), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    rows_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(rows_, "beam-rows");
    gtk_widget_set_valign(rows_, GTK_ALIGN_START);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(listScroll_), rows_);
    gtk_stack_add_named(GTK_STACK(stack_), listScroll_, "list");
    {
        GtkAdjustment* adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(listScroll_));
        g_signal_connect(adj, "changed", Callback(+[](GtkAdjustment*, gpointer self) {
                             auto* l = static_cast<LauncherWindow*>(self);
                             if (l->scrollPending_) l->ApplyScroll();
                         }),
                         this);
        g_signal_connect(adj, "value-changed", Callback(+[](GtkAdjustment* a, gpointer self) {
                             auto* l = static_cast<LauncherWindow*>(self);
                             if (l->applyingScroll_) return;
                             l->scroll_ = gtk_adjustment_get_value(a);  // the user scrolled
                             l->scrollPending_ = false;
                         }),
                         this);
        GtkEventController* motion = gtk_event_controller_motion_new();
        g_signal_connect(motion, "enter", Callback(+[](GtkEventControllerMotion*, double x, double y, gpointer self) {
                             auto* l = static_cast<LauncherWindow*>(self);
                             graphene_point_t in = GRAPHENE_POINT_INIT(static_cast<float>(x), static_cast<float>(y)), out;
                             if (gtk_widget_compute_point(l->rows_, l->window_, &in, &out)) {
                                 if (l->pointerX_ < 0) {  // first sighting after showing: do not select
                                     l->pointerX_ = out.x;
                                     l->pointerY_ = out.y;
                                 }
                             }
                         }),
                         this);
        g_signal_connect(motion, "motion", Callback(+[](GtkEventControllerMotion*, double x, double y, gpointer self) {
                             static_cast<LauncherWindow*>(self)->OnListMotion(x, y);
                         }),
                         this);
        gtk_widget_add_controller(rows_, motion);
        GtkGesture* click = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
        g_signal_connect(click, "pressed", Callback(+[](GtkGestureClick* g, int, double x, double y, gpointer self) {
                             const int button = static_cast<int>(gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g)));
                             static_cast<LauncherWindow*>(self)->OnListPressed(button, x, y);
                         }),
                         this);
        g_signal_connect(click, "released", Callback(+[](GtkGestureClick* g, int, double x, double y, gpointer self) {
                             const int button = static_cast<int>(gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g)));
                             static_cast<LauncherWindow*>(self)->OnListReleased(button, x, y);
                         }),
                         this);
        gtk_widget_add_controller(rows_, GTK_EVENT_CONTROLLER(click));
    }

    detailScroll_ = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(detailScroll_), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    detailView_ = gtk_text_view_new();
    gtk_widget_add_css_class(detailView_, "beam-detail");
    gtk_text_view_set_editable(GTK_TEXT_VIEW(detailView_), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(detailView_), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(detailView_), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(detailView_), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(detailView_), 20);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(detailView_), 28);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(detailView_), 12);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(detailView_), 12);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(detailScroll_), detailView_);
    gtk_stack_add_named(GTK_STACK(stack_), detailScroll_, "detail");

    emptyLabel_ = gtk_label_new("");
    gtk_widget_add_css_class(emptyLabel_, "beam-empty");
    gtk_label_set_wrap(GTK_LABEL(emptyLabel_), TRUE);
    gtk_label_set_justify(GTK_LABEL(emptyLabel_), GTK_JUSTIFY_CENTER);
    gtk_widget_set_halign(emptyLabel_, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(emptyLabel_, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start(emptyLabel_, 40);
    gtk_widget_set_margin_end(emptyLabel_, 40);
    gtk_widget_set_can_target(emptyLabel_, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(content), emptyLabel_);

    actionsPanel_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(actionsPanel_, "beam-actions");
    gtk_widget_set_size_request(actionsPanel_, kActionsWidth, -1);
    gtk_widget_set_halign(actionsPanel_, GTK_ALIGN_END);
    gtk_widget_set_valign(actionsPanel_, GTK_ALIGN_END);
    gtk_widget_set_margin_end(actionsPanel_, 12);
    gtk_widget_set_margin_bottom(actionsPanel_, 10);
    actionsHeader_ = Label(L"", "beam-actions-header");
    gtk_box_append(GTK_BOX(actionsPanel_), actionsHeader_);
    actionsScroll_ = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(actionsScroll_), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(actionsScroll_), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(actionsScroll_),
                                               static_cast<int>(kListHeight) - 18 - 30 - 8);
    actionsBox_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(actionsScroll_), actionsBox_);
    gtk_box_append(GTK_BOX(actionsPanel_), actionsScroll_);
    gtk_widget_set_visible(actionsPanel_, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(content), actionsPanel_);
    {
        GtkEventController* motion = gtk_event_controller_motion_new();
        g_signal_connect(motion, "motion", Callback(+[](GtkEventControllerMotion*, double x, double y, gpointer self) {
                             static_cast<LauncherWindow*>(self)->OnActionsMotion(x, y);
                         }),
                         this);
        gtk_widget_add_controller(actionsBox_, motion);
        GtkGesture* click = gtk_gesture_click_new();
        g_signal_connect(click, "released", Callback(+[](GtkGestureClick*, int, double, double y, gpointer self) {
                             static_cast<LauncherWindow*>(self)->OnActionsReleased(y);
                         }),
                         this);
        gtk_widget_add_controller(actionsBox_, GTK_EVENT_CONTROLLER(click));
    }

    // footer: [mark] status ........ primary [Enter] | 動作 [Ctrl][K]
    GtkWidget* footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(footer, "beam-footer");
    gtk_box_append(GTK_BOX(panel_), footer);
    mark_ = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(mark_), 16);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(mark_), 16);
    gtk_widget_set_valign(mark_, GTK_ALIGN_CENTER);
    gtk_drawing_area_set_draw_func(
        GTK_DRAWING_AREA(mark_),
        [](GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer self) {
            const uint32_t c = static_cast<LauncherWindow*>(self)->accent_;
            const double r = 4;
            cairo_new_sub_path(cr);
            cairo_arc(cr, w - r, r, r, -G_PI / 2, 0);
            cairo_arc(cr, w - r, h - r, r, 0, G_PI / 2);
            cairo_arc(cr, r, h - r, r, G_PI / 2, G_PI);
            cairo_arc(cr, r, r, r, G_PI, 3 * G_PI / 2);
            cairo_close_path(cr);
            cairo_set_source_rgb(cr, ((c >> 16) & 0xFF) / 255.0, ((c >> 8) & 0xFF) / 255.0, (c & 0xFF) / 255.0);
            cairo_fill(cr);
            cairo_set_source_rgb(cr, 1, 1, 1);
            cairo_set_line_width(cr, 2);
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
            cairo_move_to(cr, 4.5, h - 4.5);
            cairo_line_to(cr, w - 4.5, 4.5);
            cairo_stroke(cr);
        },
        this, nullptr);
    gtk_box_append(GTK_BOX(footer), mark_);
    statusLabel_ = Label(L"Beam", "beam-status");
    gtk_widget_set_hexpand(statusLabel_, TRUE);
    gtk_widget_set_margin_start(statusLabel_, 10);
    gtk_widget_set_margin_end(statusLabel_, 16);
    gtk_box_append(GTK_BOX(footer), statusLabel_);
    primaryBox_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    primaryLabel_ = Label(L"", "beam-primary", 1.0f);
    gtk_label_set_max_width_chars(GTK_LABEL(primaryLabel_), 18);
    gtk_widget_set_margin_end(primaryLabel_, 8);
    gtk_box_append(GTK_BOX(primaryBox_), primaryLabel_);
    GtkWidget* enter = MakeKeycaps({L"Enter"});
    gtk_box_append(GTK_BOX(primaryBox_), enter);
    GtkWidget* sep = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(sep, "beam-vsep");
    gtk_widget_set_valign(sep, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start(sep, 14);
    gtk_widget_set_margin_end(sep, 14);
    gtk_box_append(GTK_BOX(primaryBox_), sep);
    gtk_box_append(GTK_BOX(footer), primaryBox_);
    GtkWidget* hint = Label(L"動作", "beam-hint", 1.0f);
    gtk_widget_set_margin_end(hint, 8);
    gtk_box_append(GTK_BOX(footer), hint);
    gtk_box_append(GTK_BOX(footer), MakeKeycaps({L"Ctrl", L"K"}));

    // input: capture phase, so list navigation and action shortcuts come before the text
    // field (but not while the input method is composing, see OnKeyPressed)
    GtkEventController* keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed",
                     Callback(+[](GtkEventControllerKey* controller, guint keyval, guint keycode, GdkModifierType state,
                                  gpointer self) -> gboolean {
                         GdkEvent* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller));
                         const guint layout = event ? gdk_key_event_get_layout(event) : 0;
                         return static_cast<LauncherWindow*>(self)->OnKeyPressed(
                             keyval, BaseKeyval(gtk_widget_get_display(static_cast<LauncherWindow*>(self)->window_), keycode, layout),
                             state);
                     }),
                     this);
    gtk_widget_add_controller(window_, keys);

    GtkGesture* backdropClick = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(backdropClick), 0);
    g_signal_connect(backdropClick, "pressed", Callback(+[](GtkGestureClick*, int, double x, double y, gpointer self) {
                         static_cast<LauncherWindow*>(self)->OnBackdropPressed(x, y);
                     }),
                     this);
    gtk_widget_add_controller(backdrop_, GTK_EVENT_CONTROLLER(backdropClick));

    g_signal_connect(window_, "close-request", Callback(+[](GtkWindow*, gpointer self) -> gboolean {
                         static_cast<LauncherWindow*>(self)->Hide();
                         return TRUE;  // keep the window for next time
                     }),
                     this);
    // Hiding when another window takes over: see OnFocusChanged.
    g_signal_connect(window_, "notify::is-active", Callback(+[](GObject*, GParamSpec*, gpointer self) {
                         static_cast<LauncherWindow*>(self)->OnFocusChanged();
                     }),
                     this);
    g_signal_connect(window_, "realize", Callback(+[](GtkWidget* widget, gpointer self) {
                         auto* l = static_cast<LauncherWindow*>(self);
                         GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(widget));
                         if (!surface || !GDK_IS_TOPLEVEL(surface) || surface == l->surface_) return;
                         l->DisconnectSurface();
                         l->surface_ = surface;
                         l->surfaceHandler_ = g_signal_connect(surface, "notify::state",
                                                               Callback(+[](GObject*, GParamSpec*, gpointer launcher) {
                                                                   static_cast<LauncherWindow*>(launcher)->OnFocusChanged();
                                                               }),
                                                               l);
                     }),
                     this);
    g_signal_connect(window_, "unrealize", Callback(+[](GtkWidget*, gpointer self) {
                         static_cast<LauncherWindow*>(self)->DisconnectSurface();
                     }),
                     this);
}

void LauncherWindow::DisconnectSurface() {
    if (surface_ && surfaceHandler_) g_signal_handler_disconnect(surface_, surfaceHandler_);
    surface_ = nullptr;
    surfaceHandler_ = 0;
}

// Beam hides when another window becomes the active one (Alt+Tab, a click on another window,
// a launched app). The keyboard alone may also leave for a moment while Beam stays the
// compositor's active window: GNOME Shell's input-source switcher (Super+Space, to switch to
// 注音) must not close Beam. If the keyboard does not come back soon (the overview, a menu of
// the top bar), Beam hides as well.
void LauncherWindow::OnFocusChanged() {
    if (!window_) return;
    const bool keyboard = gtk_window_is_active(GTK_WINDOW(window_));
    GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(window_));
    const bool activated = surface && GDK_IS_TOPLEVEL(surface) &&
                           (gdk_toplevel_get_state(GDK_TOPLEVEL(surface)) & GDK_TOPLEVEL_STATE_FOCUSED);
    Debug("focus: keyboard=%d activated=%d visible=%d hadFocus=%d", keyboard, activated, visible_, hadFocus_);
    active_ = keyboard;
    if (keyboard) {
        hadFocus_ = true;
        StopTimer(focusSource_);
        StopTimer(blurTimer_);
        return;
    }
    if (!hadFocus_ || !visible_ || suppressAutoHide_) return;
    if (activated) {
        if (!blurTimer_) StartTimer(blurTimer_, 1500, &LauncherWindow::OnBlurTimer);
        return;
    }
    Hide();
}

void LauncherWindow::OnBlurTimer() {
    if (visible_ && !gtk_window_is_active(GTK_WINDOW(window_))) Hide();
}

GtkWidget* LauncherWindow::MakeKeycaps(const std::vector<std::wstring>& keys) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    for (const auto& k : keys) {
        GtkWidget* cap = gtk_label_new(U8(k).c_str());
        gtk_widget_add_css_class(cap, "beam-keycap");
        gtk_widget_set_valign(cap, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(box), cap);
    }
    return box;
}

GtkWidget* LauncherWindow::MakeIcon(const ItemIcon& icon, int size) {
    auto tile = [this, size](const std::string& name, const std::string& cssClass) {
        GtkWidget* t = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(t, "beam-tile");
        gtk_widget_add_css_class(t, cssClass.c_str());
        gtk_widget_set_size_request(t, size, size);
        gtk_widget_set_hexpand(t, FALSE);  // the centred image must not make the row give it space
        GtkWidget* image = SymbolicImage(name, size * 14 / 24);
        gtk_widget_set_hexpand(image, TRUE);
        gtk_widget_set_halign(image, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(image, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(t), image);
        return t;
    };
    if (icon.type == IconType::Symbolic) return tile(icon.name, TintClass(icon.tint ? icon.tint : tint::kSystem));

    GIcon* gicon = nullptr;
    if (icon.type == IconType::GIcon && icon.gicon) {
        gicon = G_ICON(g_object_ref(icon.gicon.get()));
        GtkIconTheme* theme = gtk_icon_theme_get_for_display(gtk_widget_get_display(window_));
        if (G_IS_THEMED_ICON(gicon) && !gtk_icon_theme_has_gicon(theme, gicon)) g_clear_object(&gicon);
    } else if (icon.type == IconType::File && !icon.name.empty()) {
        GFile* file = g_file_new_for_path(icon.name.c_str());
        gicon = g_file_icon_new(file);
        g_object_unref(file);
    }
    if (!gicon) return tile(icon::kApps, "placeholder");
    GtkWidget* image = gtk_image_new_from_gicon(gicon);
    g_object_unref(gicon);
    gtk_image_set_pixel_size(GTK_IMAGE(image), size);
    gtk_widget_set_size_request(image, size, size);
    return image;
}

GtkWidget* LauncherWindow::MakeRow(const ResultItem& item) {
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(row, "beam-row");
    GtkWidget* icon = MakeIcon(item.icon, 24);
    gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_end(icon, 12);
    gtk_box_append(GTK_BOX(row), icon);
    GtkWidget* title = Label(item.title, "beam-title");
    gtk_box_append(GTK_BOX(row), title);
    // The subtitle only takes what the title leaves (its natural width is tiny, it expands).
    GtkWidget* subtitle = Label(item.subtitle, "beam-subtitle");
    gtk_label_set_max_width_chars(GTK_LABEL(subtitle), 1);
    gtk_widget_set_hexpand(subtitle, TRUE);
    gtk_widget_set_margin_start(subtitle, 10);
    gtk_box_append(GTK_BOX(row), subtitle);
    if (!item.accessory.empty()) {
        GtkWidget* accessory = Label(item.accessory, "beam-accessory", 1.0f);
        gtk_label_set_max_width_chars(GTK_LABEL(accessory), 24);
        gtk_widget_set_margin_start(accessory, 18);
        gtk_box_append(GTK_BOX(row), accessory);
    }
    return row;
}

// ---------------------------------------------------------------------------
// Showing / hiding

bool LauncherWindow::IsActive() const { return visible_ && window_ && gtk_window_is_active(GTK_WINDOW(window_)); }

void LauncherWindow::FetchWindows() {
    if (auto list = app_.windowService().List()) {
        windows_.windows = std::move(*list);
        windows_.extension = ExtensionState::Running;
    } else {
        windows_.windows.clear();
        windows_.extension = app_.windowService().StateWhenNotRunning();
    }
}

void LauncherWindow::Show(Mode mode) {
    if (!window_) return;
    const bool wasVisible = visible_;
    Debug("Show(%s) wasVisible=%d active=%d", ModeName(mode), wasVisible, IsActive());
    if (!wasVisible) FetchWindows();  // before we cover anything
    // Like Raycast: reopening shortly after continues where you were (also inside a plugin);
    // after a while it pops back to the root search.
    const bool reset =
        Family(mode) != Family(mode_) || (!wasVisible && NowMs() - hiddenAt_ > kPopToRootAfterMs);
    bool selectAll = false;
    if (reset) {
        LeaveSubViews();
        mode_ = mode;
        SetText(L"");
    } else if (!wasVisible && mode_ != Mode::Detail) {
        selectAll = true;  // typing replaces the old query
    }
    RefreshResults();
    if (!wasVisible) hadFocus_ = false;
    visible_ = true;
    pointerX_ = pointerY_ = -1;  // a window appearing under a resting pointer must not change the selection
    Render();
    if (!wasVisible) gtk_window_maximize(GTK_WINDOW(window_));
    gtk_window_present(GTK_WINDOW(window_));
    gtk_widget_grab_focus(text_);
    if (selectAll) gtk_editable_select_region(GTK_EDITABLE(text_), 0, -1);
    else gtk_editable_set_position(GTK_EDITABLE(text_), -1);
    if (!gtk_window_is_active(GTK_WINDOW(window_))) EnsureFocus(0);
}

// The compositor normally focuses us (the hotkey's activation token is forwarded by
// GtkApplication). If it did not, GNOME Shell can, through Beam's extension.
void LauncherWindow::EnsureFocus(int attempt) {
    StopTimer(focusSource_);
    struct Data {
        LauncherWindow* self;
        int attempt;
    };
    auto* d = new Data{this, attempt};
    focusSource_ = g_timeout_add_full(
        G_PRIORITY_DEFAULT, attempt == 0 ? 250 : 700,
        [](gpointer p) -> gboolean {
            auto* data = static_cast<Data*>(p);
            LauncherWindow* l = data->self;
            l->focusSource_ = 0;
            if (l->visible_ && !gtk_window_is_active(GTK_WINDOW(l->window_))) {
                const bool asked = l->app_.windowService().ActivateOwnWindow();
                Debug("not focused after showing; asked GNOME Shell to focus us: %d", asked);
                if (asked && data->attempt == 0) l->EnsureFocus(1);
            }
            return G_SOURCE_REMOVE;
        },
        d, [](gpointer p) { delete static_cast<Data*>(p); });
}

void LauncherWindow::Toggle(Mode mode) {
    Debug("Toggle(%s) visible=%d active=%d", ModeName(mode), visible_, IsActive());
    if (visible_ && IsActive() && Family(mode) == Family(mode_)) Hide();
    else Show(mode);
}

void LauncherWindow::Hide() {
    if (!visible_) return;
    Debug("Hide()");
    visible_ = false;
    StopTimer(blurTimer_);
    actionsOpen_ = false;
    pressedRow_ = -1;
    Disarm();
    StopTimer(focusSource_);
    preedit_ = false;  // hiding takes the focus away, which ends any composition
    gtk_widget_set_visible(window_, FALSE);
    hiddenAt_ = NowMs();
}

// ---------------------------------------------------------------------------
// LauncherHost

std::wstring LauncherWindow::Text() const { return text_ ? W(gtk_editable_get_text(GTK_EDITABLE(text_))) : L""; }

void LauncherWindow::SetText(const std::wstring& text, bool selectAll) {
    if (!text_) return;
    settingText_ = true;
    gtk_editable_set_text(GTK_EDITABLE(text_), U8(text).c_str());
    if (selectAll) gtk_editable_select_region(GTK_EDITABLE(text_), 0, -1);
    else gtk_editable_set_position(GTK_EDITABLE(text_), -1);
    settingText_ = false;
}

void LauncherWindow::SetQuery(const std::wstring& text) {
    SetText(text);
    OnTextChanged();
}

void LauncherWindow::EnterMode(Mode mode) {
    if (mode == Mode::Windows) SwitchMode(Mode::Windows);
    else if (mode == Mode::Root) BackToRoot(L"");
}

void LauncherWindow::ShowStatus(const std::wstring& message, bool warning) {
    status_ = message;
    statusWarning_ = warning;
    StartTimer(statusTimer_, warning ? 4000 : 2500, &LauncherWindow::OnStatusTimer);
    RequestRender();
}

void LauncherWindow::RefreshWindows() { StartTimer(windowsTimer_, 250, &LauncherWindow::OnWindowsTimer); }

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

void LauncherWindow::OnThemeChanged() {
    LoadCss();
    rowsDirty_ = true;
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
            StartPluginQuery(pluginLocal_ ? std::wstring() : Text());
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
    if (mode_ == Mode::Plugin) results_ = PluginView(Text());
    else results_ = app_.search().Search(Text(), mode_, windows_);
    BuildEntries();
    actionsOpen_ = false;
    const int count = static_cast<int>(results_.size());
    if (keepSelection && previous >= 0 && count > 0) {
        selected_ = std::min(previous, count - 1);
    } else {
        scroll_ = 0;
        selected_ = count == 0 ? -1 : 0;
        // Like Alt+Tab: with no filter the second window (the previous one) is pre-selected.
        if (mode_ == Mode::Windows && Trim(Text()).empty() && count > 1) selected_ = 1;
    }
    if (selected_ >= 0) EnsureVisible(selected_);
    else scroll_ = 0;
}

void LauncherWindow::BuildEntries() {
    entries_.clear();
    double y = kListPadding;
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
    rowsDirty_ = true;
}

void LauncherWindow::OnTextChanged() {
    Disarm();
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
    LeaveSubViews();
    mode_ = mode;
    SetText(L"");
    if (mode == Mode::Windows) FetchWindows();
    RefreshResults();
    RequestRender();
}

void LauncherWindow::LeaveSubViews() {
    if (pluginTicket_) app_.plugins().Cancel(pluginTicket_);  // only ever a query run
    pluginTicket_ = 0;
    pluginBusy_ = false;
    busyShown_ = false;
    StopTimer(pluginTimer_);
    StopTimer(busyTimer_);
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
    detailDirty_ = true;
    detailReturnMode_ = Mode::Root;
    detailReturnQuery_.clear();
    detailReturnResults_.clear();
}

void LauncherWindow::BackToRoot(std::wstring query) {
    LeaveSubViews();
    mode_ = Mode::Root;
    status_.clear();  // a plugin's message does not belong to the root search
    statusWarning_ = false;
    SetText(query);
    Disarm();
    RefreshResults();
    if (text_ && !gtk_widget_has_focus(text_)) gtk_widget_grab_focus(text_);
    gtk_editable_set_position(GTK_EDITABLE(text_), -1);
    RequestRender();
}

void LauncherWindow::BackFromDetail() {
    if (mode_ != Mode::Detail) return;
    mode_ = detailReturnMode_;
    SetText(detailReturnQuery_);
    results_ = std::move(detailReturnResults_);
    detailReturnResults_.clear();
    detailTitle_.clear();
    detailText_.clear();
    detailDirty_ = true;
    actionsOpen_ = false;
    Disarm();
    BuildEntries();
    const int count = static_cast<int>(results_.size());
    selected_ = count == 0 ? -1 : std::clamp(detailReturnSelected_, 0, count - 1);
    scroll_ = detailReturnScroll_;
    if (mode_ == Mode::Root || mode_ == Mode::Windows) RefreshResults(true);  // may have changed meanwhile
    else if (selected_ >= 0) EnsureVisible(selected_);
    if (text_ && !gtk_widget_has_focus(text_)) gtk_widget_grab_focus(text_);
    gtk_editable_set_position(GTK_EDITABLE(text_), -1);
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

double LauncherWindow::ListViewHeight() const {
    const int h = listScroll_ ? gtk_widget_get_height(listScroll_) : 0;
    return h > 0 ? h : kListHeight;
}

void LauncherWindow::EnsureVisible(int index) {
    const double view = ListViewHeight();
    for (size_t k = 0; k < entries_.size(); ++k) {
        if (entries_[k].item != index) continue;
        double top = entries_[k].top;
        const double bottom = top + entries_[k].height;
        if (k > 0 && entries_[k - 1].item < 0) top = entries_[k - 1].top;  // keep the section header in view
        if (k <= 1) top = 0;
        if (top < scroll_) scroll_ = top;
        else if (bottom + kListPadding > scroll_ + view) scroll_ = bottom + kListPadding - view;
        break;
    }
    scroll_ = std::clamp(scroll_, 0.0, std::max(0.0, contentHeight_ - view));
    scrollPending_ = true;
}

void LauncherWindow::ApplyScroll() {
    GtkAdjustment* adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(listScroll_));
    const double target = std::clamp(scroll_, 0.0, std::max(0.0, contentHeight_ - ListViewHeight()));
    applyingScroll_ = true;
    gtk_adjustment_set_value(adj, target);
    applyingScroll_ = false;
    // Done once the list has its new height (the adjustment's range is updated after layout).
    if (std::fabs(gtk_adjustment_get_value(adj) - target) < 0.5 &&
        gtk_adjustment_get_upper(adj) + 0.5 >= std::max(contentHeight_, gtk_adjustment_get_page_size(adj)))
        scrollPending_ = false;
}

void LauncherWindow::RunAction(int itemIndex, int actionIndex) {
    if (itemIndex < 0 || itemIndex >= static_cast<int>(results_.size())) return;
    const ResultItem item = results_[itemIndex];  // copy: the action may rebuild results_
    if (actionIndex < 0 || actionIndex >= static_cast<int>(item.actions.size())) return;
    const Action& action = item.actions[actionIndex];
    if (!action.run) return;
    if (action.dangerous) {
        const std::wstring key = item.title + L'\x1F' + action.title;
        const int64_t now = NowMs();
        if (armedKey_ != key || now - armedAt_ > kConfirmWindowMs) {
            armedKey_ = key;
            armedAt_ = now;
            ShowStatus(L"再按一次 Enter（或再點一次）確認：" + item.title, true);
            return;
        }
    }
    Disarm();
    actionsOpen_ = false;
    if (!item.usageKey.empty() && action.recordsUsage) app_.RecordUsage(item.usageKey, Text());
    suppressAutoHide_ = true;
    const Outcome outcome = action.run(*this);
    suppressAutoHide_ = false;
    if (outcome == Outcome::Hide) Hide();
    else RequestRender();
}

bool LauncherWindow::TryShortcut(guint keyval, bool ctrl, bool shift, bool alt) {
    if (selected_ < 0 || selected_ >= static_cast<int>(results_.size())) return false;
    const auto& actions = results_[selected_].actions;
    const guint base = shortcutKey_ ? shortcutKey_ : keyval;
    auto matches = [&](const Shortcut& s) {
        return s.Matches(keyval, ctrl, shift, alt) || (base != keyval && s.Matches(base, ctrl, shift, alt));
    };
    for (size_t i = 0; i < actions.size(); ++i) {
        if (matches(actions[i].shortcut) || matches(actions[i].alternate)) {
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
    actionsDirty_ = true;
    RequestRender();
}

void LauncherWindow::CloseActions() {
    if (!actionsOpen_) return;
    actionsOpen_ = false;
    RequestRender();
}

std::wstring LauncherWindow::DefaultStatus() const {
    if (!commandBusyName_.empty()) return L"執行中：" + commandBusyName_ + L"…";
    switch (mode_) {
        case Mode::Windows:
            if (windows_.extension != ExtensionState::Running) return L"切換視窗";
            return std::to_wstring(windows_.windows.size()) + L" 個視窗";
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
    std::weak_ptr<SinkState> weak = sink_;
    return [weak](std::unique_ptr<PluginResult> result) {  // on a worker thread
        struct Payload {
            std::weak_ptr<SinkState> weak;
            std::unique_ptr<PluginResult> result;
        };
        auto* payload = new Payload{weak, std::move(result)};
        g_idle_add_full(
            G_PRIORITY_DEFAULT,
            [](gpointer data) -> gboolean {
                std::unique_ptr<Payload> p(static_cast<Payload*>(data));
                if (auto state = p->weak.lock(); state && state->launcher)
                    state->launcher->HandlePluginResult(std::move(p->result));
                return G_SOURCE_REMOVE;
            },
            payload, nullptr);
    };
}

const PluginCommand* LauncherWindow::ActivePlugin() const {
    return pluginId_.empty() ? nullptr : app_.plugins().Find(pluginId_);
}

// "keyword " (keyword + space) in the root search enters a list plugin, like Raycast aliases.
// Called from the text field's "changed" signal, where the text must not be replaced, so the
// switch happens right after (before the next frame).
const PluginCommand* LauncherWindow::KeywordPlugin(std::wstring* keyword, std::wstring* rest) const {
    const std::wstring text = Text();
    size_t space = 0;
    while (space < text.size() && !IsSpace(text[space])) ++space;
    if (space == 0 || space == text.size()) return nullptr;
    const PluginCommand* cmd = app_.plugins().FindByKeyword(text.substr(0, space));
    if (!cmd || cmd->spec.mode != PluginMode::List) return nullptr;
    if (keyword) *keyword = text.substr(0, space);
    if (rest) *rest = TrimStart(std::wstring_view(text).substr(space));
    return cmd;
}

bool LauncherWindow::TryEnterPluginByKeyword() {
    if (scopePending_) return true;
    if (!KeywordPlugin(nullptr, nullptr)) return false;
    scopePending_ = true;
    Later([this] { FinishPendingScope(); });
    return true;
}

// Also called before handling a key, so "gh" Space Enter typed quickly enters the plugin
// before Enter is handled.
void LauncherWindow::FinishPendingScope() {
    if (!scopePending_) return;
    scopePending_ = false;
    std::wstring keyword, rest;
    const PluginCommand* cmd = mode_ == Mode::Root ? KeywordPlugin(&keyword, &rest) : nullptr;
    if (!cmd) {  // the text changed meanwhile
        if (mode_ == Mode::Root) RefreshResults();
        RequestRender();
        return;
    }
    app_.RecordUsage(L"plugin:" + cmd->spec.id, keyword);
    EnterPluginScope(*cmd, rest, keyword);
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
    if (mode_ == Mode::Root) returnQuery = Text();
    else if (mode_ == Mode::Plugin && pluginId_ == commandId) returnQuery = pluginReturnQuery_;
    EnterPluginScope(*cmd, query, returnQuery);
}

void LauncherWindow::EnterPluginScope(const PluginCommand& cmd, const std::wstring& query,
                                      const std::wstring& returnQuery) {
    const PluginCommand command = cmd;  // LeaveSubViews may not touch it, but keep a copy anyway
    LeaveSubViews();
    mode_ = Mode::Plugin;
    pluginId_ = command.spec.id;
    pluginReturnQuery_ = returnQuery;
    pluginLocal_ = command.spec.filtering == PluginFiltering::Beam;
    pluginPlaceholder_ = command.spec.placeholder;
    SetText(query);
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
    if (cmd->spec.debounceMs <= 0) StartPluginQuery(Text());
    else StartTimer(pluginTimer_, static_cast<guint>(cmd->spec.debounceMs), &LauncherWindow::OnPluginTimer);
    RequestRender();
}

void LauncherWindow::StartPluginQuery(const std::wstring& query) {
    StopTimer(pluginTimer_);
    const PluginCommand* cmd = ActivePlugin();
    if (!cmd) return;
    pluginTicket_ = app_.plugins().Start(ResultSink(), *cmd, PluginRequestKind::Query, query);
    pluginBusy_ = true;
    StartTimer(busyTimer_, 150, &LauncherWindow::OnBusyTimer);
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
    const std::wstring query = mode_ == Mode::Detail ? detailReturnQuery_ : Text();
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
        StopTimer(busyTimer_);
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
                             Action{L"重試", icon::kRefresh, Keys(GDK_KEY_r, true), false, [this](LauncherHost&) {
                                        StartPluginQuery(pluginLocal_ ? std::wstring() : Text());
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
    results_ = PluginView(Text());
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
        const std::wstring returnQuery = mode_ == Mode::Root ? Text() : std::wstring();
        LeaveSubViews();
        mode_ = Mode::Plugin;
        pluginId_ = cmd.spec.id;
        pluginReturnQuery_ = returnQuery;
        pluginPlaceholder_ = cmd.spec.placeholder;
        SetText(L"");
    }
    // A list returned by a command (not by live queries) is filtered by Beam as you type.
    if (cmd.spec.mode == PluginMode::Command) pluginLocal_ = true;
    if (!response.placeholder.empty()) pluginPlaceholder_ = response.placeholder;
    pluginFailed_ = false;
    pluginSilent_ = false;
    pluginAll_ = app_.search().PluginItems(cmd, response);
    lastPluginQuery_.clear();
    results_ = PluginView(Text());
    BuildEntries();
    actionsOpen_ = false;
    selected_ = results_.empty() ? -1 : 0;
    scroll_ = 0;
    scrollPending_ = true;
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
        if (showsSomething) hiddenAt_ = NowMs();
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
    if (mode_ != Mode::Detail) {
        detailReturnMode_ = mode_;
        detailReturnQuery_ = Text();
        detailReturnResults_ = std::move(results_);
        detailReturnSelected_ = selected_;
        detailReturnScroll_ = scroll_;
    }
    mode_ = Mode::Detail;
    detailTitle_ = title;
    detailText_ = text.size() > kMaxDetailChars ? text.substr(0, kMaxDetailChars) + L"\n…（內容太長，已截斷）" : text;
    detailDirty_ = true;
    SetText(L"");
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
    item.actions.push_back(Action{L"複製全部", icon::kCopy, Keys(GDK_KEY_Return), false,
                                  [text](LauncherHost&) {
                                      CopyText(text);
                                      return Outcome::Hide;
                                  },
                                  Keys(GDK_KEY_c, true, true)});
    return item;
}

void LauncherWindow::ScrollDetail(double delta) {
    GtkAdjustment* adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(detailScroll_));
    const double max = std::max(gtk_adjustment_get_lower(adj),
                                gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj));
    gtk_adjustment_set_value(adj, std::clamp(gtk_adjustment_get_value(adj) + delta, gtk_adjustment_get_lower(adj), max));
}

// ---------------------------------------------------------------------------
// Keyboard

bool LauncherWindow::OnKeyPressed(guint rawKeyval, guint baseKeyval, GdkModifierType state) {
    if (!visible_ || IsModifierKey(rawKeyval)) return false;
    // While the input method is composing (注音, 倉頡...), Enter / arrows / Esc belong to it.
    if (preedit_ && mode_ != Mode::Detail) return false;
    if (state & (GDK_SUPER_MASK | GDK_HYPER_MASK | GDK_META_MASK)) return false;
    FinishPendingScope();
    const bool ctrl = (state & GDK_CONTROL_MASK) != 0;
    const bool shift = (state & GDK_SHIFT_MASK) != 0;
    const bool alt = (state & GDK_ALT_MASK) != 0;
    const bool plain = !ctrl && !shift && !alt;
    const guint keyval = NormalizeKey(rawKeyval);
    // Action shortcuts compare the unshifted key ("Ctrl+Shift+1", not "Ctrl+!"), on a Latin layout.
    shortcutKey_ = baseKeyval ? NormalizeKey(baseKeyval) : keyval;

    if (actionsOpen_) {
        const int count = selected_ >= 0 && selected_ < static_cast<int>(results_.size())
                              ? static_cast<int>(results_[selected_].actions.size())
                              : 0;
        switch (keyval) {
            case GDK_KEY_Escape:
                CloseActions();
                return true;
            case GDK_KEY_Up:
                actionSelected_ = std::max(0, actionSelected_ - 1);
                RequestRender();
                return true;
            case GDK_KEY_Down:
                actionSelected_ = std::max(0, std::min(count - 1, actionSelected_ + 1));
                RequestRender();
                return true;
            case GDK_KEY_Return:
                if (plain) {
                    RunAction(selected_, actionSelected_);
                    return true;
                }
                break;
            case GDK_KEY_k:
                if (ctrl && !shift && !alt) {
                    CloseActions();
                    return true;
                }
                break;
            case GDK_KEY_Tab:
                return true;
            default:
                break;
        }
        if (TryShortcut(keyval, ctrl, shift, alt)) return true;
        CloseActions();  // anything else goes back to typing
    }

    if (mode_ == Mode::Detail) return OnDetailKey(keyval, ctrl, shift, alt);

    switch (keyval) {
        case GDK_KEY_Escape:
            if (mode_ == Mode::Windows) SwitchMode(Mode::Root);
            else if (!Text().empty()) SetQuery(L"");
            else if (mode_ == Mode::Plugin) BackToRoot(L"");
            else Hide();
            return true;
        case GDK_KEY_Up:
        case GDK_KEY_Down:
            if (ctrl || alt) break;  // Ctrl+Shift+Up/Down reorder favorites (action shortcuts)
            Move(keyval == GDK_KEY_Up ? -1 : 1);
            return true;
        case GDK_KEY_Page_Up:
        case GDK_KEY_Page_Down: {
            const int page = std::max(1, static_cast<int>(ListViewHeight() / kRowHeight) - 1);
            Move(keyval == GDK_KEY_Page_Up ? -page : page);
            return true;
        }
        case GDK_KEY_Tab:
            if (selected_ >= 0 && selected_ < static_cast<int>(results_.size()) &&
                !results_[selected_].autocomplete.empty())
                SetQuery(results_[selected_].autocomplete);
            return true;
        case GDK_KEY_k:
            if (ctrl && !shift && !alt) {
                OpenActions();
                return true;
            }
            break;
        case GDK_KEY_n:
        case GDK_KEY_p:
            if (ctrl && !shift && !alt) {
                Move(keyval == GDK_KEY_n ? 1 : -1);
                return true;
            }
            break;
        case GDK_KEY_Return:
            if (plain) {
                RunAction(selected_, 0);
                return true;
            }
            break;
        case GDK_KEY_BackSpace:
            if (plain && Text().empty() && mode_ != Mode::Root) {
                GoBack();  // Plugin: back to "keyword" in the root search
                return true;
            }
            break;
        default:
            break;
    }
    return TryShortcut(keyval, ctrl, shift, alt);  // anything else is typing / editing in the text field
}

bool LauncherWindow::OnDetailKey(guint keyval, bool ctrl, bool shift, bool alt) {
    const double page = ListViewHeight() - kDetailStep;
    switch (keyval) {
        case GDK_KEY_Escape:
        case GDK_KEY_BackSpace:
            BackFromDetail();
            return true;
        case GDK_KEY_Up:
            ScrollDetail(-kDetailStep);
            return true;
        case GDK_KEY_Down:
            ScrollDetail(kDetailStep);
            return true;
        case GDK_KEY_Page_Up:
            ScrollDetail(-page);
            return true;
        case GDK_KEY_Page_Down:
            ScrollDetail(page);
            return true;
        case GDK_KEY_space:
            ScrollDetail(shift ? -page : page);
            return true;
        case GDK_KEY_Home:
            ScrollDetail(-1.0e9);
            return true;
        case GDK_KEY_End:
            ScrollDetail(1.0e9);
            return true;
        case GDK_KEY_Return:
            if (!ctrl && !shift && !alt) {
                RunAction(0, 0);
                return true;
            }
            break;
        case GDK_KEY_k:
            if (ctrl && !shift && !alt) {
                OpenActions();
                return true;
            }
            break;
        default:
            break;
    }
    if (TryShortcut(keyval, ctrl, shift, alt)) return true;
    // Ctrl+C / Ctrl+A copy or select what was marked with the mouse in the text.
    if (ctrl && !alt && (keyval == GDK_KEY_c || keyval == GDK_KEY_a) && gtk_widget_has_focus(detailView_)) return false;
    return true;  // nothing is typed while reading
}

// ---------------------------------------------------------------------------
// Mouse

int LauncherWindow::RowAt(double y) const {
    if (mode_ == Mode::Detail || rowsDirty_) return -1;
    GtkWidget* child = ChildAt(rows_, gtk_widget_get_width(rows_) / 2.0, y);
    return IndexOf(itemRows_, child);
}

void LauncherWindow::OnListMotion(double x, double y) {
    graphene_point_t in = GRAPHENE_POINT_INIT(static_cast<float>(x), static_cast<float>(y)), out;
    if (!gtk_widget_compute_point(rows_, window_, &in, &out)) return;
    if (std::fabs(out.x - pointerX_) < 0.5 && std::fabs(out.y - pointerY_) < 0.5) return;  // did not really move
    const bool first = pointerX_ < 0;
    pointerX_ = out.x;
    pointerY_ = out.y;
    if (first || actionsOpen_) return;
    const int row = RowAt(y);
    if (row >= 0) Select(row);
}

void LauncherWindow::OnListPressed(int button, double, double y) {
    if (actionsOpen_) {
        CloseActions();
        return;
    }
    const int row = RowAt(y);
    pressedRow_ = -1;
    if (row < 0) return;
    Select(row);
    if (button == GDK_BUTTON_SECONDARY) OpenActions();
    else if (button == GDK_BUTTON_PRIMARY) pressedRow_ = row;
}

void LauncherWindow::OnListReleased(int button, double, double y) {
    if (button != GDK_BUTTON_PRIMARY || pressedRow_ < 0) return;
    const int pressed = std::exchange(pressedRow_, -1);
    if (RowAt(y) != pressed) return;
    Later([this, pressed] { RunAction(pressed, 0); });
}

void LauncherWindow::OnActionsMotion(double x, double y) {
    // Only real pointer movement selects (not the panel appearing under a resting pointer).
    graphene_point_t in = GRAPHENE_POINT_INIT(static_cast<float>(x), static_cast<float>(y)), out;
    if (!gtk_widget_compute_point(actionsBox_, window_, &in, &out)) return;
    if (std::fabs(out.x - pointerX_) < 0.5 && std::fabs(out.y - pointerY_) < 0.5) return;
    const bool first = pointerX_ < 0;
    pointerX_ = out.x;
    pointerY_ = out.y;
    if (first) return;
    const int index = IndexOf(actionRows_, ChildAt(actionsBox_, gtk_widget_get_width(actionsBox_) / 2.0, y));
    if (index >= 0 && index != actionSelected_) {
        actionSelected_ = index;
        RequestRender();
    }
}

void LauncherWindow::OnActionsReleased(double y) {
    const int index = IndexOf(actionRows_, ChildAt(actionsBox_, gtk_widget_get_width(actionsBox_) / 2.0, y));
    if (index < 0) return;
    const int item = selected_;
    Later([this, item, index] {
        if (actionsOpen_ && item == selected_) RunAction(item, index);
    });
}

void LauncherWindow::OnBackdropPressed(double x, double y) {
    graphene_rect_t bounds;
    if (!gtk_widget_compute_bounds(panel_, backdrop_, &bounds)) return;
    const graphene_point_t p = GRAPHENE_POINT_INIT(static_cast<float>(x), static_cast<float>(y));
    if (!graphene_rect_contains_point(&bounds, &p)) Later([this] { Hide(); });  // clicked outside the panel
}

// ---------------------------------------------------------------------------
// Timers

void LauncherWindow::StartTimer(guint& id, guint ms, void (LauncherWindow::*fn)()) {
    StopTimer(id);
    auto* data = new TimerData{this, fn, &id};
    id = g_timeout_add_full(
        G_PRIORITY_DEFAULT, ms,
        [](gpointer p) -> gboolean {
            auto* d = static_cast<TimerData*>(p);
            *d->id = 0;  // one-shot; fn may start it again
            (d->self->*d->fn)();
            return G_SOURCE_REMOVE;
        },
        data, [](gpointer p) { delete static_cast<TimerData*>(p); });
}

void LauncherWindow::StopTimer(guint& id) {
    if (id) g_source_remove(id);
    id = 0;
}

void LauncherWindow::OnStatusTimer() {
    status_.clear();
    statusWarning_ = false;
    Disarm();
    RequestRender();
}

void LauncherWindow::OnWindowsTimer() {
    FetchWindows();
    RefreshResults(true);
    RequestRender();
}

void LauncherWindow::OnPluginTimer() {
    if (mode_ == Mode::Plugin) StartPluginQuery(Text());
}

void LauncherWindow::OnBusyTimer() {
    if (pluginBusy_) {
        busyShown_ = true;
        RequestRender();
    }
}

// ---------------------------------------------------------------------------
// Drawing

void LauncherWindow::RequestRender() {
    if (!window_ || renderSource_) return;
    renderSource_ = g_idle_add_full(
        G_PRIORITY_HIGH_IDLE + 10,  // before GTK lays out and paints the next frame
        [](gpointer self) -> gboolean {
            auto* l = static_cast<LauncherWindow*>(self);
            l->renderSource_ = 0;
            l->Render();
            return G_SOURCE_REMOVE;
        },
        this, nullptr);
}

void LauncherWindow::Render() {
    if (renderSource_) {
        g_source_remove(renderSource_);
        renderSource_ = 0;
    }
    if (!window_) return;
    UpdateChrome();
    if (mode_ == Mode::Detail) {
        gtk_stack_set_visible_child_name(GTK_STACK(stack_), "detail");
        if (detailDirty_) {
            detailDirty_ = false;
            GtkTextBuffer* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(detailView_));
            gtk_text_buffer_set_text(buffer, U8(detailText_).c_str(), -1);
            GtkTextIter start;
            gtk_text_buffer_get_start_iter(buffer, &start);
            gtk_text_buffer_place_cursor(buffer, &start);
            gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(detailScroll_)), 0);
        }
        const bool empty = Trim(detailText_).empty();
        gtk_label_set_text(GTK_LABEL(emptyLabel_), "（沒有內容）");
        gtk_widget_set_visible(emptyLabel_, empty);
    } else {
        gtk_stack_set_visible_child_name(GTK_STACK(stack_), "list");
        if (rowsDirty_) RebuildRows();
        UpdateSelection();
        std::wstring text;
        if (results_.empty()) {
            text = L"沒有結果";
            if (mode_ == Mode::Windows) text = L"找不到符合的視窗";
            else if (mode_ == Mode::Plugin && pluginBusy_) text = busyShown_ ? L"載入中…" : L"";
            else if (mode_ == Mode::Plugin && pluginSilent_) text = L"外掛沒有輸出任何內容（可在「外掛偵錯紀錄」查看 stderr）";
            else if (mode_ == Mode::Root && app_.indexing()) text = L"正在建立應用程式索引…";
        }
        gtk_label_set_text(GTK_LABEL(emptyLabel_), U8(text).c_str());
        gtk_widget_set_visible(emptyLabel_, !text.empty());
    }
    UpdateActionsPanel();
}

void LauncherWindow::RebuildRows() {
    rowsDirty_ = false;
    RemoveChildren(rows_);
    itemRows_.assign(results_.size(), nullptr);
    for (const auto& e : entries_) {
        GtkWidget* w = nullptr;
        if (e.item < 0) {
            w = Label(e.header, "beam-section");
        } else {
            w = MakeRow(results_[e.item]);
            itemRows_[e.item] = w;
        }
        gtk_box_append(GTK_BOX(rows_), w);
    }
    scrollPending_ = true;
}

void LauncherWindow::UpdateSelection() {
    for (size_t i = 0; i < itemRows_.size(); ++i) {
        if (!itemRows_[i]) continue;
        if (static_cast<int>(i) == selected_) gtk_widget_add_css_class(itemRows_[i], "selected");
        else gtk_widget_remove_css_class(itemRows_[i], "selected");
    }
    if (scrollPending_) ApplyScroll();
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

void LauncherWindow::UpdateChrome() {
    const std::wstring chip = ChipText();
    gtk_widget_set_visible(chip_, !chip.empty());
    gtk_label_set_text(GTK_LABEL(chipLabel_), U8(chip).c_str());
    gtk_text_set_placeholder_text(GTK_TEXT(text_), U8(Placeholder()).c_str());
    const bool editable = mode_ != Mode::Detail;
    if (gtk_editable_get_editable(GTK_EDITABLE(text_)) != editable) gtk_editable_set_editable(GTK_EDITABLE(text_), editable);
    if (editable) gtk_widget_remove_css_class(text_, "readonly");
    else gtk_widget_add_css_class(text_, "readonly");

    const bool custom = !status_.empty();
    gtk_label_set_text(GTK_LABEL(statusLabel_), U8(custom ? status_ : DefaultStatus()).c_str());
    if (custom && statusWarning_) gtk_widget_add_css_class(statusLabel_, "warning");
    else gtk_widget_remove_css_class(statusLabel_, "warning");

    const bool hasPrimary = selected_ >= 0 && selected_ < static_cast<int>(results_.size()) &&
                            !results_[selected_].actions.empty();
    gtk_widget_set_visible(primaryBox_, hasPrimary);
    if (hasPrimary) gtk_label_set_text(GTK_LABEL(primaryLabel_), U8(results_[selected_].actions[0].title).c_str());
}

void LauncherWindow::UpdateActionsPanel() {
    const bool show = actionsOpen_ && selected_ >= 0 && selected_ < static_cast<int>(results_.size()) &&
                      !results_[selected_].actions.empty();
    gtk_widget_set_visible(actionsPanel_, show);
    if (!show) {
        actionsDirty_ = true;
        return;
    }
    const ResultItem& item = results_[selected_];
    if (actionsDirty_) {
        actionsDirty_ = false;
        gtk_label_set_text(GTK_LABEL(actionsHeader_), U8(item.title).c_str());
        RemoveChildren(actionsBox_);
        actionRows_.clear();
        for (size_t i = 0; i < item.actions.size(); ++i) {
            const Action& a = item.actions[i];
            GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
            gtk_widget_add_css_class(row, "beam-action");
            if (a.dangerous) gtk_widget_add_css_class(row, "dangerous");
            GtkWidget* image = SymbolicImage(a.icon && *a.icon ? a.icon : icon::kPlay, 16);
            gtk_widget_set_margin_end(image, 12);
            gtk_box_append(GTK_BOX(row), image);
            GtkWidget* title = Label(a.title, "beam-action-title");
            gtk_widget_set_hexpand(title, TRUE);
            gtk_box_append(GTK_BOX(row), title);
            const std::vector<std::wstring> keys = i == 0 ? std::vector<std::wstring>{L"Enter"} : a.shortcut.Parts();
            if (!keys.empty()) {
                GtkWidget* caps = MakeKeycaps(keys);
                gtk_widget_set_margin_start(caps, 8);
                gtk_box_append(GTK_BOX(row), caps);
            }
            gtk_box_append(GTK_BOX(actionsBox_), row);
            actionRows_.push_back(row);
        }
    }
    actionSelected_ = std::clamp(actionSelected_, 0, std::max(0, static_cast<int>(actionRows_.size()) - 1));
    for (size_t i = 0; i < actionRows_.size(); ++i) {
        if (static_cast<int>(i) == actionSelected_) gtk_widget_add_css_class(actionRows_[i], "selected");
        else gtk_widget_remove_css_class(actionRows_[i], "selected");
    }
    // keep the selected action in view
    GtkAdjustment* adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(actionsScroll_));
    const double rowHeight = 34, top = actionSelected_ * rowHeight, bottom = top + rowHeight;
    const double value = gtk_adjustment_get_value(adj), page = gtk_adjustment_get_page_size(adj);
    if (page > 0) {
        if (top < value) gtk_adjustment_set_value(adj, top);
        else if (bottom > value + page) gtk_adjustment_set_value(adj, bottom - page);
    }
}

// ---------------------------------------------------------------------------
// Test hook: `beam --dump-state` prints this from the running instance.

std::wstring LauncherWindow::DescribeState() const {
    std::wstring out;
    out += L"visible=" + std::wstring(visible_ ? L"1" : L"0") + L" active=" + (IsActive() ? L"1" : L"0") +
           L" mode=" + Utf8ToWide(ModeName(mode_)) + L" query=\"" + Text() + L"\" selected=" +
           std::to_wstring(selected_) + L" actions=" + (actionsOpen_ ? L"open" : L"closed") + L"\n";
    out += L"chip=\"" + ChipText() + L"\" placeholder=\"" + Placeholder() + L"\"\n";
    out += L"status=\"" + (status_.empty() ? DefaultStatus() : status_) + L"\"" + (statusWarning_ ? L" (warning)" : L"") +
           L"\n";
    if (mode_ == Mode::Detail) {
        out += L"detail: " + detailTitle_ + L"\n" + detailText_.substr(0, 2000) + L"\n";
    }
    std::wstring section;
    for (size_t i = 0; i < results_.size(); ++i) {
        const ResultItem& r = results_[i];
        if (i == 0 || r.section != section) {
            section = r.section;
            out += L"[" + section + L"]\n";
        }
        out += (static_cast<int>(i) == selected_ ? L" > " : L"   ") + r.title;
        if (!r.subtitle.empty()) out += L" | " + r.subtitle;
        if (!r.accessory.empty()) out += L" | " + r.accessory;
        out += L"\n";
    }
    if (actionsOpen_ && selected_ >= 0 && selected_ < static_cast<int>(results_.size())) {
        out += L"actions:\n";
        const auto& actions = results_[selected_].actions;
        for (size_t i = 0; i < actions.size(); ++i) {
            out += (static_cast<int>(i) == actionSelected_ ? L" > " : L"   ") + actions[i].title;
            std::wstring keys;
            for (const auto& p : (i == 0 ? std::vector<std::wstring>{L"Enter"} : actions[i].shortcut.Parts()))
                keys += (keys.empty() ? L"" : L"+") + p;
            if (!keys.empty()) out += L" [" + keys + L"]";
            out += L"\n";
        }
    }
    return out;
}

}  // namespace beam
