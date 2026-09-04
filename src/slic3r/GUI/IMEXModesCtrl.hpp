#ifndef slic3r_IMEXModesCtrl_hpp_
#define slic3r_IMEXModesCtrl_hpp_

#include <wx/panel.h>
#include <wx/string.h>
#include <wx/sizer.h>
#include <wx/button.h>
#include <wx/textctrl.h>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "libslic3r/IMEXHelpers.hpp"

class ScalableButton;

namespace Slic3r {
namespace GUI {

// ---------------------------------------------------------------------------
// IMEXModesCtrl — visual parallel-mode editor for the IDEX/IQEX printer tab
//
// A tool tile holds an `std::optional<ImexRole>`: the role the mode gives that tool, or
// nullopt for Inactive (grey — the tool does not take part in this mode). Inactive is the
// editor's own state and the only reason the tile is not a bare ImexRole; it is the absence
// of a role, so it is spelled as one rather than as an extra enumerator that libslic3r would
// then have to keep rejecting. Roles themselves are never re-encoded here — no int state
// table, no second copy of the on-disk letters.
//
//   nullopt          (grey)   — not participating in this mode
//   ImexRole::Primary (green) — the tool the slicer generates paths for
//   ImexRole::Copy    (blue)  — firmware duplicates Primary at an offset
//   ImexRole::Mirror  (amber) — firmware mirrors Primary about an axis
//   ImexRole::Span    (yellow)— multicolor partner of Primary on the same gantry;
//                               declares paired-gantry multicolor topology and
//                               unlocks the multicolor block at slice time. Only
//                               offered on tools sharing primary's gantry, and
//                               only on multi-gantry printers (n_rows >= 2).
//
// Colour and legend text are the editor's business and live in role_style(); the tile cycle
// order and the serialized letters come from kImexRoleTable in IMEXHelpers.hpp, so the
// editor cannot drift from the slicer's reading of a mode string.
//
// Active-tools string format: "idx:P,idx:C,idx:M,idx:S"  (backwards-compat: plain "idx" = Primary)
// ---------------------------------------------------------------------------
class IMEXModesCtrl : public wxPanel {
public:
    std::function<void()> on_change;

    // Fired after on_change has written the shortened table back to the preset, with EVERY
    // name the deleted row could be known by: the name the row was built with (what a plate
    // set from the saved preset holds) and the name in the field at delete time (what a plate
    // set after an in-session rename holds). Renaming is deliberately not routed through here
    // -- the name field notifies on every keystroke -- so by the time the row is deleted the
    // two can differ, and reporting only the current name leaves the plate pointing at a mode
    // that no longer exists. Names a surviving row still carries are filtered out, so renaming
    // one row to another's old name and then deleting cannot reset a plate that legitimately
    // resolves elsewhere. Plates still set to any reported name are stale from this point on
    // -- see the handler in TabPrinter::build_fff.
    std::function<void(const std::vector<std::string>&)> on_mode_removed;

    // Lazy lookup so the widget always sees the current parent (preset switches
    // change the parent under us). Returns nullptr when the active preset has no
    // parent (system preset itself), which disables per-row reset arrows.
    using ParentConfigLookup = std::function<const DynamicPrintConfig*()>;
    void set_parent_config_lookup(ParentConfigLookup f) { m_parent_lookup = std::move(f); }

    // layout: 0=front-left, 1=front-right, 2=rear-left, 3=rear-right (T0 corner).
    // The enum values intentionally line up 1:1 so this is a no-op cast — the helper
    // is kept to make the conversion explicit at call sites.
    static int parse_layout(ImexToolLayout layout) { return static_cast<int>(layout); }

    IMEXModesCtrl(wxWindow* parent, int n_cols, int n_rows, int layout = 0);

    void set_grid_size(int n_cols, int n_rows, int layout = -1);

    void load_from_config(const DynamicPrintConfig& cfg);

