#include "SpeedDialDialog.hpp"

#include "ActionRegistry.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "NotificationManager.hpp"
#include "Plater.hpp"
#include "Widgets/WebViewHostDialog.hpp"

#include "slic3r/Utils/MacDarkMode.hpp"

#include <algorithm>

#include <wx/dcmemory.h>
#include <wx/display.h>
#include <wx/region.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/utils.h>

#ifdef __linux__
#include <gtk/gtk.h>
#endif

namespace Slic3r { namespace GUI {

namespace {

// ADJUST WIDTH HERE (DIP px). Fixed dialog width; was 360, now 1.5x. Height is not set here -
// the dialog auto-resizes to the page content (see resize_to_content + the list max-height in style.css).
constexpr int kPopupWidth     = 540;
constexpr int kPopupMinHeight = 60; // just above the bare search-bar height, so the dialog hugs content
constexpr int kPopupMaxHeight = 282;

int json_int_or(const nlohmann::json& j, const char* key, int fallback)
{
    auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<int>() : fallback;
}

// Display name of a settings mode, for the mode-switch confirmation.
wxString mode_label(ConfigOptionMode mode)
{
    switch (mode) {
    case comAdvanced: return _L("Advanced");
    case comExpert: return _L("Expert");
    case comDevelop: return _L("Developer");
    default: return _L("Simple");
    }
}

wxColour bg_color() { return wxGetApp().get_window_default_clr(); }

// Give the WebKitGTK widget itself input focus, not its GtkScrolledWindow container.
// (browser()->SetFocus() grabs focus on the container and doesn't reach the web content,
// so typing only works after the user clicks.) On Linux the native backend is the
// WebKitWebView widget; grab focus there directly. Elsewhere SetFocus() is correct.
void focus_webview(wxWebView* browser, bool page_ready)
{
    if (!browser)
        return;
#ifdef __linux__
    if (void* nb = browser->GetNativeBackend())
        gtk_widget_grab_focus((GtkWidget*) nb);
#else
    browser->SetFocus();
#endif
    if (page_ready)
        browser->RunScript("focusInput();");
}

// Localized strings for the Speed Dial page, injected as a document-start user script. The page's
// T() reads window.ORCA_UI_STRINGS, so these flow through the same .po pipeline as the rest of the
// UI (the JS literals are only a fallback before the script runs / in the node vm test).
// %% is a literal '%': T() collapses it after substituting %s. Keep the shortcut tokens out of the
// translated text so the platform prefix (Alt+/⌥+, Ctrl+/⌘+) stays correct.
nlohmann::json speed_dial_ui_strings()
{
    const std::string alt  = GUI::shortkey_alt_prefix();
    const std::string ctrl = GUI::shortkey_ctrl_prefix();
    return {
        {"shortcut_alt", alt},
        {"shortcut_ctrl", ctrl},

        {"sd_search", _u8L("Search actions")},
        {"sd_clear", _u8L("Clear")},
        {"sd_recent", _u8L("Recent")},
        {"sd_plugins", _u8L("Plugins")},
        {"sd_other", _u8L("Other")},
        {"sd_no_match_total", _u8L("No actions match (Total: %s)")},
        {"sd_no_actions", _u8L("No actions yet")},
        {"sd_no_tabs_match", _u8L("No tabs match")},
        {"sd_no_tabs", _u8L("No tabs")},
        {"sd_result_count", _u8L("Showing %s actions")},
        {"sd_result_count_all", _u8L("%s actions")},
        {"sd_tab_count", _u8L("%s tabs")},
        {"sd_tab_match_count", _u8L("%s matches")},
        {"sd_favs_full", _u8L("Favourites are full (%s max)")},
        {"sd_go_to_pct", _u8L("Go to %s%% of the layer range")},
        {"sd_enter_pct", _u8L("Enter a layer percentage (0-100)")},
        {"sd_go_layer_ph", _u8L("Go to layer %% (0-100)")},
        {"sd_go_tab_ph", _u8L("Go to tab")},
        {"sd_fav_slot", _u8L("Favourite %s (%s)")},
        {"sd_pin_fav", _u8L("Pin to favourites (%s)")},
        {"sd_unpin_fav", _u8L("Unpin from favourites (%s)")},
        {"sd_remove_fav", _u8L("Remove from favourites")},
        {"sd_move_left", _u8L("Move left")},
        {"sd_move_right", _u8L("Move right")},
        {"sd_unpin", _u8L("Unpin")},
        {"sd_mode_advanced", _u8L("Advanced")},
        {"sd_mode_expert", _u8L("Expert")},
        {"sd_mode_develop", _u8L("Developer")},
        {"sd_wiki_f1", _u8L("Wiki (F1)")},
        {"sd_no_wiki", _u8L("No wiki page for this action")},
        {"sd_show_details", _u8L("Show details")},
        {"sd_hide_details", _u8L("Hide details")},
    };
}

} // namespace

SpeedDialWebDialog::SpeedDialWebDialog(wxWindow* parent)
    : WebViewHostDialog(parent,
                        wxID_ANY,
                        wxEmptyString,
                        wxDefaultPosition,
                        wxDefaultSize,
                        wxBORDER_NONE | wxFRAME_NO_TASKBAR | wxFRAME_FLOAT_ON_PARENT | wxFRAME_SHAPED)
{
    SetBackgroundColour(bg_color());
    Bind(wxEVT_ACTIVATE, [this](wxActivateEvent& event) {
        // Focus the WebKit widget exactly when the WM makes the popup the active window
        // (modeless focus is granted asynchronously, so a focus request made right after
        // Show() is dropped). Also re-corrects focus on every re-open.
        if (event.GetActive() && IsShown())
            focus_webview(browser(), m_page_ready);
        else if (!event.GetActive() && IsShown())
            Hide();
        event.Skip();
    });
    if (!create_webview("web/dialog/SpeedDial/index.html", wxEmptyString, wxSize(kPopupWidth, kPopupMaxHeight),
                        wxSize(kPopupWidth, kPopupMinHeight))) {
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(new wxStaticText(this, wxID_ANY, wxS("wxWebView unavailable")), wxSizerFlags().Border(wxALL, 20));
        SetSizer(sizer);
        SetClientSize(FromDIP(wxSize(kPopupWidth, kPopupMinHeight)));
    }
    // WebView2's browser accelerator keys include Ctrl +/-/0 and Ctrl+wheel zoom, which would resize
    // the page inside the fixed-size popup. No-op on the other backends (wxWidgets 3.3 base virtual).
    if (wxWebView* wv = browser())
        wv->EnableBrowserAcceleratorKeys(false);
    // Re-cut the shape whenever layout changes the client size. wxOSX SetShape resizes the
    // NSWindow, which fires this synchronously; apply_rounded_shape() guards re-entry.
    Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        event.Skip();
        apply_rounded_shape();
    });
    apply_rounded_shape();
}

