#ifndef slic3r_GUI_SpeedDialDialog_hpp_
#define slic3r_GUI_SpeedDialDialog_hpp_

#include <slic3r/GUI/Widgets/WebViewHostDialog.hpp>
#include <nlohmann/json_fwd.hpp>
#include <atomic>
#include <memory>
#include <string>

#include <wx/bitmap.h>
#include <wx/gdicmn.h>
#include <wx/timer.h>

class wxWindow;

namespace Slic3r { namespace GUI {

// Top-left origin of the dial: a `box_size` rectangle centred on the parent's current screen rect,
// clamped so it stays inside the display work area (negative monitor origins are valid). Callers pass
// the fixed-width, maximum-height box rather than the current client size, so the content-driven
// resize in resize_to_content() never moves the window.
wxPoint speed_dial_origin(const wxRect& parent_screen_rect, const wxRect& work_area, const wxSize& box_size);

// Whether a deactivation really dismissed the dial: only when it is still shown and no longer holds the
// focus. Evaluated after the settle delay, never on the event itself, so a focus bounce shorter than the
// delay is ignored.
bool speed_dial_should_hide(bool shown, bool active);

class SpeedDialWebDialog : public WebViewHostDialog
{
public:
    explicit SpeedDialWebDialog(wxWindow* parent);
    ~SpeedDialWebDialog() override;
    void request_show();

private:
    nlohmann::json localized_strings() const override;
    void on_script_message(const nlohmann::json& payload) override;
    void handle_web_command(const nlohmann::json& payload);
    // Re-derives the origin from the parent's current screen rect and moves the window there. Called
    // on every open, since the dialog is a long-lived hidden window: without this the coordinates
    // stay at the frame's position from the first open (frozen at the first map by the WM).
    void position_over_parent();
    // Queues the auto-hide check that runs kHideSettleMs later; cancelled by any re-activation or
    // re-open. A check that fires after the dial was hidden on purpose is a no-op.
    void schedule_hide();
    void on_hide_timer(wxTimerEvent& event);
    // Whether the dial is the window that currently holds the keyboard focus. On wxGTK IsActive() answers
    // from wx's own bookkeeping (g_activeFrame), which the deactivation that armed the timer has already
    // cleared, so ask the window itself instead.
    bool is_active();
    void resize_to_content(int height);
    void run_action(const std::string& id, const std::string& title, const std::string& param = "");
    void open_wiki(const std::string& id);
    void send_actions();
    void search_tabs();
    void apply_rounded_shape();
    // Forces the webview to repaint after it is mapped / re-rendered. The popup is transparent and
    // chrome-less, so a missed frame leaves it blank until input; platform-specific because the
    // widget that actually paints is not always the wxWebView wrapper.
    void repaint_webview();
    void on_dpi_changed(const wxRect& suggested_rect) override;

    bool m_page_ready{false};
    // Rounded corners (shape region on GTK/MSW, native layer on macOS), since the webview is opaque.
    int m_corner_radius{7};
    wxBitmap m_shape_bmp;
    // wxOSX SetShape resizes the window, which re-enters apply_rounded_shape() through wxEVT_SIZE.
    bool m_applying_shape{false};
    // Guards the CallAfter in on_script_message across dialog destruction, same as
    // PluginsDialog::m_alive (PluginsDialog.hpp:249).
    std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);
    // Pending auto-hide, armed by a deactivation and validated when it fires (see schedule_hide).
    wxTimer m_hide_timer;
};

}}

#endif