    // Every row on screen produces exactly one entry — a row is never skipped.
    // Skipping empty-named rows used to drop the row's tool roles and G-code
    // silently, and because matches_config() compares against this same filtered
    // output the preset did not even go dirty to hint that something had gone
    // missing; the row stayed on screen until the next load_from_config() quietly
    // removed it. add_row() pre-fills a name and the Name field restores one when
    // it is left empty, so the substitution below is the last-resort guard for a
    // name that arrives empty from a hand-edited preset or 3MF.
    std::tuple<std::vector<std::string>, std::vector<std::string>, std::vector<std::string>>
    get_mode_data() const;

    // True when the widget's current rows already mirror the three IMEX-mode option
    // vectors in cfg. Used by TabPrinter::update_fff() to skip a destroy-and-rebuild
    // pass when the only thing that just changed was the widget itself writing back
    // to config — without this guard, every keystroke in the gcode textbox triggers
    // a load_from_config() that detaches the textbox the user is typing in.
    bool matches_config(const DynamicPrintConfig& cfg) const;

private:
    // map<physical tool idx, role> for one `imex_mode_active_tools` entry. A tool absent
    // from the map is Inactive — the same convention the serialized string uses, so the
    // round trip needs no separate "off" value.
    //
    // Deliberately NOT a parser of its own: the slicer decides what actually prints,
    // so the editor has to read the string exactly the way the slicer does or the
    // same mode means two different things in the two places. Roles come from
    // parse_imex_active_tools(); the primary index comes from
    // imex_primary_tool_for_mode(), which is where the "a bare index is Primary"
    // backwards-compat rule lives. Both are needed: parse_imex_active_tools()
    // defaults a bare token to Copy (deliberately — see the note above
    // parse_imex_active_tools() in IMEXHelpers.hpp), so only the combination
    // reproduces the slicer's reading of a legacy "0,1,2" string, which is
    // "T0 primary, the rest copies".
    //
    // Indices are PHYSICAL tool indices (T0..TN-1). all_tool_roles preserves
    // off-grid entries so the tile widget can round-trip indices that fall
    // outside the currently visible rows × cols without losing data on save.
    static std::map<int, ImexRole> roles_for_mode(const std::string& active_tools);

    // "Mode N" for the lowest N >= 2 not already used by a row or by `also_taken`
    // (N == 1 is conceptually the fixed Primary row). Deterministic for a given set
    // of rows, so get_mode_data() is stable across calls and matches_config() stays
    // honest. Intentionally NOT translated: mode names are identifiers — objects
    // store one in `imex_parallel_mode` and GCode.cpp matches it against
    // `imex_mode_names` by string — so a locale-dependent name would break a project
    // opened under a different language.
    std::string unique_mode_name(const std::vector<std::string>& also_taken) const;

    // Everything the editor draws for one role, in ONE place: a new role needs a colour and
    // a legend name here and nowhere else in this file. nullopt is Inactive (grey).
    // `label` is deliberately untranslated — it names a role, and the legend sits next to
    // the same letters the preset stores.
    struct RoleStyle { wxColour bg; wxColour fg; const char* label; };
    static RoleStyle role_style(std::optional<ImexRole> role);

    // True when this printer offers `role` at all, regardless of which tile is clicked.
    // Span needs a second gantry to be a partner across, so a single-gantry printer neither
    // cycles to it nor lists it in the legend.
    bool role_offered(ImexRole role) const;

    // True when the tile for `tool_idx` may take `role` right now. Adds the positional rules
    // on top of role_offered(): only one tile may hold Primary, and Span only on a tile
    // sharing the Primary's gantry.
    bool role_allowed_on_tile(ImexRole role, int tool_idx,
                              bool other_primary, int primary_gantry) const;

    // Next state in the tile cycle: Inactive → kImexRoleTable order → Inactive, stepping
    // over any role `allowed` rejects. Inactive is always reachable, so it terminates.
    static std::optional<ImexRole> next_tile_role(std::optional<ImexRole> current,
                                                  const std::function<bool(ImexRole)>& allowed);

    void apply_btn(wxButton* btn, int tool_idx, std::optional<ImexRole> role);