SpeedDialWebDialog::~SpeedDialWebDialog() { m_alive->store(false, std::memory_order_release); }

// Document-start hook: hand the page its translated strings before speeddial.js runs, so the first
// paint is already localized. The table is built when the dialog is created; a live language switch
// rebuilds the GUI (and with it this dialog), so the next open re-injects the new locale.
void SpeedDialWebDialog::add_user_scripts()
{
    if (wxWebView* wv = browser()) {
        const std::string js = "window.ORCA_UI_STRINGS = " +
                               speed_dial_ui_strings().dump(-1, ' ', false, nlohmann::json::error_handler_t::ignore) + ";";
        wv->AddUserScript(wxString::FromUTF8(js));
    }
}

void SpeedDialWebDialog::request_show()
{
    if (IsShown()) {
        Raise();
        focus_webview(browser(), m_page_ready);
        repaint_webview();
        return;
    }

    Show();
    Raise();
    apply_rounded_shape();
    if (m_page_ready)
        send_actions();
    // Grab focus now and again on wxEVT_ACTIVATE; grabbing directly on the WebKit widget is
    // what makes typing reach the search field immediately on open.
    focus_webview(browser(), m_page_ready);
    repaint_webview();
}

void SpeedDialWebDialog::on_script_message(const nlohmann::json& payload)
{
    if (handle_common_script_command(payload))
        return;

    // Defer command handling out of the webview script-message callback: GTK and macOS deliver
    // it synchronously inside the native webview callback, and window work on that stack is the
    // crash class fixed in b779a7bfed/f2ccbfc8b5 (see PluginsDialog::on_script_message).
    // run_action puts a modal confirm on that stack, which is the same bug.
    wxGetApp().CallAfter([this, alive = m_alive, payload]() {
        if (alive->load(std::memory_order_acquire))
            handle_web_command(payload);
    });
}

