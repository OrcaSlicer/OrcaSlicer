#include "SyntheticInput.hpp"

#include <wx/mousestate.h>
#include <wx/utils.h>

#include <algorithm>
#include <optional>
#include <vector>

namespace Slic3r { namespace GUI {

namespace {

std::vector<int>&           held_keys() { static std::vector<int> keys; return keys; }
std::optional<wxMouseState>& held_mouse() { static std::optional<wxMouseState> mouse; return mouse; }

// The modifier a key code stands for, so a held Shift also reads as WXK_SHIFT etc.
bool same_key(int held, int asked)
{
    if (held == asked)
        return true;
#ifdef __APPLE__
    // WXK_CONTROL is Command on macOS; WXK_RAW_CONTROL is the Control key.
    return false;
#else
    return (held == WXK_CONTROL && asked == WXK_RAW_CONTROL) || (held == WXK_RAW_CONTROL && asked == WXK_CONTROL);
#endif
}

} // namespace

namespace SyntheticInput {

void press_key(int key_code)
{
    if (!is_key_down(key_code))
        held_keys().push_back(key_code);
}

void release_key(int key_code)
{
    auto& keys = held_keys();
    keys.erase(std::remove(keys.begin(), keys.end(), key_code), keys.end());
}

bool is_key_down(int key_code)
{
    const auto& keys = held_keys();
    return std::any_of(keys.begin(), keys.end(), [key_code](int k) { return same_key(k, key_code); });
}

void set_mouse(const wxMouseState& state) { held_mouse() = state; }

void release_mouse() { held_mouse().reset(); }

void clear()
{
    held_keys().clear();
    held_mouse().reset();
}

} // namespace SyntheticInput

bool get_key_state(wxKeyCode key)
{
    if (SyntheticInput::is_key_down(int(key)))
        return true;
    return wxGetKeyState(key);
}

wxMouseState get_mouse_state()
{
    if (const auto& mouse = held_mouse())
        return *mouse;
    return wxGetMouseState();
}

}} // namespace Slic3r::GUI
