// MakerBot / UltiMaker Fork – Orca Slicer 2.4
// BirdwingHandshakeDialog.cpp

#include "BirdwingHandshakeDialog.hpp"
#include "slic3r/Utils/MakerbotLink.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"

#include <wx/sizer.h>
#include <wx/button.h>
#include <wx/statbmp.h>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include "libslic3r/Utils.hpp"   // resources_dir()

namespace Slic3r {
namespace GUI {

BirdwingHandshakeDialog::BirdwingHandshakeDialog(wxWindow* parent,
                                                 const MakerbotLink& link)
    : wxDialog(parent, wxID_ANY,
               _L("MakerBot Birdwing – Authorize Connection"),
               wxDefaultPosition, wxSize(480, 380),
               wxDEFAULT_DIALOG_STYLE | wxSTAY_ON_TOP)
    , m_link(link)
    , m_timer(this)
{
    SetFont(wxGetApp().normal_font());

    // ── Animated GIF ─────────────────────────────────────────────────────────
    const boost::filesystem::path gif_path =
        boost::filesystem::path(Slic3r::resources_dir()) /
        "profiles" / "MakerBot" / "makerbot_birdwing_handshake.gif";

    m_anim = new wxAnimationCtrl(this, wxID_ANY, wxNullAnimation,
                                  wxDefaultPosition, wxSize(300, 180));
    if (boost::filesystem::exists(gif_path) &&
        m_anim->LoadFile(wxString::FromUTF8(gif_path.string()), wxANIMATION_TYPE_GIF))
        m_anim->Play();
    else
        m_anim->Hide();

    // ── Status + Countdown ────────────────────────────────────────────────────
    m_status = new wxStaticText(this, wxID_ANY,
        _L("Please press the controller wheel on your MakerBot printer\n"
           "to authorize the connection with Orca Slicer."),
        wxDefaultPosition, wxDefaultSize, wxALIGN_CENTRE_HORIZONTAL);
    m_status->SetFont(m_status->GetFont().MakeLarger());
    m_status->Wrap(440);

    m_counter = new wxStaticText(this, wxID_ANY, "120",
                                  wxDefaultPosition, wxDefaultSize,
                                  wxALIGN_CENTRE_HORIZONTAL);
    {
        wxFont f = m_counter->GetFont();
        f.SetPointSize(f.GetPointSize() + 10);
        f.SetWeight(wxFONTWEIGHT_BOLD);
        m_counter->SetFont(f);
    }
    auto* seconds_label = new wxStaticText(this, wxID_ANY, _L("seconds remaining"),
                                            wxDefaultPosition, wxDefaultSize,
                                            wxALIGN_CENTRE_HORIZONTAL);

    auto* cancel_btn = new wxButton(this, wxID_CANCEL, _L("Cancel"));

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->AddSpacer(12);
    if (m_anim->IsShown())
        sizer->Add(m_anim,    0, wxALIGN_CENTER | wxALL, 8);
    sizer->Add(m_status,      0, wxALIGN_CENTER | wxLEFT | wxRIGHT, 16);
    sizer->AddSpacer(16);
    sizer->Add(m_counter,     0, wxALIGN_CENTER);
    sizer->Add(seconds_label, 0, wxALIGN_CENTER);
    sizer->AddSpacer(16);
    sizer->Add(cancel_btn,    0, wxALIGN_CENTER | wxBOTTOM, 12);
    SetSizerAndFit(sizer);

    Bind(wxEVT_TIMER, &BirdwingHandshakeDialog::on_timer, this);
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& e) { stop_auth(); e.Skip(); });

    start_auth();
}

BirdwingHandshakeDialog::~BirdwingHandshakeDialog()
{
    stop_auth();
}

void BirdwingHandshakeDialog::start_auth()
{
    m_seconds_left = 120;
    m_stop = false;
    m_timer.Start(1000);

    auto alive = m_alive;  // shared ownership
    m_thread = std::thread([this, alive] {
        std::string result;
        const auto status = m_link.birdwing_authorize(result, 120);
        if (!m_stop && *alive) {
            const bool ok = (status == MakerbotLink::BirdwingAuthResult::Success);
            wxTheApp->CallAfter([this, alive, ok, result] {
                if (*alive)            // dialog still alive?
                    on_auth_result(ok, result);
            });
        }
    });
}

void BirdwingHandshakeDialog::stop_auth()
{
    m_stop = true;
    *m_alive = false;     // invalidate before detach
    m_timer.Stop();
    if (m_thread.joinable())
        m_thread.detach(); // never block UI – SSL read times out on its own
    if (m_anim) m_anim->Stop();
}

void BirdwingHandshakeDialog::on_timer(wxTimerEvent&)
{
    --m_seconds_left;
    m_counter->SetLabel(wxString::Format("%d", m_seconds_left));

    if (m_seconds_left <= 0) {
        stop_auth();
        m_status->SetLabel(_L("Timeout – the printer did not respond.\n"
                              "Please try again and press the controller wheel within 120 seconds."));
        m_status->SetForegroundColour(wxColour(220, 60, 60));
        m_status->Refresh();
    }
}

void BirdwingHandshakeDialog::on_auth_result(bool success,
                                              const std::string& token_or_error)
{
    stop_auth();
    if (success) {
        m_token = token_or_error;
        m_status->SetLabel(_L("✅  Connection authorized!\n"
                              "Orca Slicer is now paired with your MakerBot."));
        m_status->SetForegroundColour(wxColour(0, 160, 80));
        m_status->Refresh();
        // Close after short delay so user can see success message
        wxMilliSleep(1500);
        EndModal(wxID_OK);
    } else {
        m_status->SetLabel(wxString::FromUTF8("❌  " + token_or_error));
        m_status->SetForegroundColour(wxColour(220, 60, 60));
        m_status->Wrap(440);
        m_status->Refresh();
    }
}

} // namespace GUI
} // namespace Slic3r
