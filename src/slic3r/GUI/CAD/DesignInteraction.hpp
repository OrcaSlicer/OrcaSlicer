#ifndef slic3r_GUI_DesignInteraction_hpp_
#define slic3r_GUI_DesignInteraction_hpp_

namespace Slic3r { namespace GUI {

// The Design tab's interaction stack, and the ONE rule Esc obeys.
//
// Esc unwinds exactly one level per press, deepest first, and never more. The enum value IS
// the LIFO depth, so "which level does this press belong to" is a comparison, not a chain of
// special cases scattered over three files — which is what it was, and why two presses in a
// row could reach past a tool and destroy the sketch underneath it.
//
// STRICT INVARIANT (the bug this exists to make unrepresentable): no level of Esc deletes a
// feature, discards a sketch that holds geometry, or rolls history back. Destroying work needs
// a gesture that says so — Delete/Backspace on an explicit selection, the banner's Cancel, or
// Ctrl+Z. An Esc that can destroy is an Esc nobody can press with confidence, and being the
// safe key is the whole point of it.
enum class CadLevel : int {
    Idle      = 0,   // nothing transient is up: Esc clears the selection
    Tool      = 1,   // a feature card / armed sketch tool / constrain session: Esc exits it
    Gesture   = 2,   // an uncommitted delta (entity being drawn, body being dragged): Esc reverts it
    Transient = 3,   // a value field or a popup menu: Esc closes just that
};

// What the tab is doing, reduced to the four bits the routing actually needs. Kept as a POD of
// answers rather than a pointer to the panel so the rule below is decidable — and checkable —
// without a window, a GL context or an event loop.
struct CadInteractionState {
    bool value_field_open{false};   // in-canvas value field, or the panel's value card
    bool gesture_active{false};     // in-progress entity points, or a body being moved
    bool tool_armed{false};         // feature card open, sketch draw tool armed, constrain session
    bool has_selection{false};      // something is picked (model or sketch)
};

// The whole routing rule. Deepest live level wins; Idle is the floor.
constexpr CadLevel cad_escape_level(const CadInteractionState& s)
{
    if (s.value_field_open) return CadLevel::Transient;
    if (s.gesture_active)   return CadLevel::Gesture;
    if (s.tool_armed)       return CadLevel::Tool;
    return CadLevel::Idle;
}

// The ordering is the entire contract, so it is checked where it is defined, at compile time.
static_assert(cad_escape_level({true, true, true, true})     == CadLevel::Transient, "value field is deepest");
static_assert(cad_escape_level({false, true, true, true})    == CadLevel::Gesture,   "gesture beats tool");
static_assert(cad_escape_level({false, false, true, true})   == CadLevel::Tool,      "tool beats idle");
static_assert(cad_escape_level({false, false, false, true})  == CadLevel::Idle,      "selection is idle-level");
static_assert(cad_escape_level({false, false, false, false}) == CadLevel::Idle,      "empty is idle");

// Right-click vs. right-hold-orbit. A press that stays put and is let go promptly is a click and
// summons the offer; anything longer or further was navigation, and navigation must never be
// rewarded with a menu over wherever the camera happened to stop.
inline constexpr int kCadRightClickMs      = 200;   // press->release budget
inline constexpr int kCadRightClickDriftPx = 3;     // cursor drift budget, max(|dx|,|dy|)

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_DesignInteraction_hpp_
