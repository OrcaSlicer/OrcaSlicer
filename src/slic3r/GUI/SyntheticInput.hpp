#pragma once

#include <wx/defs.h>
#include <wx/mousestate.h>

namespace Slic3r { namespace GUI {

// Input state that automation (the MCP server) holds down while it synthesizes a gesture.
// Synthesized wx events carry their own modifier and button flags, but parts of the 3D view also
// ask the system for the keyboard and mouse state; those reads go through get_key_state() and
// get_mouse_state(), which add whatever automation is holding. Empty unless a synthesized gesture
// is in progress, so real input is unaffected. UI thread only.
namespace SyntheticInput {
void press_key(int key_code);
void release_key(int key_code);
bool is_key_down(int key_code);
// The pointer automation drives, in screen coordinates, with its buttons.
void set_mouse(const wxMouseState& state);
void release_mouse();
// Drops everything held, e.g. when a gesture fails half-way.
void clear();
} // namespace SyntheticInput

// wxGetKeyState() plus the keys automation holds down.
bool get_key_state(wxKeyCode key);
// wxGetMouseState(), or automation's pointer while it drives one.
wxMouseState get_mouse_state();

}} // namespace Slic3r::GUI
