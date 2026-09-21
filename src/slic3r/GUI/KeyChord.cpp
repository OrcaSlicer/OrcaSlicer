#include "KeyChord.hpp"

#include "GUI.hpp"
#include "I18N.hpp"

#include <wx/event.h>

#include <algorithm>
#include <array>
#include <cctype>

namespace Slic3r { namespace GUI {

namespace {

constexpr int BINDABLE_MODIFIERS = wxMOD_CONTROL | wxMOD_SHIFT | wxMOD_ALT | wxMOD_RAW_CONTROL;

struct KeyName
{
    int         key;
    const char* name;       // canonical name, as wx parses it
    const char* alias;      // accepted when parsing; nullptr when there is none
    const char* label;      // translation key for KeyChord::display(); nullptr when name is it
};

constexpr std::array<KeyName, 15> special_keys{{
    { WXK_BACK,     L_CONTEXT("Backspace", "Keyboard Shortcut"), "Back",     nullptr },
    { WXK_TAB,      L_CONTEXT("Tab", "Keyboard Shortcut"),       nullptr,    nullptr },
    { WXK_RETURN,   L_CONTEXT("Enter", "Keyboard Shortcut"),     "Return",   nullptr },
    { WXK_ESCAPE,   L_CONTEXT("Esc", "Keyboard Shortcut"),       "Escape",   nullptr },
    { WXK_SPACE,    L_CONTEXT("Space", "Keyboard Shortcut"),     nullptr,    nullptr },
    { WXK_DELETE,   L_CONTEXT("Del", "Keyboard Shortcut"),       "Delete",   nullptr },
    { WXK_INSERT,   L_CONTEXT("Ins", "Keyboard Shortcut"),       "Insert",   nullptr },
    { WXK_HOME,     L_CONTEXT("Home", "Keyboard Shortcut"),      nullptr,    nullptr },
    { WXK_END,      L_CONTEXT("End", "Keyboard Shortcut"),       nullptr,    nullptr },
    { WXK_PAGEUP,   L_CONTEXT("PgUp", "Keyboard Shortcut"),      "PageUp",   nullptr },
    { WXK_PAGEDOWN, L_CONTEXT("PgDn", "Keyboard Shortcut"),      "PageDown", nullptr },
    // Displayed as "Arrow Left" and so on, which is what the catalogs translate.
    { WXK_LEFT,     "Left",  nullptr, L_CONTEXT("Arrow Left", "Keyboard Shortcut") },
    { WXK_RIGHT,    "Right", nullptr, L_CONTEXT("Arrow Right", "Keyboard Shortcut") },
    { WXK_UP,       "Up",    nullptr, L_CONTEXT("Arrow Up", "Keyboard Shortcut") },
    { WXK_DOWN,     "Down",  nullptr, L_CONTEXT("Arrow Down", "Keyboard Shortcut") },
}};

bool equals_ignoring_case(const std::string& a, const char* b)
{
    if (b == nullptr)
        return false;
    size_t i = 0;
    for (; i < a.size() && b[i] != '\0'; ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return i == a.size() && b[i] == '\0';
}

bool is_letter(int key) { return key >= 'A' && key <= 'Z'; }
bool is_digit(int key) { return key >= '0' && key <= '9'; }
bool is_printable(int key) { return key > ' ' && key < 127; }
bool is_symbol(int key) { return is_printable(key) && !std::isalnum(key); }
bool is_function_key(int key) { return key >= WXK_F1 && key <= WXK_F24; }

bool is_special(int key)
{
    if (is_function_key(key))
        return true;
    return std::any_of(special_keys.begin(), special_keys.end(), [key](const KeyName& k) { return k.key == key; });
}

std::string special_key_name(int key)
{
    if (is_function_key(key))
        return "F" + std::to_string(key - WXK_F1 + 1);
    for (const KeyName& k : special_keys)
        if (k.key == key)
            return k.name;
    return {};
}

std::string special_key_label(int key)
{
    for (const KeyName& k : special_keys)
        if (k.key == key)
            return _u8L_CONTEXT(k.label != nullptr ? k.label : k.name, "Keyboard Shortcut");
    return special_key_name(key);
}

int parse_key(const std::string& text)
{
    if (text.size() == 1) {
        const int key = static_cast<unsigned char>(text[0]);
        return is_printable(key) ? std::toupper(key) : WXK_NONE;
    }
    for (const KeyName& k : special_keys)
        if (equals_ignoring_case(text, k.name) || equals_ignoring_case(text, k.alias))
            return k.key;
    if ((text[0] == 'F' || text[0] == 'f') && text.size() <= 3 && std::all_of(text.begin() + 1, text.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
        const int n = std::stoi(text.substr(1));
        if (n >= 1 && n <= 24)
            return WXK_F1 + n - 1;
    }
    return WXK_NONE;
}

int parse_modifier(const std::string& text)
{
    if (equals_ignoring_case(text, "Ctrl") || equals_ignoring_case(text, "Control") || equals_ignoring_case(text, "Cmd") || equals_ignoring_case(text, "Command"))
        return wxMOD_CONTROL;
    if (equals_ignoring_case(text, "Shift"))
        return wxMOD_SHIFT;
    if (equals_ignoring_case(text, "Alt") || equals_ignoring_case(text, "Option"))
        return wxMOD_ALT;
    if (equals_ignoring_case(text, "RawCtrl"))
        return wxMOD_RAW_CONTROL;
    return wxMOD_NONE;
}

// Numpad keys act as their main-keyboard counterparts, so one binding covers both.
int fold_numpad(int key)
{
    if (key >= WXK_NUMPAD0 && key <= WXK_NUMPAD9)
        return '0' + (key - WXK_NUMPAD0);
    switch (key) {
    case WXK_NUMPAD_ENTER:    return WXK_RETURN;
    case WXK_NUMPAD_SPACE:    return WXK_SPACE;
    case WXK_NUMPAD_TAB:      return WXK_TAB;
    case WXK_NUMPAD_HOME:     return WXK_HOME;
    case WXK_NUMPAD_END:      return WXK_END;
    case WXK_NUMPAD_PAGEUP:   return WXK_PAGEUP;
    case WXK_NUMPAD_PAGEDOWN: return WXK_PAGEDOWN;
    case WXK_NUMPAD_LEFT:     return WXK_LEFT;
    case WXK_NUMPAD_RIGHT:    return WXK_RIGHT;
    case WXK_NUMPAD_UP:       return WXK_UP;
    case WXK_NUMPAD_DOWN:     return WXK_DOWN;
    case WXK_NUMPAD_INSERT:   return WXK_INSERT;
    case WXK_NUMPAD_DELETE:   return WXK_DELETE;
    case WXK_NUMPAD_ADD:      return '+';
    case WXK_NUMPAD_SUBTRACT: return '-';
    case WXK_NUMPAD_MULTIPLY: return '*';
    case WXK_NUMPAD_DIVIDE:   return '/';
    case WXK_NUMPAD_DECIMAL:  return '.';
    case WXK_NUMPAD_EQUAL:    return '=';
    default:                  return key;
    }
}

// Modifiers in the order the text forms list them; wxMOD_RAW_CONTROL is wxMOD_CONTROL off macOS.
#ifdef __APPLE__
constexpr std::array<int, 4> MODIFIER_ORDER{ wxMOD_CONTROL, wxMOD_SHIFT, wxMOD_ALT, wxMOD_RAW_CONTROL };
#else
constexpr std::array<int, 3> MODIFIER_ORDER{ wxMOD_CONTROL, wxMOD_SHIFT, wxMOD_ALT };
#endif

const char* canonical_modifier_prefix(int modifier)
{
    if (modifier == wxMOD_CONTROL)
        return "Ctrl+";
    if (modifier == wxMOD_SHIFT)
        return "Shift+";
    if (modifier == wxMOD_ALT)
        return "Alt+";
    return "RawCtrl+";
}

template<typename Prefix>
std::string join_modifiers(int modifiers, Prefix prefix)
{
    std::string out;
    for (int modifier : MODIFIER_ORDER)
        if (modifiers & modifier)
            out += prefix(modifier);
    return out;
}

} // namespace

bool KeyChord::is_punctuation() const { return is_symbol(key) && modifiers == wxMOD_NONE; }

bool KeyChord::needs_char_event() const { return is_symbol(key) && (modifiers & ~wxMOD_SHIFT) == 0; }

bool KeyChord::is_menu_accelerator() const
{
    return valid() && ((modifiers & (wxMOD_CONTROL | wxMOD_ALT | wxMOD_RAW_CONTROL)) != 0 || (!is_printable(key) && key != WXK_SPACE));
}

std::string KeyChord::to_string() const
{
    if (!valid())
        return {};
    std::string out = join_modifiers(modifiers, canonical_modifier_prefix);
    if (is_printable(key))
        out += char(key);
    else
        out += special_key_name(key);
    return out;
}

std::optional<KeyChord> KeyChord::parse(const std::string& text)
{
    if (text.empty())
        return std::nullopt;

    // The key is whatever follows the last separator; a trailing '+' is the '+' key itself.
    size_t      key_start = text.size() - 1;
    if (text.back() != '+') {
        const size_t sep = text.rfind('+');
        key_start = sep == std::string::npos ? 0 : sep + 1;
    }
    KeyChord chord;
    chord.key = parse_key(text.substr(key_start));
    if (chord.key == WXK_NONE)
        return std::nullopt;

    const std::string prefix = key_start == 0 ? std::string() : text.substr(0, key_start - 1);
    size_t begin = 0;
    while (begin < prefix.size()) {
        size_t end = prefix.find('+', begin);
        if (end == std::string::npos)
            end = prefix.size();
        const int modifier = parse_modifier(prefix.substr(begin, end - begin));
        if (modifier == wxMOD_NONE)
            return std::nullopt;
        chord.modifiers |= modifier;
        begin = end + 1;
    }
    if (is_letter(chord.key) || is_digit(chord.key) || !is_printable(chord.key) || chord.modifiers == wxMOD_NONE)
        return chord;
    // Shift is folded into the character for punctuation, so "Shift+/" is not accepted.
    return (chord.modifiers & wxMOD_SHIFT) ? std::nullopt : std::optional<KeyChord>(chord);
}

std::string KeyChord::display() const
{
    std::string out;
    for (const std::string& part : display_parts())
        out += (out.empty() ? "" : "+") + part;
    return out;
}

std::vector<std::string> KeyChord::display_parts() const
{
    std::vector<std::string> parts;
    if (!valid())
        return parts;
    for (int modifier : MODIFIER_ORDER)
        if (modifiers & modifier)
            parts.push_back(modifier_name(modifier));
    parts.push_back(is_printable(key) ? std::string(1, char(key)) : special_key_label(key));
    return parts;
}

std::string KeyChord::modifier_prefix(int modifier)
{
    if (modifier == wxMOD_CONTROL)
        return shortkey_ctrl_prefix();
    if (modifier == wxMOD_SHIFT)
        return _u8L("Shift+");
    if (modifier == wxMOD_ALT)
        return shortkey_alt_prefix();
#ifdef __APPLE__
    if (modifier == wxMOD_RAW_CONTROL)
        return u8"⌃+";
#endif
    return {};
}

// The catalogue holds the "Ctrl+" prefixes, so the bare name is the prefix without its "+"
// and any space before it ("Strg +" in German).
std::string KeyChord::modifier_name(int modifier)
{
    std::string name = modifier_prefix(modifier);
    if (!name.empty() && name.back() == '+')
        name.pop_back();
    while (!name.empty() && name.back() == ' ')
        name.pop_back();
    return name;
}

wxAcceleratorEntry KeyChord::to_accelerator_entry(int command) const
{
    int flags = wxACCEL_NORMAL;
    if (modifiers & wxMOD_CONTROL)
        flags |= wxACCEL_CTRL;
    if (modifiers & wxMOD_SHIFT)
        flags |= wxACCEL_SHIFT;
    if (modifiers & wxMOD_ALT)
        flags |= wxACCEL_ALT;
#ifdef __APPLE__
    if (modifiers & wxMOD_RAW_CONTROL)
        flags |= wxACCEL_RAW_CTRL;
#endif
    return wxAcceleratorEntry(flags, key, command);
}

KeyChord KeyChord::from_event(const wxKeyEvent& evt)
{
    KeyChord chord;
    chord.modifiers = evt.GetModifiers() & BINDABLE_MODIFIERS;
    int key = fold_numpad(evt.GetKeyCode());

    if (evt.GetEventType() == wxEVT_CHAR) {
        if (key >= 1 && key <= 26 && evt.ControlDown())
            key = 'A' + key - 1;   // Ctrl+letter arrives as the control character
        else if (is_symbol(key))
            chord.modifiers &= ~wxMOD_SHIFT;   // the character already reflects Shift
    }
    if (key >= 'a' && key <= 'z')
        key -= 'a' - 'A';

    if (is_printable(key) || is_special(key))
        chord.key = key;
    return chord;
}

}} // namespace Slic3r::GUI
