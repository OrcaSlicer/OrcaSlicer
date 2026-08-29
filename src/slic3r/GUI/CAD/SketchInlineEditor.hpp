#ifndef slic3r_SketchInlineEditor_hpp_
#define slic3r_SketchInlineEditor_hpp_

#include <functional>
#include <string>

#include <wx/window.h>

class wxFrame;
class wxTextCtrl;
class wxStaticText;
class wxPoint;

namespace Slic3r {
namespace GUI {

// Onshape-style in-canvas value editor: a small borderless floating frame holding a
// wxTextCtrl, shown at screen coordinates over the GL canvas. A top-level frame is
// used (not a child widget) because a native child cannot be composited over the
// double-buffered wxGLCanvas under GTK3/llvmpipe — it stays invisible. Enter (or blur)
// commits the parsed number, Esc cancels. This is the single numeric-entry path for
// sketch dimensions, replacing the docked/modal value cards.
class SketchInlineEditor
{
public:
    explicit SketchInlineEditor(wxWindow* parent_canvas);

    // Show the editor centred on `screen_px` (absolute screen coords), pre-filled with
    // `value`. on_commit(parsed) fires on Enter with a valid number; on_cancel() on Esc.
    void open(const wxPoint& screen_px, double value, const std::string& title,
              std::function<void(double)> on_commit,
              std::function<void()> on_cancel);
    void close();
    void cancel();                       // if open, run the registered cancel (keep-as-drawn)
    void commit();                       // if open, run the registered commit (accept the typed value)
    bool is_open() const { return m_open; }
    // True when the field itself holds keyboard focus. Callers use this to decide whether the
    // field will handle a key on its own or needs it forwarded — see DesignPanel's CHAR_HOOK.
    bool has_focus() const { return m_ctrl != nullptr && wxWindow::FindFocus() == m_ctrl; }

private:
    void do_commit();
    void do_cancel();
    // Say WHY a value was refused, in the title line above the field. Refusing input in
    // silence is indistinguishable from the app having frozen — the field just sits there
    // with the text re-selected and the user has no idea what it wants.
    void refit();                        // re-Fit around a changed title, then re-clamp on-screen
    void flag_invalid(const wxString& why);
    void clear_invalid();

    wxFrame*                    m_frame{nullptr};
    wxTextCtrl*                 m_ctrl{nullptr};
    wxStaticText*               m_title{nullptr};
    std::function<void(double)> m_commit;
    std::function<void()>       m_cancel;
    bool                        m_open{false};
    bool                        m_closing{false};
    wxString                    m_title_text;   // the real title, restored after an error message
};

}} // namespace Slic3r::GUI

#endif // slic3r_SketchInlineEditor_hpp_
