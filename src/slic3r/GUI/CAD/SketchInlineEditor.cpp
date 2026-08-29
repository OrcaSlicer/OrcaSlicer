#include "slic3r/GUI/CAD/SketchInlineEditor.hpp"
#include "slic3r/GUI/I18N.hpp"   // _L for the refusal messages shown in the title line

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
#include <cstdlib>

#ifdef __WXGTK__
#include <gtk/gtk.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif
#endif

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

// Present the toplevel with a real X11 server timestamp: wxFrame::Raise() maps to
// gtk_window_present() with gtk_get_current_event_time(), which inside a CallAfter is
// GDK_CURRENT_TIME (0) and is ignored by mutter's focus-stealing prevention. A server
// timestamp lets the compositor grant focus to the re-mapped window.
#ifdef __WXGTK__
void present_toplevel(wxFrame* frame)
{
#ifdef GDK_WINDOWING_X11
    if (frame) {
        GtkWidget* widget = static_cast<GtkWidget*>(frame->GetHandle());
        if (widget && GTK_IS_WIDGET(widget)) {
            GdkWindow* gdkwin = gtk_widget_get_window(widget);
            if (gdkwin) {
                gtk_window_present_with_time(GTK_WINDOW(widget),
                                             gdk_x11_get_server_time(gdkwin));
                return;
            }
        }
    }
#endif
    if (frame) frame->Raise();
}
#else
void present_toplevel(wxFrame* frame)
{
    if (frame) frame->Raise();
}
#endif

// Between two queued dimensions (a rectangle's Width then Height) the frame is either kept
// MAPPED and merely re-titled, or unmapped and mapped again. That is a per-toolkit choice, not
// a preference:
//   GTK/mutter  keep it mapped. Focus-stealing prevention refuses keyboard focus to a window
//               that was just re-mapped, so hiding between the two fields left the second one
//               visible but dead (snaporca-p8uw).
//   elsewhere   map it afresh. This is what shipped before that workaround, which was applied
//               with no platform guard — and it is the only difference between the first queued
//               field (works everywhere) and the second (macOS wedges the whole app, PR #15238).
//               A workaround for one window manager must not become a contract for all of them.
constexpr bool keep_mapped_between_fields =
#ifdef __WXGTK__
    true;
#else
    false;
#endif

void trace_inline_focus(wxFrame* frame, const std::string& title)
{
    if (!std::getenv("SNAPORCA_KEYTRACE")) return;
#ifdef __WXGTK__
    GtkWindow* win = nullptr;
    if (frame) {
        GtkWidget* widget = static_cast<GtkWidget*>(frame->GetHandle());
        if (widget && GTK_IS_WIDGET(widget)) win = GTK_WINDOW(widget);
    }
    fprintf(stderr, "[INLINE_FOCUS] title=%s active=%d toplevel_focus=%d shown=%d\n",
            title.c_str(),
            win ? (int) gtk_window_is_active(win) : -1,
            win ? (int) gtk_window_has_toplevel_focus(win) : -1,
            frame ? (int) frame->IsShown() : -1);
#else
    fprintf(stderr, "[INLINE_FOCUS] title=%s shown=%d\n",
            title.c_str(), frame ? (int) frame->IsShown() : -1);
#endif
    fflush(stderr);
}
} // namespace

// The title line doubles as the error line, so both colours live here rather than as a
// literal at the one place that used to set it.
// Keep the frame fully on-screen: an anchor that maps off the display makes GTK drop the
// window at a default corner (top-left) instead of the requested point. Clamp to the display
// the anchor is ON, not the primary one — wxGetClientDisplayRect() only ever describes the
// primary monitor, so on a multi-head desktop this shoved the field onto a different screen
// than the app. It then sat invisible while m_awaiting_length made the sketch tool eat every
// mouse event, which read as the viewport freezing after a sketch, with only Enter able to
// release it. Shared with the error re-fit below, which can widen the frame after placement.
static wxPoint clamp_to_display(wxPoint pos, const wxSize& sz, const wxPoint& anchor, wxWindow* w)
{
    int disp = wxDisplay::GetFromPoint(anchor);
    if (disp == wxNOT_FOUND) disp = wxDisplay::GetFromWindow(w);
    const wxRect area = (disp != wxNOT_FOUND) ? wxDisplay(unsigned(disp)).GetClientArea()
                                              : wxGetClientDisplayRect();
    pos.x = std::max(area.GetLeft(), std::min(pos.x, area.GetRight()  - sz.GetWidth()));
    pos.y = std::max(area.GetTop(),  std::min(pos.y, area.GetBottom() - sz.GetHeight()));
    return pos;
}

