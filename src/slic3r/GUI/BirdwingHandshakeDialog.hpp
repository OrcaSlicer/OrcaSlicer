
#ifndef slic3r_BirdwingHandshakeDialog_hpp_
#define slic3r_BirdwingHandshakeDialog_hpp_

// MakerBot / UltiMaker Fork – Orca Slicer 2.4
// Modal dialog shown while waiting for the user to press the MakerBot
// Birdwing printer's handwheel to authorize the connection.
//
// Shows:
//   • Animated GIF (resources/profiles/MakerBot/makerbot_birdwing_handshake.gif)
//   • 120-second countdown timer
//   • Status text (updated from background auth thread)
//
// Usage:
//   BirdwingHandshakeDialog dlg(parent, makerbotlink_instance);
//   if (dlg.ShowModal() == wxID_OK)
//       store_token(dlg.get_token());

#include <wx/dialog.h>
#include <wx/stattext.h>
#include <wx/timer.h>
#include <wx/animate.h>

#include <string>
#include <thread>
#include <atomic>

namespace Slic3r { class MakerbotLink; }

namespace Slic3r {
namespace GUI {

class BirdwingHandshakeDialog : public wxDialog
{
public:
    BirdwingHandshakeDialog(wxWindow* parent, const MakerbotLink& link);
    ~BirdwingHandshakeDialog() override;

    // Token received after successful pairing
    std::string get_token() const { return m_token; }

private:
    void start_auth();
    void stop_auth();
    void on_timer(wxTimerEvent&);
    void on_auth_result(bool success, const std::string& token_or_error);

    const MakerbotLink& m_link;
    wxAnimationCtrl*    m_anim    { nullptr };
    wxStaticText*       m_status  { nullptr };
    wxStaticText*       m_counter { nullptr };
    wxTimer             m_timer;
    int                 m_seconds_left { 120 };
    std::string         m_token;

    std::thread                          m_thread;
    std::atomic<bool>                    m_stop  { false };
    std::shared_ptr<std::atomic<bool>>   m_alive { std::make_shared<std::atomic<bool>>(true) };
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_BirdwingHandshakeDialog_hpp_