void SpeedDialWebDialog::handle_web_command(const nlohmann::json& payload)
{
    const std::string command = payload.value("command", "");
    if (command == "request_actions") {
        m_page_ready = true;
        send_actions();
    } else if (command == "toggle_favourite") {
        // set_favourite() refuses once the bar hits kFavLimit; tell the page so it can undo the
        // pin and show a "favourites are full" hint instead of silently losing the favourite.
        const std::string fav_id = payload.value("id", "");
        const bool ok            = wxGetApp().action_registry().set_favourite(fav_id, payload.value("fav", false));
        if (!ok)
            call_web_handler({{"command", "favourite_full"}, {"limit", (int) ActionRegistry::kFavLimit}, {"id", fav_id}});
    } else if (command == "reorder_favourites") {
        std::vector<std::string> ids;
        if (payload.contains("ids") && payload["ids"].is_array())
            for (const auto& id : payload["ids"])
                if (id.is_string())
                    ids.push_back(id.get<std::string>());
        wxGetApp().action_registry().reorder_favourites(ids);
    } else if (command == "set_tooltip_expanded")
        wxGetApp().action_registry().set_tooltip_expanded(payload.value("expanded", true));
    else if (command == "run_action")
        run_action(payload.value("id", ""), payload.value("title", ""), payload.value("param", ""));
    else if (command == "open_wiki")
        open_wiki(payload.value("id", ""));
    else if (command == "search_tabs")
        search_tabs();
    else if (command == "resize")
        resize_to_content(json_int_or(payload, "height", 0));
}

void SpeedDialWebDialog::search_tabs()
{
    // Round-trip is async because the webview delivers script messages synchronously on the
    // GTK/macOS stack; defer the (cheap) enumeration and push the result back to the page.
    wxGetApp().CallAfter([this, alive = m_alive]() {
        if (!alive->load(std::memory_order_acquire))
            return;
        auto tabs = wxGetApp().action_registry().tab_options();
        call_web_handler({{"command", "tab_results"}, {"tabs", std::move(tabs)}});
    });
}

void SpeedDialWebDialog::resize_to_content(int height)
{
    if (height <= 0)
        return;

    int display_index = wxDisplay::GetFromWindow(this);
    if (display_index == wxNOT_FOUND)
        display_index = 0;
    const int screen_dip = ToDIP(wxDisplay(display_index).GetClientArea().GetHeight());
    const int max_dip    = std::max(kPopupMinHeight, screen_dip * 85 / 100);
    const int height_dip = std::max(kPopupMinHeight, std::min(height, max_dip));
    SetClientSize(FromDIP(wxSize(kPopupWidth, height_dip)));
    Layout();
#ifdef __WXOSX__
    // WKWebView can lag the dialog's new client size; force the viewport to match so the page is
    // never painted (and clipped by the rounded layer) below the footer. Unconditional: on a
    // re-open the size is often unchanged, and skipping the sync leaves the fresh render unpainted.
    if (wxWebView* wv = browser())
        wv->SetSize(GetClientSize());
#endif
    apply_rounded_shape();
    // A re-open re-renders at (usually) the same size, so nothing above may generate damage.
    // Repaint explicitly so the newly rendered list is shown without needing user input.
    repaint_webview();
}

void SpeedDialWebDialog::repaint_webview()
{
    wxWebView* wv = browser();
    if (!wv)
        return;
    // Portable invalidate; the platform blocks below reach the widget/layer that actually paints.
    wv->Refresh();
#ifdef __WXOSX__
    if (void* nb = wv->GetNativeBackend())
        WKWebView_force_display(nb);
    wv->Update();
#elif defined(__linux__)
    // WebKitGTK's WebKitWebView owns its own GdkWindow, so invalidating the wxWebView wrapper
    // (the GtkScrolledWindow) does not redraw it.
    if (void* nb = wv->GetNativeBackend())
        gtk_widget_queue_draw((GtkWidget*) nb);
#else
    wv->Update();
#endif
}

// Rounded corners: the webview paints an opaque rectangle, so round the whole top-level window.
// GTK/MSW use a shape region (same mask trick as FilamentPickerDialog, binary edges, no
// anti-aliasing); macOS clips the native view layer instead, since SetShape cannot shape there.
void SpeedDialWebDialog::apply_rounded_shape()
{
    // wxOSX SetShape resizes the NSWindow (setContentSize 10x10 then back), which synchronously
    // fires wxEVT_SIZE -> apply_rounded_shape() -> SetShape() and recurses until the stack
    // overflows. GTK/MSW set a region without resizing, so they are unaffected.
    if (m_applying_shape)
        return;

    // BORDER_NONE means the window is all client area, so the client size is the shape size.
    const wxSize size = GetClientSize();
    if (size.GetWidth() <= 0 || size.GetHeight() <= 0)
        return;

    m_applying_shape = true;

#ifdef __WXOSX__
    // wxOSX ignores the region (it only clears the window background), so round the native view.
    set_window_corner_radius(this, FromDIP(m_corner_radius));
#else
    m_shape_bmp.Create(size.GetWidth(), size.GetHeight(), 32);
    if (m_shape_bmp.IsOk()) {
        wxMemoryDC dc;
        dc.SelectObject(m_shape_bmp);
        dc.SetBackground(wxBrush(wxColour(0, 0, 0)));
        dc.Clear();
        dc.SetBrush(wxBrush(wxColour(255, 255, 255, 255)));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRoundedRectangle(0, 0, size.GetWidth(), size.GetHeight(), FromDIP(m_corner_radius));
        dc.SelectObject(wxNullBitmap);

        wxRegion region(m_shape_bmp, wxColour(0, 0, 0));
        if (region.IsOk())
            SetShape(region);
    }
#endif

    m_applying_shape = false;
}

