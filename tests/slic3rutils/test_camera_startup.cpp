#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/wxMediaCtrl3.h"
#include "test_utils.hpp"

#include <cstdlib>
#include <wx/app.h>
#include <wx/bitmap.h>
#include <wx/colour.h>
#include <wx/dcmemory.h>
#include <wx/defs.h>
#include <wx/frame.h>
#include <wx/gdicmn.h>
#include <wx/image.h>
#include <wx/init.h>
#include <wx/string.h>
#include <wx/windowptr.h>

namespace {
class CameraPaintView : public wxMediaCtrl3
{
public:
    using wxMediaCtrl3::wxMediaCtrl3;
    using wxMediaCtrl3::draw_frame;
};
}

TEST_CASE("A camera becomes ready only after a video frame and resets readiness for a new stream", "[CameraStartup][Regression]")
{
#if defined(__linux__)
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY"))
        SKIP("The camera widget test requires a display (for example, xvfb-run).");
#endif
    // wxInitializer otherwise creates a console-only app in this Catch2 executable.
    if (!wxAppConsole::GetInstance()) wxApp::SetInstance(new wxApp());
    wxInitializer init;
    REQUIRE(init.IsOk());
    wxInitAllImageHandlers();
    wxImage image(4, 4);
    image.SetRGB(wxRect(0, 0, 4, 4), 40, 80, 120);
    ScopedTemporaryFile idle_file(".bmp");
    const auto path = Slic3r::GUI::from_u8(idle_file.string());
    REQUIRE(image.SaveFile(path, wxBITMAP_TYPE_BMP));

    wxWindowPtr<wxFrame> host(new wxFrame(nullptr, wxID_ANY, "Camera startup test"));
    wxWindowPtr<wxMediaCtrl3> player(new wxMediaCtrl3(host.get()));
    player->SetIdleImage(path);
    CHECK_FALSE(player->HasVideoFrame()); // A valid splash image is not a live camera frame.
    player->BeginExternalStream();
    CHECK_FALSE(player->HasVideoFrame());
    player->SetExternalFrame(wxImage(), wxSize(4, 4));
    CHECK_FALSE(player->HasVideoFrame());
    player->SetExternalFrame(image, wxSize(4, 4));
    CHECK(player->HasVideoFrame());
    player->Stop();
    CHECK_FALSE(player->HasVideoFrame());
    player->BeginExternalStream();
    CHECK_FALSE(player->HasVideoFrame());
    player->SetExternalFrame(image, wxSize(4, 4));
    CHECK(player->HasVideoFrame());
    player->EndExternalStream();
    CHECK_FALSE(player->HasVideoFrame());
    player.reset();
    host.reset();
    wxTheApp->ProcessPendingEvents();
}

TEST_CASE("Camera repaint retains the last valid frame and fills its letterbox", "[CameraPaint][Regression]")
{
#if defined(__linux__)
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY"))
        SKIP("The camera widget test requires a display (for example, xvfb-run).");
#endif
    if (!wxAppConsole::GetInstance()) wxApp::SetInstance(new wxApp());
    wxInitializer init;
    REQUIRE(init.IsOk());
    wxInitAllImageHandlers();
    wxImage image(4, 4);
    image.SetRGB(wxRect(0, 0, 4, 4), 40, 80, 120);
    ScopedTemporaryFile idle_file(".bmp");
    const auto path = Slic3r::GUI::from_u8(idle_file.string());
    REQUIRE(image.SaveFile(path, wxBITMAP_TYPE_BMP));

    wxWindowPtr<wxFrame> host(new wxFrame(nullptr, wxID_ANY, "Camera paint test"));
    wxWindowPtr<CameraPaintView> player(new CameraPaintView(host.get()));
    player->SetIdleImage(path);
    player->BeginExternalStream();
    player->SetExternalFrame(image, image.GetSize());
    const auto background = player->GetBackgroundColour();
    for (int repaint = 0; repaint < 3; ++repaint) {
        // An incomplete incoming frame must not clear the displayed picture.
        player->SetExternalFrame(wxImage(), image.GetSize());
        wxImage dirty_buffer(12, 8);
        dirty_buffer.SetRGB(wxRect(0, 0, 12, 8), 255, 0, 255);
        wxBitmap buffer(dirty_buffer);
        {
            wxMemoryDC dc(buffer);
            player->draw_frame(dc, wxSize(12, 8));
        }
        const auto painted = buffer.ConvertToImage();
        REQUIRE(painted.IsOk());
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 12; ++x) {
                const bool picture = x >= 2 && x < 10;
                CHECK(painted.GetRed(x, y) == (picture ? 40 : background.Red()));
                CHECK(painted.GetGreen(x, y) == (picture ? 80 : background.Green()));
                CHECK(painted.GetBlue(x, y) == (picture ? 120 : background.Blue()));
            }
        }
    }
    CHECK(player->HasVideoFrame());
    player.reset();
    host.reset();
    wxTheApp->ProcessPendingEvents();
}
