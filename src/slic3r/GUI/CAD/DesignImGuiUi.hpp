#ifndef slic3r_DesignImGuiUi_hpp_
#define slic3r_DesignImGuiUi_hpp_

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "slic3r/GUI/CAD/DesignRibbonTable.hpp"
#include "slic3r/GUI/CAD/DesignOffer.hpp"

namespace Slic3r { namespace GUI {

class ImGuiWrapper;

// The Design tab's navigation drawn on the canvas with ImGui (feature tree here, ribbon in
// DesignRibbon). These classes own NO document state: they take a read-only view each frame and hand
// back events, which DesignPanel applies on the next turn of the event loop through the very same
// handlers the wx tree calls. That is what makes the two UIs behave identically.

// ---- feature tree -------------------------------------------------------------------------
struct TreeRowView {
    std::string label;
    bool        suppressed{false};   // disabled: grey + struck through
    bool        failed{false};       // replay threw here: red + `note` in a tooltip
    bool        conflict{false};     // mate conflict: orange-red + `note`
    std::string note;
};

struct TreeView {
    std::vector<TreeRowView> rows;
    int  selected{-1};
    int  owner{-1};                  // the feature that made the face picked in the viewport: its row is tinted, not selected
    std::vector<int> selected_set;   // every selected row (Ctrl toggles, Shift extends); `selected` is the primary
    bool can_paste{false};           // the feature clipboard holds something
    int  rollback{-1};        // history is rolled back to this row; -1 = at the end
    bool show_origin{true};
    bool show_bed{true};
};

struct TreeEvent {
    enum Kind { Select, Deselect, Edit, ToggleEnabled, Delete, Zoom, MoveUp, MoveDown, Reorder, Rename, SetRollback,
                ToggleOrigin, ToggleBed, DockBack, Copy, Paste } kind{Deselect};
    int         row{-1};
    std::string text;   // Rename: the new name; Reorder: the destination row (row = the dragged one); Select: "ctrl" | "shift" | ""
};

class DesignTreeOverlay
{
public:
    std::function<TreeView()>                       get_view;   // rebuilt by the owner every frame (cheap: names + flags)
    std::function<void(std::vector<TreeEvent>)>     emit;       // events from this frame, applied later by the owner
    std::function<bool(int row)>                    can_zoom;   // called only while a row menu is open
    int                                             test_mods{0};   // rig only: modifiers a synthetic click carries (1 shift, 2 ctrl); ImGui learns real ones from key events

    // `inset_left/top` keep the window clear of the canvas's own corner widgets.
    void render(ImGuiWrapper& imgui, float scale, float inset_left, float inset_top);

    // Is the pointer over the tree window? (The canvas also asks ImGui, this is for the rig/tests.)
    bool hovered() const { return m_hovered; }
    // Last frame's row centres in screen pixels (x is shared): lets a rig test aim a drag at a row.
    const std::vector<float>& row_mids() const { return m_row_mid; }
    float row_x() const { return m_row_x; }

private:
    bool        m_hovered{false};
    int         m_rename_row{-1};
    char        m_rename_buf[128]{};
    bool        m_rename_focus{false};
    int         m_rename_age{0};
    int         m_menu_row{-1};
    int         m_drag_rollback{-1};   // where the bar is being dragged to (valid while m_dragging)
    bool        m_dragging{false};
    float       m_last_inset_top{-1.f};
    float       m_row_x{0.f};
    int         m_owner_shown{-1};   // the owner row already scrolled into view
    int         m_drag_src{-1};   // the row being dragged this frame
    std::vector<float> m_row_mid;      // screen y of each row centre, from the previous frame (the bar drag reads it)
};

// ---- ribbon ------------------------------------------------------------------------------------
// Top-anchored tabs -> groups -> tools (DesignRibbonTable.hpp). One function draws it, so a TabCtrl
// renderer could replace it without touching the table or the panel. A click only EMITS the verb;
// the panel runs it through the offer's own action router, i.e. the state machine the wx toolbar
// and the right-click menu already share (one Confirm/Cancel bar, Esc cancels).
class DesignRibbonOverlay
{
public:
    struct VerbState { bool enabled{true}; std::string why; };
    std::function<VerbState(const OfferVerb&)>                    state;
    std::function<void(const OfferVerb&)>                         run;
    std::function<bool()>                                         sketching;   // live sketch mode: show the Sketch tab
    std::function<std::string(const std::string& group)>          load_last;   // last-used variant of a split group
    std::function<void(const std::string& group, const std::string& verb_id)> save_last;

