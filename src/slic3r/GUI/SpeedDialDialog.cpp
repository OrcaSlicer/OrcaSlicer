#include "SpeedDialDialog.hpp"

#include "ActionRegistry.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "NotificationManager.hpp"
#include "Plater.hpp"
#include "Widgets/WebViewHostDialog.hpp"

#include <algorithm>

#include <wx/display.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

namespace Slic3r { namespace GUI {

namespace {

// ADJUST WIDTH HERE (DIP px). Fixed dialog width; was 360, now 1.5x. Height is not set here -
// the dialog auto-resizes to the page content (see resize_to_content + the list max-height in style.css).
constexpr int kPopupWidth = 540;
constexpr int kPopupMinHeight = 60;   // just above the bare search-bar height, so the dialog hugs content
constexpr int kPopupMaxHeight = 282;

int json_int_or(const nlohmann::json& j, const char* key, int fallback)
{
    auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<int>() : fallback;
}

wxColour bg_color() { return wxGetApp().get_window_default_clr(); }

}

SpeedDialWebDialog::SpeedDialWebDialog(wxWindow* parent)
    : WebViewHostDialog(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                        wxBORDER_NONE | wxFRAME_NO_TASKBAR)
{
    SetBackgroundColour(bg_color());
    Bind(wxEVT_ACTIVATE, [this](wxActivateEvent& event) {
        if (!event.GetActive() && IsShown())
            Hide();
        event.Skip();
    });
    if (!create_webview("web/dialog/SpeedDial/index.html", wxEmptyString,
                       wxSize(kPopupWidth, kPopupMaxHeight), wxSize(kPopupWidth, kPopupMinHeight))) {
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(new wxStaticText(this, wxID_ANY, wxS("wxWebView unavailable")),
                   wxSizerFlags().Border(wxALL, 20));
        SetSizer(sizer);
        SetClientSize(FromDIP(wxSize(kPopupWidth, kPopupMinHeight)));
    }
}

SpeedDialWebDialog::~SpeedDialWebDialog() { m_alive->store(false, std::memory_order_release); }

void SpeedDialWebDialog::request_show()
{
    if (IsShown()) {
        Raise();
        if (browser())
            browser()->SetFocus();
        return;
    }

    Show();
    Raise();
    if (m_page_ready)
        send_actions();
    if (browser())
        browser()->SetFocus();
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
    }
    else if (command == "toggle_favourite")
        wxGetApp().action_registry().set_favourite(payload.value("id", ""), payload.value("fav", false));
    else if (command == "reorder_favourites") {
        std::vector<std::string> ids;
        if (payload.contains("ids") && payload["ids"].is_array())
            for (const auto& id : payload["ids"])
                if (id.is_string())
                    ids.push_back(id.get<std::string>());
        wxGetApp().action_registry().reorder_favourites(ids);
    }
    else if (command == "run_action")
        run_action(payload.value("id", ""), payload.value("title", ""));
    else if (command == "resize")
        resize_to_content(json_int_or(payload, "height", 0));
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
}

void SpeedDialWebDialog::run_action(const std::string& id, const std::string& title)
{
    ActionRegistry& reg = wxGetApp().action_registry();
    const AppAction* a = reg.by_id(id);
    if (!a)
        return;

    const bool        ask    = reg.should_ask(id);
    const std::string atitle = a->title();
    if (IsModal())
        EndModal(wxID_CANCEL);
    else
        Hide();

    if (ask) {
        const wxString label = title.empty() ? from_u8(atitle) : from_u8(title);
        RichMessageDialog dlg(wxGetApp().mainframe, wxString::Format(_L("Run \"%s\"?"), label),
                              _L("Run plugin"), wxOK | wxCANCEL);
        dlg.ShowCheckBox(_L("Don't ask again for this action"));
        if (dlg.ShowModal() != wxID_OK)
            return;
        if (dlg.IsCheckBoxChecked())
            wxGetApp().action_registry().suppress_ask(id);
    }

    wxGetApp().CallAfter([id] {
        if (wxGetApp().is_closing())
            return;
        AppActionRunResult result = wxGetApp().action_registry().run(id);
        if (result.level == AppActionRunResult::Level::Busy)
            return;
        if (!result.message.IsEmpty() && wxGetApp().plater())
            wxGetApp().plater()->get_notification_manager()->push_notification(
                NotificationType::CustomNotification,
                result.level == AppActionRunResult::Level::Error ? NotificationManager::NotificationLevel::ErrorNotificationLevel :
                                                                    NotificationManager::NotificationLevel::RegularNotificationLevel,
                into_u8(result.message));
    });
}

void SpeedDialWebDialog::send_actions()
{
    nlohmann::json snap = wxGetApp().action_registry().snapshot();
    call_web_handler({{"command", "list_actions"},
                      {"actions", std::move(snap["actions"])},
                      {"favourites", std::move(snap["favourites"])}});
}

}}
