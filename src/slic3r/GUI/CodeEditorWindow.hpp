#pragma once

#include <set>
#include <string>

class wxWebView;
class wxWindow;

namespace Slic3r { namespace GUI {

// A window whose page has a code editor (the Run Script panel, Action Replay). While it has the focus, the main frame's
// File › Save Project, Save Project as and Edit menu items act on its editor, and its other shortcut menu items whose key
// the editor takes are disabled, so the key reaches the editor (MainFrame::focused_code_editor()). UI thread.
class CodeEditorWindow
{
public:
    virtual ~CodeEditorWindow() = default;
    // Whether `focus` (wxWindow::FindFocus()), or the native focus inside the window's web view, is in this window.
    virtual bool owns_focus(const wxWindow* focus) const = 0;
    // Sends a menu's action to the page: "save", "save_as", "undo", "redo" or "select_all".
    virtual void forward_action(const std::string& action) = 0;
    // The page's web view, for the clipboard items; nullptr while there is none.
    virtual wxWebView* web_view() const = 0;
    // The chords the editor's own commands bind on this platform, in KeyChord::to_string()'s form, as the page reported
    // them.
    virtual const std::set<std::string>& editor_keys() const = 0;
};

}} // namespace Slic3r::GUI
