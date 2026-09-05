#ifndef slic3r_SketchInlineEditor_hpp_
#define slic3r_SketchInlineEditor_hpp_

#include <functional>
#include <string>

#include <wx/gdicmn.h>

namespace Slic3r {
namespace GUI {

class ImGuiWrapper;

// Onshape-style in-canvas value editor.
//
// IT IS NOT A WINDOW. It used to be a borderless top-level wxFrame holding a wxTextCtrl, and
// that is the whole history of this file: a separate top-level window can only receive typing
// if the window manager grants it focus, and whether it does is not ours to decide. openbox
// grants it; mutter's focus-stealing prevention refuses it, so on a GNOME desktop the field
// appeared, showed its value selected, and silently ignored every keystroke — Enter then
// committed the number it opened with. Seven workarounds were tried against that (a real X11
// server timestamp for gtk_window_present, re-asserted SetFocus, dropping the _UTILITY hint,
// keeping the frame mapped between two queued fields, forwarding keys from the panel's
// CHAR_HOOK), one of them caused a macOS regression, and the test harness ended up clicking the
// field before typing — which is the workaround a user cannot be asked to perform, and is
// exactly the "label value not editable" report.
//
// So the field stops asking. It is now drawn INSIDE the GL canvas as an ImGui overlay, at the
// same screen point as before, and its keys arrive through the canvas's own key events, which
// GLCanvas3D already feeds to ImGui (see GLCanvas3D::on_key / on_char -> update_key_data). The
// canvas is part of the main window and already has focus, so there is no second window, no
// second focus, and no window manager in the path. The dimension labels next to it are already
// ImGui overlays (DesignSketchTool::draw_dim_label), so this is the same vocabulary, not a new
// one.
//
// Ownership: DesignCanvas owns it; DesignSketchTool::render() calls render() once per frame.
class SketchInlineEditor
{
public:
    SketchInlineEditor() = default;

    // Open the field anchored at `canvas_px` (canvas DEVICE pixels, the coordinate space the
    // sketch tool works in), pre-filled with `value` and pre-selected. on_commit(parsed) fires
    // on Enter with a valid number; on_cancel() on Esc.
    void open(const wxPoint& canvas_px, double value, const std::string& title,
              std::function<void(double)> on_commit,
              std::function<void()> on_cancel);
    void close();                        // drop it with neither callback
    void cancel();                       // if open, run the registered cancel (keep-as-drawn)
    void commit();                       // if open, run the registered commit (accept the typed value)
    bool is_open() const { return m_open; }

    // Draw it, and let ImGui do the editing. Called from DesignSketchTool::render() inside the
    // frame's ImGui pass; `scale` is the tool's m_render_scale. Returns true if it drew.
    bool render(ImGuiWrapper& imgui, float scale);

    // Kept because callers ask them, but there is no longer any difference to report: with no
    // window there is no state where the field is on screen but logically closed, and no state
    // where it is open but somebody else holds the keyboard.
    bool is_mapped() const { return m_open; }
    bool has_focus() const { return m_open; }
    void dismiss() { close(); }

private:
    void do_commit();
    void do_cancel();

    std::function<void(double)> m_commit;
    std::function<void()>       m_cancel;
    bool        m_open{false};
    bool        m_focus_pending{false};  // one frame of SetKeyboardFocusHere after opening
    wxPoint     m_anchor{0, 0};          // canvas device px
    std::string m_title;
    std::string m_err;                   // why the last value was refused, shown in the title line
    char        m_buf[64]{};             // the edited text; ImGui::InputText writes into it
};

}} // namespace Slic3r::GUI

#endif // slic3r_SketchInlineEditor_hpp_