    struct Row {
        wxPanel*             panel;
        wxTextCtrl*          name;       // nullptr for primary row (name is fixed)
        wxTextCtrl*          gcode;
        std::vector<wxButton*> btns;
        std::vector<std::optional<ImexRole>> btn_roles;   // nullopt == Inactive
        std::vector<int>     btn_tool_idx;
        std::map<int, ImexRole> all_tool_roles;           // absent == Inactive
        // Name the row was built with (UTF-8), i.e. the name that was in the preset — and so
        // in the plate's mode selector — when the row appeared. Kept because the name field is
        // free to drift from it without the row being rebuilt; remove_row() needs both to find
        // the plates a delete strands. Rebuilding the rows (load_from_config / set_grid_size)
        // re-seeds it, which is correct: config and rows agree again at that point.
        std::string          orig_name;
        bool                 is_primary {false};
        ScalableButton*      reset_btn {nullptr};   // nullptr when row has no parent counterpart
        bool                 reset_dirty_cached {false};
    };

    // Snapshot of row content as it would be saved (matches get_mode_data() per-row).
    struct RowSnapshot { std::string name, tools, gcode; };
    RowSnapshot snapshot_row(const Row& r) const;

    // True when row at `row_idx` differs from the parent preset's value at the same
    // index. Returns false when there is no parent (system preset), when the row is
    // beyond the parent's mode count (user added it — no defined "default"), or when
    // the lookup callback isn't wired. The Primary row's name is sentinel-fixed so
    // a name diff doesn't count for it; only tools/gcode do.
    bool row_differs_from_parent(int row_idx) const;

    // True when row index has a parent counterpart at all (i.e., the reset arrow
    // should be present on the row at all, regardless of dirty state).
    bool row_has_parent_counterpart(int row_idx) const;

    void add_row(const std::string& name = "",
                 const std::string& active_tools = "",
                 const std::string& gcode = "",
                 bool is_primary = false);

    // Returns every name the removed row was known by (UTF-8, deduplicated: its build-time
    // name and its current name, which differ after an in-session rename), minus any name a
    // surviving row still carries. The caller uses them to find the plates the delete
    // stranded. Empty when nothing was removed, when the row is the non-deletable Primary,
    // or when both names are blank -- a blank name is only reachable on a row that was never
    // named, which no plate can be holding.
    std::vector<std::string> remove_row(wxPanel* panel);

    void clear_rows();

    std::string active_tools_string(const Row& r) const;

    void notify();

    // Update each row's reset bitmap to reflect current dirty state. Cached so we
    // only swap the bitmap when state actually transitions — avoids flicker on
    // every keystroke. Disabled buttons (no saved counterpart) keep their dot.
    void refresh_reset_buttons();

    // Reset row identified by `panel` (stable across add/remove) to the parent
    // preset's value at the same index. Primary row's name stays sentinel-fixed —
    // only tools and gcode are restored.
    void reset_row_to_parent(wxPanel* panel);

    // Rebuild the instruction text, colour legend and column headers for the CURRENT grid.
    // These depend on the grid shape -- the Span role is only offered on a multi-gantry printer,
    // and the header spacing is derived from the column count -- so they cannot be built once in
    // the constructor: adding a second gantry has to reveal Span without a save-and-reopen cycle.
    void rebuild_info_and_header();

    // One-line explanation per role, shown as a tooltip on that role's legend swatch and label.
    // Keeps the per-role detail out of the instruction paragraph.
    static wxString role_hint(ImexRole role);

    wxBoxSizer*         m_outer;
    wxBoxSizer*         m_rows_sizer;
    wxPanel*            m_info_panel = nullptr;
    wxPanel*            m_hdr_panel  = nullptr;
    std::vector<Row>    m_rows;
    int                 m_n_cols, m_n_rows;
    int                 m_layout {0}; // 0=front-left, 1=front-right, 2=rear-left, 3=rear-right
    bool                m_clearing_rows {false}; // guards handlers against teardown-time events
    ParentConfigLookup  m_parent_lookup;
};

}} // namespace Slic3r::GUI

#endif // slic3r_IMEXModesCtrl_hpp_
