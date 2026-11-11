#pragma once

#include <wx/gdicmn.h>

#include <chrono>
#include <string>

class wxTopLevelWindow;

namespace Slic3r { namespace GUI { namespace Mcp {

// How the MCP tools read and change a top-level window's size and state where wx alone falls short, on
// Linux: on Wayland GTK never reports a minimized window, and wx counts the invisible margins of a
// client-side decorated window in its size; under X11 with no window manager, as on a bare Xvfb, nothing
// maximizes or minimizes a window unless the app does it itself. Elsewhere each call is wx's own. UI
// thread.

// Starts following the windows a Wayland compositor hides, which GTK passes on only as a visibility
// change, and the focus that ends a minimize. Once, before the first window_state().
void track_window_states();

// "minimized", "maximized" or "normal": a window's state as list_windows reports it and set_window takes
// it. On Wayland, which reports no minimize, a window reads minimized while the compositor hides it after
// set_window minimized it.
const char* window_state(const wxTopLevelWindow& window);

// The window's size as the window system shows it, in the units list_windows reports: wx's size, less
// the margins GTK draws a client-side decorated window's shadow in on Wayland, which the compositor and a
// window screenshot leave out.
wxSize window_size(wxTopLevelWindow& window);

// The wx size that gives a window `asked` in window_size()'s terms: `asked` plus the `extra` wx counts in
// the window's size beyond window_size(), held to its `min` and `max` wx sizes (wxDefaultCoord for none).
// `limited` says whether a limit held it.
wxSize frame_size_for(const wxSize& asked, const wxSize& extra, const wxSize& min, const wxSize& max, bool& limited);

// Gives the window `size` in window_size()'s terms, held to its minimum and maximum size; returns whether
// a limit held it.
bool resize_window(wxTopLevelWindow& window, const wxSize& size);
void maximize_window(wxTopLevelWindow& window);
void minimize_window(wxTopLevelWindow& window);
// Takes a window out of its minimized state, then out of its maximized one, touching only the state it is
// in. Returns whether the window system applies that later (GTK), so that a size or position given before
// it reads back would go to the window as it was.
bool restore_window(wxTopLevelWindow& window);
// Brings the window in front of the app's other windows, keeping a dialog that belongs to it above it.
void raise_window(wxTopLevelWindow& window);
// Whether bringing a minimized window back takes the compositor's leave to activate it, which it may
// refuse (Wayland).
bool restore_needs_activation();
// How long set_window waits for a window to report the `state` it was asked for: 3 s, and 5 s for a
// minimize on Wayland, where Mutter reports a window hidden once it has not shown it for 3 s.
std::chrono::seconds state_change_timeout(const std::string& state);

}}} // namespace Slic3r::GUI::Mcp
