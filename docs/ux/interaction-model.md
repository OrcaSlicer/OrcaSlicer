# Design tab — interaction model

The contract for Esc, the right mouse button, and the states between them. Code that changes any
of the three changes this file in the same commit.

## 1. The state machine

`src/slic3r/GUI/CAD/DesignInteraction.hpp` — a four-level LIFO stack. The enum value *is* the
depth, so "which level does this press belong to" is a comparison rather than a chain of
special cases spread over three files.

```cpp
enum class CadLevel : int {
    Idle      = 0,   // nothing transient is up: Esc clears the selection
    Tool      = 1,   // a feature card / armed sketch tool / constrain session: Esc exits it
    Gesture   = 2,   // an uncommitted delta (entity being drawn, body being dragged): Esc reverts it
    Transient = 3,   // a value field or a popup menu: Esc closes just that
};

struct CadInteractionState {          // the four bits routing actually needs
    bool value_field_open{false};
    bool gesture_active{false};
    bool tool_armed{false};
    bool has_selection{false};
};

constexpr CadLevel cad_escape_level(const CadInteractionState& s)
{
    if (s.value_field_open) return CadLevel::Transient;
    if (s.gesture_active)   return CadLevel::Gesture;
    if (s.tool_armed)       return CadLevel::Tool;
    return CadLevel::Idle;
}
```

The rule is a `constexpr` free function over a POD, not a method on the panel, so the ordering
that is the entire contract is checkable without a window, a GL context or an event loop. Five
`static_assert`s in the header do exactly that, at compile time.

**Strict invariant.** No level of Esc deletes a feature, discards a sketch that holds geometry,
or rolls history back. Destroying work needs a gesture that says so:

| To destroy | Gesture |
|---|---|
| a feature | Delete / Backspace on an explicit selection |
| a drawn sketch | the ribbon's ✗ Cancel, which asks first |
| the last committed change | Ctrl+Z |

## 2. Event routing

**`OnKeyDown(WXK_ESCAPE)`** — `DesignPanel`'s `wxEVT_CHAR_HOOK`, one line:

```cpp
if (key == WXK_ESCAPE) { escape(); return; }
```

Every Esc in the tab goes through it, whatever holds focus. `DesignPanel::escape_level()` answers
the four questions of `CadInteractionState` about this panel; `DesignPanel::escape()` acts on the
one level that answer names, and on no other:

| Level | What one press does | What it must not touch |
|---|---|---|
| `Transient` | close the value field (`cancel_value` / `inline_cancel`) | the tool, which stays armed |
| `Gesture` | drop the clicks of the entity being drawn, or put a moved body back at the pose it had when the gizmo appeared | everything already committed |
| `Tool` | discard a feature card's *candidate*; drop an armed sketch tool to Select; end Constrain | committed features; entities already drawn |
| `Idle` | clear the selection (model and sketch); leave a sketch session **only if it is empty** | a sketch holding geometry — it is left through Finish or Cancel |

A sketch *session* is deliberately not a `Tool`. It is the environment the Idle level lives in,
which is what makes the destructive path unrepresentable rather than merely unlikely.

**`OnRightDown` / `OnRightUp`** — `DesignCanvas::set_on_context_menu`, bound after `GLCanvas3D`'s
own handlers so it can consume the event before them:

```cpp
RIGHT_DOWN: remember the press position and the clock, then Skip()   // the canvas still seeds the orbit

RIGHT_UP:   terminated = sketch_tool.take_right_consumed();          // read-and-clear, always
            is_click   = drift <= 3 px && dt <= 200 ms;              // both budgets, or it was navigation
            if (callback && !terminated && !inline_busy && is_click) {
                select_at_screen(press.x, press.y);                  // raycast at the PRESS, not the release
                on_context_menu(ClientToScreen(press));
                return;                                              // consumed
            }
            Skip();                                                  // orbit / pan / the handlers underneath
```

Two independent budgets because the two failure modes are independent: drift alone still popped a
menu at the end of a slow, careful orbit. `take_right_consumed()` is how a right-click that
already meant something to the armed sketch tool (terminate a chain, drop an edit-op) declines to
also mean "open a menu".

## 3. Transition table

`sel` = something is picked. Blank = the input does nothing at that state.

| State | Left-click | Right-click | Esc | Enter |
|---|---|---|---|---|
| **Idle — model view** | pick / escalate the pick | offer menu for what is under the cursor | clear the selection | — |
| **Idle — sketch, empty** | pick | sketch offer menu | leave the session (nothing to lose) | Finish sketch |
| **Idle — sketch, drawn** | pick | sketch offer menu | clear the selection; status says the sketch is kept | Finish sketch |
| **Tool — feature card** | pick the card's next reference | offer menu | discard the candidate, close the card | commit the feature |
| **Tool — sketch tool armed** | place the first point | drop the tool to Select | drop the tool to Select | — |
| **Tool — constrain** | pick an entity | offer menu | end the session | apply |
| **Gesture — drawing** | place the next point | terminate the chain (keep what is drawn) | drop the in-progress entity, tool stays armed | commit the entity as drawn |
| **Gesture — moving a body** | drop the body here | end the move | revert to the pose at move-start | keep the placement |
| **Transient — value field** | — | — | close the field, tool stays armed | commit the value, advance the chain |
| **Transient — popup menu** | run the entry | — | close the menu | run the highlighted entry |
| **any** | — | — | *never* deletes, discards or rolls back | — |

Right-hold-and-drag is not in the table on purpose: past 3 px or 200 ms it is navigation, and
navigation does not transition the state machine.