static const wxColour kTitleFg (160, 162, 168);
static const wxColour kTitleErr(232, 106, 106);

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
    m_title->SetForegroundColour(kTitleFg);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_title, 0, wxLEFT | wxRIGHT | wxTOP, 3);
    sizer->Add(m_ctrl, 1, wxEXPAND | wxALL, 2);
    m_frame->SetSizerAndFit(sizer);
    m_frame->Hide();

    m_ctrl->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { do_commit(); });
    // The complaint goes away the moment the user starts answering it — an error that
    // outlives the input it was about is just noise on the next attempt.
    m_ctrl->Bind(wxEVT_TEXT, [this](wxCommandEvent& e) { clear_invalid(); e.Skip(); });
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
    // Where the frame is kept mapped, never close/unmap on the way in: the previous queued
    // dimension left it mapped (see do_commit) and re-mapping is what mutter refuses to focus,
    // so reuse it and just re-title/re-position. Elsewhere, force a fresh map.
    if (!keep_mapped_between_fields && m_frame->IsShown())
        m_frame->Hide();
    m_commit = std::move(on_commit);
    m_cancel = std::move(on_cancel);
    m_ctrl->ChangeValue(en_format(value));
    if (m_title) {
        m_title_text = wxString::FromUTF8(title.c_str());
        m_title->SetLabel(m_title_text);
        m_title->SetForegroundColour(kTitleFg);   // drop any refusal left over from the last field
        m_title->Show(!title.empty());
    }
    m_frame->Fit();
    const wxSize sz = m_frame->GetSize();
    wxPoint pos = clamp_to_display(wxPoint(screen_px.x - sz.GetWidth() / 2,
                                           screen_px.y - sz.GetHeight() / 2),
                                   sz, screen_px, m_frame);
    // Show() BEFORE Move(): GTK ignores a Move() issued before the window is mapped (the
    // WM places it at its default, i.e. the top-left corner). Move after Show sticks.
    if (!m_frame->IsShown())
        m_frame->Show();
    m_frame->Move(pos);
    present_toplevel(m_frame);    // activate the top-level so SetFocus routes
    m_frame->SetFocus();
    m_ctrl->SetFocus();
    m_ctrl->SelectAll();
    m_open = true;
    trace_inline_focus(m_frame, title);
    // Re-assert on the next tick too: the GL canvas can reclaim focus while it finishes
    // handling the click/render that opened us, so a single immediate SetFocus may be stolen.
    m_ctrl->CallAfter([this, title] {
        if (m_open && m_ctrl) {
            present_toplevel(m_frame);
            m_ctrl->SetFocus();
            m_ctrl->SelectAll();
            trace_inline_focus(m_frame, title);
        }
    });
}

void SketchInlineEditor::do_commit()
{
    if (!m_open || m_ctrl == nullptr) return;
    double v = 0.0;
    if (!en_parse(m_ctrl->GetValue(), v)) {   // invalid: keep editing, and SAY SO
        // Silence here read as a freeze: Enter did nothing, the text re-selected itself, and
        // nothing on screen said the value had been refused or what would be accepted. Every
        // other CAD names the problem in place; so do we.
        flag_invalid(m_ctrl->GetValue().Strip(wxString::both).IsEmpty()
                         ? _L("Enter a number")
                         : _L("Not a number"));
        m_ctrl->SetFocus();
        m_ctrl->SelectAll();
        return;
    }
    auto cb = m_commit;
    m_open   = false;          // logically closed; whether it stays MAPPED is per-toolkit
    m_commit = nullptr;
    m_cancel = nullptr;
    // Unmap BEFORE the callback where we are not keeping it mapped, so the reopen the callback
    // schedules starts from a hidden frame — the ordering that shipped before the workaround.
    if (!keep_mapped_between_fields)
        m_frame->Hide();
    if (cb) cb(v);
    // Kept mapped: the callback either re-opens us for the next queued dimension (via its own
    // CallAfter, queued during cb(v), therefore BEFORE the one below) or it does not. Hiding
    // here would unmap the window and mutter would refuse to focus the re-map; so hide only
    // after the reopen has had its turn. Harmless on the unmapped path — already hidden.
    m_frame->CallAfter([this] { if (!m_open && m_frame) m_frame->Hide(); });
}

void SketchInlineEditor::cancel()
{
    if (m_open) do_cancel();
}

// Accept what is typed and close. Leaving a tool must not silently discard the value the user
// just entered — the same rule set_tool already follows for a ready edit-op.
void SketchInlineEditor::commit()
{
    if (!m_open) return;
    do_commit();
    // do_commit REFUSES to close on unparseable text, which is right while the user is still
    // typing — but this entry point is "we are leaving", and the caller (set_tool) unfreezes
    // the canvas immediately afterwards. Refusing here left the field alive and focused over a
    // viewport that was interactive again, editing geometry nothing was pointing at any more.
    // We cannot accept the text and we must not keep it: fall back to keep-as-drawn, the same
    // thing Esc means.
    if (m_open) do_cancel();
}

// Re-fit around a changed title, keeping the field itself where it is. The frame is anchored
// top-left, so growing it can push the right edge off the display — re-clamp after the Fit.
void SketchInlineEditor::refit()
{
    if (m_frame == nullptr) return;
    const wxPoint at = m_frame->GetPosition();
    m_frame->Fit();
    m_frame->Move(clamp_to_display(at, m_frame->GetSize(), at, m_frame));
}

void SketchInlineEditor::flag_invalid(const wxString& why)
{
    if (m_title == nullptr) return;
    m_title->SetLabel(why);
    m_title->SetForegroundColour(kTitleErr);
    m_title->Show(true);
    refit();      // "Not a number" is wider than "Length" — without this it renders as "Not a"
    m_title->Refresh();
}

void SketchInlineEditor::clear_invalid()
{
    if (m_title == nullptr || m_title->GetForegroundColour() != kTitleErr) return;
    m_title->SetLabel(m_title_text);
    m_title->SetForegroundColour(kTitleFg);
    m_title->Show(!m_title_text.IsEmpty());
    refit();
    m_title->Refresh();
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
    // m_closing was written and never read — a flag that looked like re-entrancy protection
    // and was not. Hide() below pumps native events, so a nested close is reachable in
    // principle; read the flag and the guard becomes real.
    if (m_frame == nullptr || !m_open || m_closing) return;
    m_closing = true;
    m_open    = false;
    m_frame->Hide();
    m_commit = nullptr;
    m_cancel = nullptr;
    m_closing = false;
}

}} // namespace Slic3r::GUI
