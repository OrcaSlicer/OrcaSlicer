#include "SketchInlineEditor.hpp"

#include <wx/display.h>

#include <wx/frame.h>
#include <wx/textctrl.h>
#include <wx/stattext.h>
#include <wx/sizer.h>
#include <wx/window.h>
#include <wx/toplevel.h>
#include <wx/gdicmn.h>

#include <algorithm>
#include <cstdio>

namespace Slic3r {
namespace GUI {

namespace {
// Locale-safe value <-> text (wx sets LC_NUMERIC to the user locale, so snprintf may
// emit a comma; parsing accepts either separator). Mirrors DesignPanel's en_*.
wxString en_format(double v, int digits = 2)
{
    char fmt[16];
    std::snprintf(fmt, sizeof(fmt), "%%.%df", digits);
    char buf[64];
    std::snprintf(buf, sizeof(buf), fmt, v);
    for (char* c = buf; *c; ++c) if (*c == ',') *c = '.';
    return wxString::FromUTF8(buf);
}
bool en_parse(const wxString& text, double& out)
{
    wxString t(text);
    t.Replace(wxT(","), wxT("."));
    return t.ToCDouble(&out);
}
} // namespace

SketchInlineEditor::SketchInlineEditor(wxWindow* parent_canvas)
{
    wxWindow* top = parent_canvas ? wxGetTopLevelParent(parent_canvas) : nullptr;
    // Borderless floating frame: a top-level window so the WM composites it above the
    // GL canvas (a child widget would be hidden by the GL surface). Floats on its
    // parent and stays on top so it tracks the main window.
    // NB: no wxFRAME_FLOAT_ON_PARENT — that maps to a GTK _UTILITY_ window-type hint, which
    // many WMs (incl. the xrdp/x11vnc session on :10) refuse to give keyboard focus, so the
    // field opened un-focusable and needed a click before typing. Plain stay-on-top frame is
    // WM-focusable; we present + SetFocus it explicitly in open().
    m_frame = new wxFrame(top, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                          wxFRAME_NO_TASKBAR | wxBORDER_NONE | wxSTAY_ON_TOP);
    m_ctrl = new wxTextCtrl(m_frame, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(82, -1),
                            wxTE_PROCESS_ENTER | wxTE_RIGHT | wxBORDER_SIMPLE);
    m_frame->SetBackgroundColour(wxColour(40, 42, 46));
    m_title = new wxStaticText(m_frame, wxID_ANY, wxEmptyString);
    m_title->SetForegroundColour(wxColour(160, 162, 168));
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_title, 0, wxLEFT | wxRIGHT | wxTOP, 3);
    sizer->Add(m_ctrl, 1, wxEXPAND | wxALL, 2);
    m_frame->SetSizerAndFit(sizer);
    m_frame->Hide();

    m_ctrl->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { do_commit(); });
    m_ctrl->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
        if (e.GetKeyCode() == WXK_ESCAPE) do_cancel();
        // Tab commits, exactly like Enter — the caller's on_commit is what walks to the next
        // dimension. Left to wx's default handling it navigated within this one-control popup,
        // i.e. back to the same field with the text re-selected: typing 60, Tab, 40 looked like
        // two dimensions entered and silently kept only the 40. Losing typed input with no
        // visible difference from a committed field is the part that made this worth a key case.
        else if (e.GetKeyCode() == WXK_TAB) do_commit();
        else                             e.Skip();
    });
}

void SketchInlineEditor::open(const wxPoint& screen_px, double value,
                              const std::string& title,
                              std::function<void(double)> on_commit,
                              std::function<void()> on_cancel)
{
    if (m_frame == nullptr || m_ctrl == nullptr) { if (on_cancel) on_cancel(); return; }
    if (m_open) close();
    m_commit = std::move(on_commit);
    m_cancel = std::move(on_cancel);
    m_ctrl->ChangeValue(en_format(value));
    if (m_title) {
        m_title->SetLabel(wxString::FromUTF8(title.c_str()));
        m_title->Show(!title.empty());
    }
    m_frame->Fit();
    const wxSize sz = m_frame->GetSize();
    wxPoint pos(screen_px.x - sz.GetWidth() / 2, screen_px.y - sz.GetHeight() / 2);
    // Keep the frame fully on-screen: an anchor that maps off the display makes GTK drop
    // the window at a default corner (top-left) instead of the requested point.
    // Clamp to the display the anchor is ON, not the primary one: wxGetClientDisplayRect()
    // only ever describes the primary monitor, so on a multi-head desktop it shoved this
    // field onto a different screen than the app. It then sat invisible while
    // m_awaiting_length made the sketch tool eat every mouse event, which read as the
    // viewport freezing after a sketch with only Enter able to release it.
    int disp = wxDisplay::GetFromPoint(screen_px);
    if (disp == wxNOT_FOUND) disp = wxDisplay::GetFromWindow(m_frame);
    const wxRect area = (disp != wxNOT_FOUND) ? wxDisplay(unsigned(disp)).GetClientArea()
                                              : wxGetClientDisplayRect();
    pos.x = std::max(area.GetLeft(), std::min(pos.x, area.GetRight()  - sz.GetWidth()));
    pos.y = std::max(area.GetTop(),  std::min(pos.y, area.GetBottom() - sz.GetHeight()));
    // Show() BEFORE Move(): GTK ignores a Move() issued before the window is mapped (the
    // WM places it at its default, i.e. the top-left corner). Move after Show sticks.
    m_frame->Show();
    m_frame->Move(pos);
    m_frame->Raise();             // gtk_window_present -> activate the top-level so SetFocus routes
    m_frame->SetFocus();
    m_ctrl->SetFocus();
    m_ctrl->SelectAll();
    m_open = true;
    // Re-assert on the next tick too: the GL canvas can reclaim focus while it finishes
    // handling the click/render that opened us, so a single immediate SetFocus may be stolen.
    m_ctrl->CallAfter([this] {
        if (m_open && m_ctrl) { m_frame->Raise(); m_ctrl->SetFocus(); m_ctrl->SelectAll(); }
    });
}

void SketchInlineEditor::do_commit()
{
    if (!m_open || m_ctrl == nullptr) return;
    double v = 0.0;
    if (!en_parse(m_ctrl->GetValue(), v)) {   // invalid: keep editing
        m_ctrl->SetFocus();
        m_ctrl->SelectAll();
        return;
    }
    auto cb = m_commit;   // copy-then-close: the callback re-enters (re-solve + render)
    close();
    if (cb) cb(v);
}

void SketchInlineEditor::cancel()
{
    if (m_open) do_cancel();
}

void SketchInlineEditor::do_cancel()
{
    if (!m_open) return;
    auto cb = m_cancel;
    close();
    if (cb) cb();
}

void SketchInlineEditor::close()
{
    if (m_frame == nullptr || !m_open) return;
    m_closing = true;
    m_open    = false;
    m_frame->Hide();
    m_commit = nullptr;
    m_cancel = nullptr;
    m_closing = false;
}

}} // namespace Slic3r::GUI
