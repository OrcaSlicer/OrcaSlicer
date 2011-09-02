#include "OAuthDialog.hpp"

#include "GUI_App.hpp"
#include "Jobs/BoostThreadWorker.hpp"
#include "Jobs/PlaterWorker.hpp"
#include "wxExtensions.hpp"
#include "Widgets/DialogButtons.hpp"

namespace Slic3r {
namespace GUI {
	

#define BORDER_W FromDIP(10)

OAuthDialog::OAuthDialog(wxWindow* parent, OAuthParams params)
    : DPIDialog(parent, wxID_ANY, _L("Login"), wxDefaultPosition, wxSize(45 * wxGetApp().em_unit(), -1), wxDEFAULT_DIALOG_STYLE)
    , _params(params)
{
    SetFont(wxGetApp().normal_font());
    SetBackgroundColour(*wxWHITE);

    m_worker = std::make_unique<PlaterWorker<BoostThreadWorker>>(this, nullptr, "auth_worker");

    auto dlg_btns = new DialogButtons(this, {"Cancel"});
    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, &OAuthDialog::on_cancel, this);

    const auto message_sizer = new wxBoxSizer(wxVERTICAL);
    const auto message = new wxStaticText(this, wxID_ANY, _L("Authorizing..."), wxDefaultPosition, wxDefaultSize, 0);
    message->SetForegroundColour(*wxBLACK);
    message_sizer->Add(message, 0, wxEXPAND | wxLEFT | wxTOP | wxBOTTOM, BORDER_W);

    const auto topSizer = new wxBoxSizer(wxVERTICAL);
    topSizer->Add(message_sizer, 0, wxEXPAND | wxALL, BORDER_W);
    topSizer->Add(dlg_btns, 0, wxEXPAND);

    Bind(wxEVT_CLOSE_WINDOW, &OAuthDialog::on_cancel, this);

    SetSizer(topSizer);
    topSizer->SetSizeHints(this);
    this->CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

void OAuthDialog::on_cancel(wxEvent& event)
{
    m_worker->cancel_all();
    m_worker->wait_for_idle();
    EndModal(wxID_NO);
}

bool OAuthDialog::Show(bool show)
{
    if (show) {
        // Prepare login job
        _result        = std::make_shared<OAuthResult>();
        auto job = std::make_unique<OAuthJob>(OAuthData{_params, _result});
        job->set_event_handle(this);
        Bind(EVT_OAUTH_COMPLETE_MESSAGE, [this](wxCommandEvent& evt) { EndModal(wxID_NO); });

        // Start auth job
        replace_job(*m_worker, std::move(job));

        // Open login URL in external browser
        wxLaunchDefaultBrowser(_params.login_url);
    }

    return DPIDialog::Show(show);
}

void OAuthDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    const int& em = em_unit();

    msw_buttons_rescale(this, em, {wxID_CANCEL});

    const wxSize& size = wxSize(45 * em, 35 * em);
    SetMinSize(size);

    Fit();
    Refresh();
}

}}
