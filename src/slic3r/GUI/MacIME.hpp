#pragma once

// Input methods (Chinese, Japanese, Korean) for ImGui text fields drawn on a GL canvas on macOS.
//
// wxWidgets gives a wxGLCanvas a view that is no NSTextInputClient, so the view has no input context:
// keys skip the input method and a CJK input source types plain letters. These functions turn the
// canvas view into a text input client while an ImGui text field wants text input. Committed text still
// arrives as wxEVT_CHAR.

namespace Slic3r::GUI {

// Call every frame with io.WantTextInput. While active, the view's input context stays the current one,
// which also lets the input source's own language toggle (Caps Lock, 中/英) reach the field. Deactivating
// drops an unfinished composition.
void mac_ime_set_active(void* ns_view, bool active);

// Anchors the candidate window at the text cursor. x, y (top of the cursor line) and height are canvas
// backing pixels, the coordinates ImGui works in.
void mac_ime_set_caret(void* ns_view, int x, int y, int height);

// True when the key-down being dispatched belongs to the input method: it changed or ended a composition,
// or arrived while one was open. wxWidgets still sends a key-down for such a key, which must not also
// edit the text field.
bool mac_ime_owns_key(void* ns_view);

} // namespace Slic3r::GUI
