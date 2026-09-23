#pragma once

#include <wx/accel.h>
#include <wx/defs.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

class wxKeyEvent;

namespace Slic3r { namespace GUI {

// One key press: a key code as wxEVT_KEY_DOWN reports it (letters upper-case, numpad keys
// folded onto their main-keyboard equivalents) plus the wxMOD_* modifiers held with it.
// Printable punctuation is stored as the character it produces, so "+" means the key that
// types "+" on the user's layout.
struct KeyChord
{
    int key       = WXK_NONE;
    int modifiers = wxMOD_NONE;

    bool valid() const { return key != WXK_NONE; }
    bool operator==(const KeyChord& other) const { return key == other.key && modifiers == other.modifiers; }
    bool operator!=(const KeyChord& other) const { return !(*this == other); }

    // Bare printable keys other than letters and digits are matched on wxEVT_CHAR, because
    // only the char event knows which character a key produces under the active layout.
    bool is_punctuation() const;
    // True for a printable non-alphanumeric key pressed with nothing but Shift, which only the
    // char event that follows can resolve.
    bool needs_char_event() const;
    // True when Ctrl or Alt is held or the key is non-printable, the chords a menu can own without
    // swallowing typing in text fields.
    bool is_menu_accelerator() const;
    // True for a chord the desktop acts on although the app receives it, so a binding would
    // take it from the system.
    bool is_system_shortcut() const;

    // Platform-neutral text ("Ctrl+Shift+S") for persistence and wx accelerator strings.
    std::string to_string() const;
    static std::optional<KeyChord> parse(const std::string& text);

    // Text for menus, tooltips and the shortcuts dialog, with translated modifier names and the
    // command and option glyphs on macOS.
    std::string display() const;
    // The pieces display() joins with "+": the modifier names, then the key name.
    std::vector<std::string> display_parts() const;
    // Translated text of one wxMOD_* modifier, as a "Ctrl+" prefix or the bare "Ctrl" name.
    static std::string modifier_prefix(int modifier);
    static std::string modifier_name(int modifier);

    wxAcceleratorEntry to_accelerator_entry(int command) const;

    // Builds the chord a key event describes, or an invalid chord for pure modifier presses and
    // keys outside the bindable set. wxEVT_CHAR events are normalized to the key codes
    // wxEVT_KEY_DOWN reports for letters, digits and special keys.
    static KeyChord from_event(const wxKeyEvent& evt);
};

struct KeyChordHash
{
    size_t operator()(const KeyChord& chord) const { return std::hash<long long>()((static_cast<long long>(chord.modifiers) << 32) | unsigned(chord.key)); }
};

}} // namespace Slic3r::GUI