void SpeedDialWebDialog::on_dpi_changed(const wxRect&)
{
    apply_rounded_shape();
    Refresh();
}

void SpeedDialWebDialog::run_action(const std::string& id, const std::string& title, const std::string& param)
{
    ActionRegistry& reg = wxGetApp().action_registry();
    const AppAction* a  = reg.by_id(id);
    if (!a)
        return;

    // Only plugin actions get the "Run plugin?" confirm. Built-in commands act immediately.
    const bool ask                  = a->kind == AppActionKind::Plugin && reg.should_ask(id);
    const std::string atitle        = a->title();
    const ConfigOptionMode required = a->required_mode;

    // Settings the current mode hides require a switch first. Ask while the dial is still up; a
    // cancel dismisses both (the dial also auto-hides when the modal takes activation).
    if (requires_mode_switch(required, wxGetApp().get_mode())) {
        const wxString setting = title.empty() ? from_u8(atitle) : from_u8(title);
        if (required == comDevelop) {
            RichMessageDialog dlg(wxGetApp().mainframe,
                                  wxString::Format(_L("\"%s\" is a Developer setting. Enable Developer mode to edit it?"), setting),
                                  _L("Developer setting"), wxOK | wxCANCEL);
            if (dlg.ShowModal() != wxID_OK)
                return;
            wxGetApp().enable_developer_mode();
        } else {
            RichMessageDialog dlg(wxGetApp().mainframe,
                                  wxString::Format(_L("\"%s\" is a %s setting. Switch from %s mode to %s mode to edit it?"), setting,
                                                   mode_label(required), mode_label(wxGetApp().get_mode()), mode_label(required)),
                                  _L("Switch settings mode"), wxOK | wxCANCEL);
            if (dlg.ShowModal() != wxID_OK)
                return;
            wxGetApp().save_mode(required);
        }
    }

    if (IsModal())
        EndModal(wxID_CANCEL);
    else
        Hide();

    if (ask) {
        const wxString label = title.empty() ? from_u8(atitle) : from_u8(title);
        RichMessageDialog dlg(wxGetApp().mainframe, wxString::Format(_L("Run \"%s\"?"), label), _L("Run plugin"), wxOK | wxCANCEL);
        dlg.ShowCheckBox(_L("Don't ask again for this action"));
        if (dlg.ShowModal() != wxID_OK)
            return;
        if (dlg.IsCheckBoxChecked())
            wxGetApp().action_registry().suppress_ask(id);
    }

    wxGetApp().CallAfter([id, param] {
        if (wxGetApp().is_closing())
            return;
        AppActionRunResult result = wxGetApp().action_registry().run(id, param);
        if (result.level == AppActionRunResult::Level::Busy)
            return;
        if (!result.message.IsEmpty() && wxGetApp().plater())
            wxGetApp()
                .plater()
                ->get_notification_manager()
                ->push_notification(NotificationType::CustomNotification,
                                    result.level == AppActionRunResult::Level::Error ?
                                        NotificationManager::NotificationLevel::ErrorNotificationLevel :
                                        NotificationManager::NotificationLevel::RegularNotificationLevel,
                                    into_u8(result.message));
    });
}

void SpeedDialWebDialog::open_wiki(const std::string& id)
{
    const AppAction* a = wxGetApp().action_registry().by_id(id);
    if (!a || a->help_url.empty())
        return;
    Hide();
    wxLaunchDefaultBrowser(from_u8(a->help_url));
}

void SpeedDialWebDialog::send_actions()
{
    nlohmann::json snap = wxGetApp().action_registry().snapshot();
    call_web_handler({{"command", "list_actions"},
                      {"actions", std::move(snap["actions"])},
                      {"favourites", std::move(snap["favourites"])},
                      {"recent", std::move(snap["recent"])},
                      {"user_mode", std::move(snap["user_mode"])},
                      {"tooltip_expanded", std::move(snap["tooltip_expanded"])}});
}

}} // namespace Slic3r::GUI
