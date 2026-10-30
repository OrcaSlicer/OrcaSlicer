#include "RecordingIndicator.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "NativeCommands.hpp"
#include "Widgets/Label.hpp"

#include <wx/bitmap.h>
#include <wx/brush.h>
#include <wx/colour.h>
#include <wx/cursor.h>
#include <wx/dc.h>
#include <wx/dcclient.h>
#include <wx/dcgraph.h>
#include <wx/dcmemory.h>
#include <wx/defs.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/pen.h>
#include <wx/timer.h>
#include <wx/window.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace Slic3r { namespace GUI {

namespace {

// In DIPs.
constexpr int kHeight = 18, kPadLeft = 7, kDot = 7, kGap = 5, kPadRight = 8;
const wxColour kRed(240, 68, 56);
const wxColour kLabel(255, 180, 174);

// The dot's opacity `seconds` after the pill showed: a fade out and back in every 1.2 s.
double pulse(double seconds)
{
    constexpr double kPi = 3.14159265358979323846;
    return 0.25 + 0.75 * (0.5 + 0.5 * std::cos(2. * kPi * seconds / 1.2));
}

// Runs Stop Recording once the mouse handler that calls this has returned: it hides the pill.
void stop_recording() { wxGetApp().CallAfter([] { NativeCommands::notify_result(NativeCommands::run("stop_recording")); }); }

} // namespace

RecordingIndicator::RecordingIndicator(wxWindow* parent)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Create(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    SetFont(Label::Head_10);
    // TRN Short label of the indicator the main window shows while a recording runs, as on a camera.
    SetLabel(_L("REC"));
    SetToolTip(_L("Recording. Click to stop."));

    m_pulse.Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        if (!m_hover)
            Refresh();
    });
    Bind(wxEVT_PAINT, [this](wxPaintEvent&) { on_paint(); });
    for (const wxEventTypeTag<wxMouseEvent>& type : {wxEVT_ENTER_WINDOW, wxEVT_LEAVE_WINDOW, wxEVT_MOTION, wxEVT_LEFT_DOWN, wxEVT_LEFT_UP})
        Bind(type, &RecordingIndicator::on_mouse, this);
}

bool RecordingIndicator::Show(bool show)
{
    if (!wxControl::Show(show))
        return false;
    m_hover = m_pressed = false;
    if (show) {
        m_shown = std::chrono::steady_clock::now();
        m_pulse.Start(50);
    } else {
        m_pulse.Stop();
    }
    return true;
}

wxSize RecordingIndicator::DoGetBestSize() const
{
    return {pill_rect().width, FromDIP(kHeight)};
}

wxRect RecordingIndicator::pill_rect() const
{
    const int text_w = GetTextExtent(GetLabel()).x;
    const int height = FromDIP(kHeight);
    return {0, (GetClientSize().y - height) / 2, FromDIP(kPadLeft + kDot + kGap + kPadRight) + text_w, height};
}

void RecordingIndicator::on_paint()
{
    wxPaintDC      dc(this);
    const wxSize   size = GetClientSize();
    const wxRect   pill = pill_rect();
    const wxColour bar  = GetParent()->GetBackgroundColour();
#ifdef __WXMSW__
    // GDI draws neither alpha nor smooth curves: the shapes go through GDI+ on a bitmap the window's size
    // (physical pixels on MSW), the text straight onto the window after, at the window's DPI.
    wxMemoryDC memdc(&dc);
    wxBitmap   bmp(std::max(1, size.x), std::max(1, size.y));
    memdc.SelectObject(bmp);
    memdc.SetBackground(wxBrush(bar));
    memdc.Clear();
    {
        wxGCDC gcdc(memdc);
        draw_shapes(gcdc, pill);
    }
    memdc.SelectObject(wxNullBitmap);
    dc.DrawBitmap(bmp, 0, 0);
#else
    dc.SetBackground(wxBrush(bar));
    dc.Clear();
    draw_shapes(dc, pill);
#endif
    dc.SetFont(GetFont());
    dc.SetTextForeground(m_hover ? *wxWHITE : kLabel);
    const wxSize text = dc.GetTextExtent(GetLabel());
    dc.DrawText(GetLabel(), pill.x + FromDIP(kPadLeft + kDot + kGap), pill.y + (pill.height - text.y) / 2);
}

void RecordingIndicator::draw_shapes(wxDC& dc, const wxRect& pill) const
{
    const double radius = pill.height / 2.;
    const wxRect mark(pill.x + FromDIP(kPadLeft), pill.y + (pill.height - FromDIP(kDot)) / 2, FromDIP(kDot), FromDIP(kDot));
    if (m_hover) {
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(kRed));
        dc.DrawRoundedRectangle(pill, radius);
        dc.SetBrush(*wxWHITE_BRUSH);
        dc.DrawRoundedRectangle(mark, FromDIP(3) / 2.);
        return;
    }
    const int line = std::max(1, FromDIP(1));
    dc.SetPen(wxPen(wxColour(kRed.Red(), kRed.Green(), kRed.Blue(), 140), line));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawRoundedRectangle(wxRect(pill).Deflate(line / 2), radius - line / 2);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_shown).count();
    const auto   alpha   = static_cast<unsigned char>(std::lround(255. * pulse(seconds)));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(wxColour(kRed.Red(), kRed.Green(), kRed.Blue(), alpha)));
    dc.DrawEllipse(mark);
}

void RecordingIndicator::on_mouse(wxMouseEvent& event)
{
    const bool over = event.GetEventType() != wxEVT_LEAVE_WINDOW && GetClientRect().Contains(event.GetPosition());
    if (over != m_hover) {
        m_hover = over;
        SetCursor(over ? wxCursor(wxCURSOR_HAND) : wxNullCursor);
        Refresh();
    }
    if (event.LeftDown())
        m_pressed = over;
    else if (event.LeftUp()) {
        if (m_pressed && over)
            stop_recording();
        m_pressed = false;
    } else if (!over)
        m_pressed = false;
    event.Skip();
}

}} // namespace Slic3r::GUI
