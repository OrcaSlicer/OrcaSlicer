#pragma once

#include <wx/control.h>
#include <wx/gdicmn.h>
#include <wx/timer.h>

#include <chrono>

class wxDC;
class wxMouseEvent;

namespace Slic3r { namespace GUI {

// The REC pill the main window's tab bar shows before the slice button while a recording runs, whoever started
// it: an outlined pill whose dot pulses, filled red with a stop square under the pointer, and a click runs Stop
// Recording. It is in the window's content on every platform, where captures and full screen keep it (macOS
// leaves the title bar out of both), and paints its parent's background colour around the pill's corners.
class RecordingIndicator : public wxControl
{
public:
    explicit RecordingIndicator(wxWindow* parent);

    // Shown, the dot pulses until it is hidden again.
    bool Show(bool show = true) override;
    bool AcceptsFocus() const override { return false; }

protected:
    // The pill's size.
    wxSize DoGetBestSize() const override;

private:
    wxRect pill_rect() const;
    void   on_paint();
    void   draw_shapes(wxDC& dc, const wxRect& pill) const;
    void   on_mouse(wxMouseEvent& event);

    wxTimer                               m_pulse;
    std::chrono::steady_clock::time_point m_shown;
    bool                                  m_hover   = false;
    bool                                  m_pressed = false;
};

}} // namespace Slic3r::GUI