    void render(ImGuiWrapper& imgui, float scale, float inset_left, float inset_top);
    float height() const { return m_height; }
    bool  hovered() const { return m_hovered; }

private:
    std::vector<RibbonTab> m_tabs;
    bool   m_hovered{false};
    float  m_height{0.f};
    bool   m_was_sketching{false};
};

// ---- top bar --------------------------------------------------------------------------------------------
// The row above the viewport, drawn on the canvas: the document actions, one split button per toolbox group (the
// ribbon's own groups and rules, as icon buttons), Commit to Plate and the active tool's Confirm / Cancel. Like the ribbon it
// owns no document state: a click only EMITS, and the panel runs it on the next turn of the event loop.
class DesignTopBar
{
public:
    struct Doc    { std::string icon; std::string tip; std::function<void()> run; };
    struct Commit { std::string label; std::string tip; };
    std::vector<Doc>    docs;                  // New, Import STEP, Import mesh, Export
    std::vector<Commit> commit_modes;          // one row per mode of the Commit split button
    std::function<std::string(int)>                               commit_icon;       // the face of mode i, for the current theme
    std::function<int()>                                          commit_current;
    std::function<void(int)>                                      commit_choose;     // the ▾ list: select a mode
    std::function<void()>                                         commit_run;        // the face: commit in the current mode
    std::function<DesignRibbonOverlay::VerbState(const OfferVerb&)> state;
    std::function<void(const OfferVerb&)>                         run;
    std::function<std::string(const std::string& group)>          load_last;
    std::function<void(const std::string& group, const std::string& verb_id)> save_last;
    std::function<bool()>                                         action_active;     // a tool or mode is open: Confirm / Cancel show
    std::function<bool()>                                         confirm_enabled;   // would Confirm act right now
    std::function<void()>                                         confirm, cancel;

    // `inset_left` keeps the bar clear of the canvas's corner collapse button.
    void  render(ImGuiWrapper& imgui, float scale, float inset_left);
    float height() const { return m_height; }
    bool  hovered() const { return m_hovered; }

private:
    std::vector<RibbonTab> m_tabs;
    bool   m_hovered{false};
    float  m_height{0.f};
};

// ---- sidebar --------------------------------------------------------------------------------------------
// An icon by name (resources/images/<name>.svg), decolorized like the bars' icons: an ImTextureID, or null when the app ships
// no such file. For the code that draws the panel's own controls.
void* design_icon_texture(const std::string& name);

// The left panel as an ImGui window at the canvas's left edge, full height, its edge draggable. It draws nothing itself: the
// owner's `draw` fills it (DesignPanel mirrors its wx form there).
class DesignSidebarOverlay
{
public:
    std::function<void()>      draw;
    std::function<void(float)> on_width;   // the edge was dragged: the new width, in px at 100% scaling
    float                      width_px{440.f};

    void  render(ImGuiWrapper& imgui, float scale, float inset_top);
    float drawn_width() const { return m_drawn_w; }   // screen px, as drawn last
    bool  hovered() const { return m_hovered; }

private:
    bool   m_hovered{false};
    float  m_drawn_w{0.f};
    bool   m_dragging{false};
};

// ---- right-click offer menu as an ImGui popup (item 3) ----------------------------------------------
// The same rows the wx offer builds (flat rows first, then one submenu per family), as plain data.
struct OfferNode {
    std::string            label;
    std::string            shortcut;
    bool                   enabled{true};
    std::string            why;                 // tooltip, shown only while hovering a DISABLED item
    const OfferVerb*       verb{nullptr};       // null for a submenu
    bool                   separator_before{false};
    std::vector<OfferNode> children;            // non-empty = submenu
};

class DesignOfferPopup
{
public:
    std::function<void(const OfferVerb&)> run;   // the panel applies it next turn, through the offer's action router
    void open(std::string header, std::vector<OfferNode> nodes, float x, float y);
    void render(ImGuiWrapper& imgui, float scale);
    bool is_open() const { return m_open; }

private:
    bool                   m_open{false};
    bool                   m_opened_now{false};
    float                  m_x{0.f}, m_y{0.f};
    std::string            m_header;
    std::vector<OfferNode> m_nodes;
};

// ---- command palette (Ctrl+K) ------------------------------------------------------------------------
// A search box at the cursor over every command the ribbon knows. Typing ranks them (PaletteScore.hpp),
// Up/Down move, Enter runs the highlighted one, Esc or a click away closes. With an empty query the
// recently used commands come first. Disabled commands are listed (dimmed) and say why on hover.
struct PaletteItem {
    std::string      id;
    std::string      label;
    std::string      shortcut;
    bool             enabled{true};
    std::string      why;
    const OfferVerb* verb{nullptr};
};

class DesignPalette
{
public:
    std::function<std::vector<PaletteItem>()>            items;          // rebuilt on open: enabled state is live
    std::function<void(const OfferVerb&)>                run;
    std::function<std::vector<std::string>()>            load_recents;   // verb ids, most recent first
    std::function<void(const std::vector<std::string>&)> save_recents;

    void open(float x, float y);
    void render(ImGuiWrapper& imgui, float scale);
    bool is_open() const { return m_open; }
    // Keys arrive through the panel's shortcut gate, not through ImGui's per-frame key state (a press shorter
    // than a frame would be missed): dir = -1 / +1 moves the highlight, 0 runs it.
    void nav(int dir) { if (dir == 0) m_activate = true; else m_move += dir; }
    // For the rig: what the box shows right now (query, ranked ids, highlighted index).
    const std::string&              query() const { return m_query; }
    const std::vector<std::string>& shown() const { return m_shown; }
    int                             cursor() const { return m_cursor; }
    std::string label_of(const std::string& id) const { for (const PaletteItem& i : m_items) if (i.id == id) return i.label; return {}; }

private:
    bool                     m_open{false};
    bool                     m_opened_now{false};
    float                    m_x{0.f}, m_y{0.f};
    char                     m_buf[128]{};
    std::string              m_query;
    int                      m_cursor{0};
    int                      m_move{0};
    bool                     m_activate{false};
    std::vector<PaletteItem> m_items;
    std::vector<std::string> m_shown;
};

// Select Other: the list of everything under the pointer after a 0.5 s press-hold. Hovering a row previews that pick,
// clicking a row takes it; a click elsewhere or Esc closes it and drops the preview.
class DesignCandidateMenu
{
public:
    std::function<void(int)> on_hover;    // row index, or -1 when no row is hovered / the menu closed
    std::function<void(int)> on_choose;

    void open(float x, float y, std::vector<std::string> labels);
    void render(ImGuiWrapper& imgui, float scale);
    bool is_open() const { return m_open; }
    // For the rig: drive a row without knowing where ImGui drew it.
    void request_hover(int i) { m_force_hover = i; }
    void request_choose(int i) { m_force_choose = i; }
    const std::vector<std::string>& labels() const { return m_labels; }
    int hovered() const { return m_hover; }

private:
    bool                     m_open{false}, m_opened_now{false};
    float                    m_x{0.f}, m_y{0.f};
    int                      m_hover{-1}, m_force_hover{-2}, m_force_choose{-1};
    std::vector<std::string> m_labels;
};

// Does an ImGui text field hold the keyboard right now? The panel's single shortcut gate asks this next
// to "is a wx text control focused", so a letter typed into ANY ImGui field is never also a tool key.
bool imgui_wants_text();

// An ImGui popup (ribbon dropdown, tree row menu) is open anywhere? Esc closes it, before the
// panel's own Esc ladder, which knows nothing about ImGui.
bool imgui_popup_open();
void imgui_close_popups();

}} // namespace Slic3r::GUI

#endif // slic3r_DesignImGuiUi_hpp_
