#include "slic3r/GUI/IMEXModesCtrl.hpp"

#include <wx/app.h>
#include <wx/font.h>
#include <wx/stattext.h>

#include <algorithm>
#include <utility>

#include "slic3r/GUI/EditGCodeDialog.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/wxExtensions.hpp"

namespace Slic3r {
namespace GUI {

IMEXModesCtrl::IMEXModesCtrl(wxWindow* parent, int n_cols, int n_rows, int layout)
    : wxPanel(parent, wxID_ANY), m_n_cols(std::max(1, n_cols)), m_n_rows(std::max(1, n_rows)), m_layout(layout)
{
    // Pull the app's window-default colour explicitly. Without this, GTK gives
    // child wxPanels a slightly lighter "widget bg" instead of the app's dark
    // theme — making chromeless ScalableButtons inside the panel render with a
    // visible light box around the icon. Sub-panels inherit this colour.
    SetBackgroundColour(wxGetApp().get_window_default_clr());

    m_outer = new wxBoxSizer(wxVERTICAL);
    m_rows_sizer = new wxBoxSizer(wxVERTICAL);

    // Info panel (instructions + legend) and the column-header row. Both depend on the grid
    // shape, so their CONTENTS are (re)built by rebuild_info_and_header(), which set_grid_size()
    // calls again whenever the grid changes. The panels themselves are stable so m_outer's
    // ordering never has to be rearranged.
    m_info_panel = new wxPanel(this, wxID_ANY);
    m_info_panel->SetBackgroundColour(GetBackgroundColour());
    m_hdr_panel = new wxPanel(this, wxID_ANY);
    m_hdr_panel->SetBackgroundColour(GetBackgroundColour());
    rebuild_info_and_header();

    auto* add_btn = new wxButton(this, wxID_ANY, _L("+ Add Mode"),
                                 wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    add_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { add_row(); notify(); });

    m_outer->Add(m_info_panel, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
    m_outer->Add(m_hdr_panel,  0, wxEXPAND | wxBOTTOM, FromDIP(2));
    m_outer->Add(m_rows_sizer, 0, wxEXPAND);
    m_outer->AddSpacer(FromDIP(4));
    m_outer->Add(add_btn, 0);
    // SetSizer(), not SetSizerAndFit(): no row exists yet, so there is nothing to fit to --
    // rows arrive from add_row() / load_from_config() / set_grid_size(). Deliberately NOT
    // followed by m_outer->SetSizeHints(this) once they do: this panel owns a sizer, so
    // wxWindow::GetBestSize() bypasses the best-size cache and re-runs m_outer->CalcMin()
    // on every parent layout, and that is what the enclosing sizer already reserves space
    // for via GetEffectiveMinSize(). An explicitly set min size takes PRIORITY over the best
    // size in that call, so a size hint would pin the reserved height to the row count that
    // happened to be on screen when it ran -- adding a mode after that would clip the bottom
    // row and the "+ Add Mode" button, which is the very failure the hint is meant to avoid.
    SetSizer(m_outer);
}

void IMEXModesCtrl::rebuild_info_and_header() {
    // --- Info panel: instruction text + colour legend ---
    m_info_panel->DestroyChildren();
    auto* info_sizer = new wxBoxSizer(wxVERTICAL);

    // Keep this to one line. Per-role detail lives in the legend tooltips below, so the
    // panel does not open with a paragraph the user has to read before touching anything.
    const wxString instructions =
        _L("Each mode names the tool heads that take part and the role each one plays. "
           "Click a tool button to cycle its role — hover a colour below for what each role does.");
    auto* inst = new wxStaticText(m_info_panel, wxID_ANY, instructions);
    inst->Wrap(FromDIP(620));
    info_sizer->Add(inst, 0, wxBOTTOM, FromDIP(6));

    // Color legend — swatches sized to the body text height so they read as
    // matched pairs with their labels regardless of system DPI / font scale,
    // matching the on-hover ghost tooltip swatch's visual weight.
    // One entry per role this printer offers, in kImexRoleTable order — the same order the
    // tiles cycle in. A role added to that table appears here with no edit of its own.
    auto* leg_sizer = new wxBoxSizer(wxHORIZONTAL);
    const int swatch_side = m_info_panel->GetCharHeight();
    for (const ImexRoleDesc& d : kImexRoleTable) {
        if (!role_offered(d.role)) continue;
        const RoleStyle style = role_style(d.role);
        auto* swatch = new wxPanel(m_info_panel, wxID_ANY, wxDefaultPosition, wxSize(swatch_side, swatch_side));
        swatch->SetBackgroundColour(style.bg);
        auto* leg_label = new wxStaticText(m_info_panel, wxID_ANY, style.label);
        const wxString hint = role_hint(d.role);
        swatch->SetToolTip(hint);
        leg_label->SetToolTip(hint);
        leg_sizer->Add(swatch, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
        leg_sizer->Add(leg_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(16));
    }
    info_sizer->Add(leg_sizer, 0, wxBOTTOM, FromDIP(6));
    m_info_panel->SetSizerAndFit(info_sizer);

    // --- Column header row ---
    // Spacers sized to align with the mode-row fields below. Name field: FromDIP(130) + FromDIP(6)
    // gap; tool grid: n_cols*(FromDIP(36)+FromDIP(2))-FromDIP(2) + FromDIP(6) gap. grid_px tracks
    // the live column count, so the G-code header stays aligned after a grid change.
    m_hdr_panel->DestroyChildren();
    auto* hdr_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto* hdr_name  = new wxStaticText(m_hdr_panel, wxID_ANY, _L("Name"));
    auto* hdr_tools = new wxStaticText(m_hdr_panel, wxID_ANY, _L("Tools"));
    auto* hdr_gcode = new wxStaticText(m_hdr_panel, wxID_ANY, _L("G-code"));
    hdr_name->SetToolTip(_L("How the mode is labelled in the plate's IDEX/IQEX mode selector. "
                            "Required — a mode with no name cannot be selected."));
    hdr_tools->SetToolTip(_L("Which tool heads take part in the mode and what role each one plays. "
                             "Click a tile to cycle its role."));
    hdr_gcode->SetToolTip(_L("G-code run at print start to put the printer into this mode "
                             "(for example a Klipper SET_PRINT_MODE call). The slicer only emits "
                             "the Primary tool's paths; the firmware drives the others."));
    const int grid_px = m_n_cols * FromDIP(38) - FromDIP(2);  // approx grid panel width
    const int name_col_px = FromDIP(136);
    hdr_sizer->Add(hdr_name,  0, wxALIGN_CENTER_VERTICAL);
    hdr_sizer->AddSpacer(std::max(0, name_col_px - hdr_name->GetBestSize().x));
    hdr_sizer->Add(hdr_tools, 0, wxALIGN_CENTER_VERTICAL);
    hdr_sizer->AddSpacer(std::max(0, grid_px + FromDIP(6) - hdr_tools->GetBestSize().x));
    hdr_sizer->Add(hdr_gcode, 1, wxALIGN_CENTER_VERTICAL);
    m_hdr_panel->SetSizerAndFit(hdr_sizer);
}

void IMEXModesCtrl::set_grid_size(int n_cols, int n_rows, int layout) {
    n_cols = std::max(1, n_cols);
    n_rows = std::max(1, n_rows);
    if (layout < 0) layout = m_layout;
    if (n_cols == m_n_cols && n_rows == m_n_rows && layout == m_layout) return;
    auto [names, tools, gcodes] = get_mode_data();
    clear_rows();
    m_n_cols = n_cols;
    m_n_rows = n_rows;
    m_layout = layout;
    // Span becomes available the moment a second gantry exists, and the header spacing tracks the
    // column count -- so the legend and headers are rebuilt here rather than only at construction.
    // Without this, adding a gantry gives the tiles a Span role the legend never explains until
    // the preset is saved and the page reopened.
    rebuild_info_and_header();
    for (size_t i = 0; i < names.size(); ++i)
        add_row(names[i], tools[i], gcodes[i], /*is_primary=*/(names[i] == kImexPrimaryMode));
    Layout();
}

void IMEXModesCtrl::load_from_config(const DynamicPrintConfig& cfg) {
    clear_rows();
    // One ImexMode per row of imex_mode_names, with any sibling array too short for a row
    // padded to an empty string -- exactly what the three per-index bounds checks this
    // replaced were doing, now in one place shared with every other IMEX consumer.
    const std::vector<ImexMode> table = imex_mode_table(cfg);

    // Primary row is always first and non-deletable.  Look for an existing
    // "primary" entry in the config (present in configs saved after #8 was
    // implemented); fall back to empty tool/gcode for older configs.
    const ImexMode primary = find_imex_mode(cfg, kImexPrimaryMode);
    add_row(kImexPrimaryMode, primary.active_tools, primary.gcode, /*is_primary=*/true);

    for (const ImexMode& m : table) {
        if (m.index == primary.index) continue; // already added above
        add_row(m.name, m.active_tools, m.gcode);
    }
    refresh_reset_buttons();
    Layout();
}

std::tuple<std::vector<std::string>, std::vector<std::string>, std::vector<std::string>>
IMEXModesCtrl::get_mode_data() const {
    std::vector<std::string> names, tools, gcodes;
    for (auto& r : m_rows) {
        // into_u8(), not ToStdString(): everything downstream of here -- the preset, the 3MF
        // metadata, the plate's stored mode name -- is UTF-8, while ToStdString() encodes
        // through wxConvLibc, which on Windows is the ANSI codepage. The two agree only on a
        // UTF-8 locale, so mixing them turns a non-ASCII mode name into mojibake (or an empty
        // field, once from_u8() rejects it) on the round trip through the preset.
        std::string nm = r.is_primary ? std::string(kImexPrimaryMode) : into_u8(r.name->GetValue());
        if (nm.empty()) nm = unique_mode_name(names);
        names.push_back(nm);
        tools.push_back(active_tools_string(r));
        gcodes.push_back(into_u8(r.gcode->GetValue()));
    }
    return {names, tools, gcodes};
}

bool IMEXModesCtrl::matches_config(const DynamicPrintConfig& cfg) const {
    auto [names, tools, gcodes] = get_mode_data();
    auto cfg_strings = [&cfg](const char* key) {
        std::vector<std::string> v;
        if (auto* o = cfg.option<ConfigOptionStrings>(key)) v = o->values;
        return v;
    };
    return names  == cfg_strings("imex_mode_names")
        && tools  == cfg_strings("imex_mode_active_tools")
        && gcodes == cfg_strings("imex_mode_gcodes");
}

std::map<int, ImexRole> IMEXModesCtrl::roles_for_mode(const std::string& active_tools) {
    std::map<int, ImexRole> roles;
    for (const auto& [phys, role] : parse_imex_active_tools(active_tools))
        roles[phys] = role;
    // Promote the mode's primary, but never invent a tool that isn't in the
    // slicer's own active-tool list: parse_imex_active_tools() drops negative and
    // out-of-range indices, imex_primary_tool_for_mode() does not range-check the
    // upper bound, and a junk index round-tripped back out on save would be a
    // token the slicer ignores.
    const int primary = imex_primary_tool_for_mode(active_tools);
    if (primary >= 0) {
        auto it = roles.find(primary);
        if (it != roles.end())
            it->second = ImexRole::Primary;
    }
    return roles;
}

std::string IMEXModesCtrl::unique_mode_name(const std::vector<std::string>& also_taken) const {
    auto is_taken = [&](const std::string& cand) {
        if (std::find(also_taken.begin(), also_taken.end(), cand) != also_taken.end())
            return true;
        for (const auto& r : m_rows)
            if (!r.is_primary && r.name && into_u8(r.name->GetValue()) == cand)
                return true;
        return false;
    };
    for (int n = 2; ; ++n) {
        std::string cand = "Mode " + std::to_string(n);
        if (!is_taken(cand))
            return cand;
    }
}

IMEXModesCtrl::RoleStyle IMEXModesCtrl::role_style(std::optional<ImexRole> role) {
    // Grey / "Inactive" is the no-role answer; every role gets an explicit case so a new
    // one is a compile-time -Wswitch prompt rather than a tile that silently renders grey.
    if (!role)
        return { wxColour(90, 90, 90), *wxWHITE, "Inactive" };
    switch (*role) {
    case ImexRole::Primary: return { wxColour(50,  160, 50),  *wxWHITE, "Primary" };  // green
    case ImexRole::Copy:    return { wxColour(60,  120, 210), *wxWHITE, "Copy"    };  // blue
    case ImexRole::Mirror:  return { wxColour(210, 130, 20),  *wxWHITE, "Mirror"  };  // amber
    case ImexRole::Span:    return { wxColour(180, 180, 40),  *wxWHITE, "Span"    };  // yellow
    }
    return { wxColour(90, 90, 90), *wxWHITE, "Inactive" };  // keeps every compiler quiet
}

wxString IMEXModesCtrl::role_hint(ImexRole role) {
    switch (role) {
    case ImexRole::Primary:
        return _L("Drives every sliced path. Always tool 0 — use Tool 0 Position above to choose "
                  "which corner of the bed it occupies. It cannot be moved from the grid.");
    case ImexRole::Copy:
        return _L("Follows the Primary at the firmware level, printing the same paths offset into "
                  "its own zone.");
    case ImexRole::Mirror:
        return _L("Follows the Primary at the firmware level, printing the same paths reflected "
                  "across the boundary between the two zones.");
    case ImexRole::Span:
        return _L("Marks a tool on the Primary's gantry as its multicolor partner. Required to "
                  "allow multi-color printing in paired-gantry modes.");
    }
    return wxEmptyString;
}

bool IMEXModesCtrl::role_offered(ImexRole role) const {
    // Span declares a within-gantry multicolor partner of the Primary, which only means
    // something when there is a second gantry running in parallel with it.
    if (role == ImexRole::Span)
        return m_n_rows >= 2;
    return true;
}

bool IMEXModesCtrl::role_allowed_on_tile(ImexRole role, int tool_idx,
                                         bool other_primary, int primary_gantry) const {
    if (!role_offered(role))
        return false;
    // Exactly one tile drives the sliced paths.
    if (role == ImexRole::Primary)
        return !other_primary;
    // Span only on a tile sharing the Primary's gantry — that is what the marker declares.
    if (role == ImexRole::Span)
        return primary_gantry >= 0 && (tool_idx / m_n_cols) == primary_gantry;
    return true;
}

std::optional<ImexRole> IMEXModesCtrl::next_tile_role(std::optional<ImexRole> current,
                                                      const std::function<bool(ImexRole)>& allowed) {
    const int n = (int)(sizeof(kImexRoleTable) / sizeof(kImexRoleTable[0]));
    int start = 0;  // no current role (Inactive) → start at the first role
    if (current) {
        for (int i = 0; i < n; ++i)
            if (kImexRoleTable[i].role == *current) { start = i + 1; break; }
    }
    for (int i = start; i < n; ++i)
        if (allowed(kImexRoleTable[i].role))
            return kImexRoleTable[i].role;
    return std::nullopt;  // walked off the end → back to Inactive
}

void IMEXModesCtrl::apply_btn(wxButton* btn, int tool_idx, std::optional<ImexRole> role) {
    // Always label as T{n} — the button color already encodes the role.
    const RoleStyle style = role_style(role);
    btn->SetLabel(wxString::Format("T%d", tool_idx));
    btn->SetBackgroundColour(style.bg);
    btn->SetForegroundColour(style.fg);
    btn->Refresh();
}

IMEXModesCtrl::RowSnapshot IMEXModesCtrl::snapshot_row(const Row& r) const {
    RowSnapshot s;
    // UTF-8 throughout -- this snapshot is compared against the preset's own values.
    s.name  = r.is_primary ? std::string(kImexPrimaryMode) : into_u8(r.name->GetValue());
    s.tools = active_tools_string(r);
    s.gcode = into_u8(r.gcode->GetValue());
    return s;
}

bool IMEXModesCtrl::row_differs_from_parent(int row_idx) const {
    if (!m_parent_lookup) return false;
    const DynamicPrintConfig* parent = m_parent_lookup();
    if (!parent) return false;
    auto* p_names  = parent->option<ConfigOptionStrings>("imex_mode_names");
    auto* p_tools  = parent->option<ConfigOptionStrings>("imex_mode_active_tools");
    auto* p_gcodes = parent->option<ConfigOptionStrings>("imex_mode_gcodes");
    if (!p_names || row_idx < 0 || row_idx >= (int)p_names->values.size()) return false;
    const Row& r = m_rows[row_idx];
    const RowSnapshot s = snapshot_row(r);
    if (!r.is_primary && s.name != p_names->values[row_idx]) return true;
    if (p_tools  && row_idx < (int)p_tools->values.size()  && s.tools  != p_tools->values[row_idx])  return true;
    if (p_gcodes && row_idx < (int)p_gcodes->values.size() && s.gcode != p_gcodes->values[row_idx]) return true;
    return false;
}

bool IMEXModesCtrl::row_has_parent_counterpart(int row_idx) const {
    if (!m_parent_lookup) return false;
    const DynamicPrintConfig* parent = m_parent_lookup();
    if (!parent) return false;
    auto* p_names = parent->option<ConfigOptionStrings>("imex_mode_names");
    return p_names && row_idx >= 0 && row_idx < (int)p_names->values.size();
}

void IMEXModesCtrl::add_row(const std::string& name,
                            const std::string& active_tools,
                            const std::string& gcode,
                            bool is_primary)
{
    Row r;
    r.is_primary = is_primary;
    r.panel = new wxPanel(this, wxID_ANY);
    r.panel->SetBackgroundColour(GetBackgroundColour());
    auto* sizer = new wxBoxSizer(wxHORIZONTAL);

    if (is_primary) {
        r.orig_name = kImexPrimaryMode;
        r.name = nullptr;
        auto* lbl = new wxStaticText(r.panel, wxID_ANY, _L("Primary"),
                                     wxDefaultPosition, FromDIP(wxSize(130, -1)));
        wxFont f = lbl->GetFont();
        f.SetWeight(wxFONTWEIGHT_BOLD);
        lbl->SetFont(f);
        sizer->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    } else {
        // A nameless mode cannot be selected on a plate and used to be dropped on
        // save together with its tool roles and G-code, so a name is pre-filled
        // here and restored below if the field is left empty. That keeps the
        // "+ Add Mode → assign tools → never typed a name" path from losing work
        // without ever showing an error.
        const std::string nm = name.empty() ? unique_mode_name({}) : name;
        // The name as first shown, i.e. the one the plate's mode selector offered while this
        // row looked like this. remove_row() reports it alongside the current name, because
        // renaming is not routed through on_mode_removed and the two then diverge.
        r.orig_name = nm;
        r.name = new wxTextCtrl(r.panel, wxID_ANY, from_u8(nm), wxDefaultPosition, FromDIP(wxSize(130, -1)));
        r.name->SetHint(_L("Mode name (required)"));
        r.name->SetToolTip(_L("Name of this parallel mode, as it appears in the plate's IDEX/IQEX mode "
                              "selector. Stored in the project by name, so renaming a mode that "
                              "plates already use makes them fall back to Primary. Cannot be empty "
                              "— a blank name is replaced with a generated one."));
        r.name->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { notify(); });
        // Restore a name rather than let the row reach get_mode_data() unnamed.
        // Row is located by panel pointer (stable across add/remove) so a
        // kill-focus delivered while the rows are being torn down is a no-op.
        r.name->Bind(wxEVT_KILL_FOCUS, [this, panel = r.panel](wxFocusEvent& e) {
            e.Skip();
            if (m_clearing_rows) return; // focus-out emitted while the rows are being deleted
            for (auto& row_ref : m_rows) {
                if (row_ref.panel != panel) continue;
                if (!row_ref.name || !row_ref.name->GetValue().empty()) return;
                // ChangeValue(), not SetValue(): no nested wxEVT_TEXT.
                row_ref.name->ChangeValue(from_u8(unique_mode_name({})));
                notify();
                return;
            }
        });
    }

    auto* grid_panel = new wxPanel(r.panel, wxID_ANY);
    grid_panel->SetBackgroundColour(GetBackgroundColour());
    auto* grid_sizer = new wxGridSizer(m_n_rows, m_n_cols, FromDIP(2), FromDIP(2));

    auto tool_roles = roles_for_mode(active_tools);
    if (tool_roles.empty())
        tool_roles[0] = ImexRole::Primary; // default T0 → Primary when no assignment is stored
    r.all_tool_roles = tool_roles; // preserve all assignments, including off-screen tools
    wxPanel* this_panel = r.panel;

    // Buttons rendered top=rear (high Y), bottom=front (low Y).
    // Layout remapping: m_layout encodes which corner T0 is at physically.
    //   flip_x (layout 1,3): col 0 is on the right (max-X) instead of left
    //   flip_y (layout 2,3): row 0 is at the rear (max-Y) instead of front
    // Display iterates: row from n_rows-1 down to 0 (rear→front = top→bottom).
    // raw_row = flip_y ? (n_rows-1-row) : row
    // raw_col = flip_x ? (n_cols-1-col) : col
    //
    // row_start: anchor displayed rows to the gantry row containing the Primary
    // assignment, so reducing gantry count keeps the meaningful row visible.
    int primary_gantry_row = 0;
    for (const auto& [idx, role] : tool_roles) {
        if (role == ImexRole::Primary) { primary_gantry_row = idx / m_n_cols; break; }
    }
    int row_start = primary_gantry_row; // display rows [row_start .. row_start+m_n_rows-1]

    bool flip_x = (m_layout == 1 || m_layout == 3);
    bool flip_y = (m_layout == 2 || m_layout == 3);
    for (int row = m_n_rows - 1; row >= 0; --row) {
        for (int col = 0; col < m_n_cols; ++col) {
            int raw_row = flip_y ? (m_n_rows - 1 - row) : row;
            raw_row += row_start; // anchor to Primary's gantry row
            int raw_col = flip_x ? (m_n_cols - 1 - col) : col;
            int tool_idx = raw_row * m_n_cols + raw_col;
            std::optional<ImexRole> role;  // nullopt == Inactive
            auto it = tool_roles.find(tool_idx);
            if (it != tool_roles.end()) role = it->second;

            auto* btn = new wxButton(grid_panel, wxID_ANY, wxEmptyString,
                                     wxDefaultPosition, FromDIP(wxSize(36, 26)), wxBU_EXACTFIT);
            apply_btn(btn, tool_idx, role);

            int btn_pos = (int)r.btns.size();
            r.btns.push_back(btn);
            r.btn_roles.push_back(role);
            r.btn_tool_idx.push_back(tool_idx);

            // Two tiles are read-only.
            //
            // The whole reserved `primary` row: it is the IMEX-off mode, so cycling roles there
            // would be a no-op and confusing.
            //
            // And, on any row, the tile that currently HOLDS Primary. Primary is tool 0 and moves
            // only via Tool 0 Position; letting a click demote it would leave the mode with no
            // Primary at all, which parses to -1 and degrades the plate to an ordinary single-tool
            // print with no zones — authored silently, in one click, with nothing in the editor
            // showing what is wrong. The other tiles already never OFFER Primary while one is held
            // (role_allowed_on_tile), so this closes the only remaining route to that state.
            // A mode that reaches us WITHOUT a Primary (hand-edited preset, foreign 3MF) still has
            // every tile live, so it can be repaired by promoting one.
            if (is_primary || role == ImexRole::Primary) {
                btn->Disable();
            } else {
                btn->Bind(wxEVT_BUTTON, [this, this_panel, btn_pos](wxCommandEvent&) {
                    // Find the row by panel pointer (stable across add/remove)
                    for (auto& row_ref : m_rows) {
                        if (row_ref.panel != this_panel) continue;
                        std::optional<ImexRole>& tile = row_ref.btn_roles[btn_pos];
                        const int tidx = row_ref.btn_tool_idx[btn_pos];

                        // Check if another button already holds the Primary role,
                        // and locate the primary's gantry row for Span eligibility.
                        bool other_primary = false;
                        int  primary_gantry = -1;
                        for (int j = 0; j < (int)row_ref.btn_roles.size(); ++j) {
                            if (row_ref.btn_roles[j] == ImexRole::Primary) {
                                if (j != btn_pos) other_primary = true;
                                primary_gantry = row_ref.btn_tool_idx[j] / m_n_cols;
                            }
                        }

                        // Cycle Inactive → Primary → Copy → Mirror → Span → Inactive,
                        // skipping whatever this tile may not take right now.
                        tile = next_tile_role(tile, [&](ImexRole cand) {
                            return role_allowed_on_tile(cand, tidx, other_primary, primary_gantry);
                        });

                        // Keep all_tool_roles in sync so off-screen tools are preserved
                        if (tile)
                            row_ref.all_tool_roles[tidx] = *tile;
                        else
                            row_ref.all_tool_roles.erase(tidx);

                        apply_btn(row_ref.btns[btn_pos], row_ref.btn_tool_idx[btn_pos], tile);
                        notify();
                        break;
                    }
                });
            }

            grid_sizer->Add(btn, 0);
        }
    }
    grid_panel->SetSizerAndFit(grid_sizer);

    // from_u8(): `gcode` arrives from the preset as UTF-8. Handing the raw std::string to
    // wxString would decode it through the current locale's encoding instead.
    r.gcode = new wxTextCtrl(r.panel, wxID_ANY, from_u8(gcode),
                             wxDefaultPosition, FromDIP(wxSize(220, 54)), wxTE_MULTILINE);
    // Tooltip only, no SetHint(): wxTextEntry has no native placeholder for a
    // multiline control, so wxWidgets emulates one by writing the hint into the
    // control as grey text — indistinguishable from real G-code in this box.
    r.gcode->SetToolTip(_L("G-code emitted once at the start of a print that uses this mode, before "
                           "the machine start G-code. This is where the printer is put into the "
                           "matching firmware mode — for example a Klipper SET_PRINT_MODE call or a "
                           "RepRapFirmware M567 — since the slicer only emits the Primary tool's "
                           "paths and the firmware drives the Copy / Mirror tools. Placeholders are "
                           "supported; use the edit button to browse them. Leave empty if the mode "
                           "needs no firmware setup."));
    r.gcode->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { notify(); });

    if (!is_primary) {
        sizer->Add(r.name, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    }
    sizer->Add(grid_panel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    sizer->Add(r.gcode,   1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));

    // Edit + remove (for non-primary) — stacked vertically.
    auto* btn_col = new wxBoxSizer(wxVERTICAL);
    wxTextCtrl* gcode_ctrl = r.gcode;
    auto* ph_btn = new ScalableButton(r.panel, wxID_ANY, "edit", wxEmptyString,
                                      wxDefaultSize, wxDefaultPosition,
                                      wxBU_EXACTFIT | wxNO_BORDER, 16);
    ph_btn->SetToolTip(_L("Edit G-code / browse placeholders"));
    ph_btn->Bind(wxEVT_BUTTON, [this, gcode_ctrl](wxCommandEvent&) {
        // EditGCodeDialog takes and returns UTF-8 (get_edited_gcode() is a ToUTF8()).
        EditGCodeDialog dlg(this, "imex_mode_gcode", into_u8(gcode_ctrl->GetValue()));
        if (dlg.ShowModal() == wxID_OK)
            gcode_ctrl->SetValue(from_u8(dlg.get_edited_gcode()));
    });
    btn_col->Add(ph_btn, 0, wxBOTTOM, FromDIP(2));

    if (!is_primary) {
        auto* rm = new ScalableButton(r.panel, wxID_ANY, "imex_remove", wxEmptyString,
                                      wxDefaultSize, wxDefaultPosition,
                                      wxBU_EXACTFIT | wxNO_BORDER, 16);
        rm->SetToolTip(_L("Remove mode"));
        rm->Bind(wxEVT_BUTTON, [this, this_panel](wxCommandEvent&) {
            const std::vector<std::string> removed = remove_row(this_panel);
            // Copied before notify(): on_change ends in on_value_change, which can send
            // the tab through update() and hence load_from_config(), and that rebuilds
            // every row. Reading the member afterwards would be reading through a
            // handler whose owner has just been rebuilt underneath it.
            auto removed_cb = on_mode_removed;
            notify();
            if (removed_cb && !removed.empty())
                removed_cb(removed);
        });
        btn_col->Add(rm, 0);
    }
    sizer->Add(btn_col, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));

    // Per-row reset gets its own column on the right so the icon reads as
    // distinct from the edit/remove column. Reset only renders when this row
    // has a counterpart in the saved preset (user-added rows beyond the saved
    // mode count get no reset — the X button covers "remove user-added row").
    auto* reset_col = new wxBoxSizer(wxVERTICAL);
    wxPanel* this_panel_for_reset = r.panel;
    if (row_has_parent_counterpart(static_cast<int>(m_rows.size()))) {
        r.reset_btn = new ScalableButton(r.panel, wxID_ANY, "dot", wxEmptyString,
                                         wxDefaultSize, wxDefaultPosition,
                                         wxBU_EXACTFIT | wxNO_BORDER, 16);
        r.reset_btn->SetToolTip(_L("Discard in-session edits to this mode (snap back to saved value)"));
        r.reset_btn->Bind(wxEVT_BUTTON, [this, this_panel_for_reset](wxCommandEvent&) {
            reset_row_to_parent(this_panel_for_reset);
        });
        reset_col->Add(r.reset_btn, 0, wxALIGN_CENTER_VERTICAL);
    }
    sizer->Add(reset_col, 0, wxALIGN_CENTER_VERTICAL);
    r.panel->SetSizerAndFit(sizer);

    m_rows_sizer->Add(r.panel, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
    m_rows.push_back(std::move(r));
    Layout();
}

std::vector<std::string> IMEXModesCtrl::remove_row(wxPanel* panel) {
    for (size_t i = 0; i < m_rows.size(); ++i) {
        if (m_rows[i].panel != panel) continue;
        if (m_rows[i].is_primary) return {}; // primary row is non-deletable

        // Both names a plate can be holding for this row: the one the row was built with and
        // the one in the field now. They diverge as soon as the row is renamed, because a
        // rename does not rebuild the row and is deliberately not reported as a removal (the
        // name field notifies on every keystroke). Reporting only the current name is what
        // let "rename, then delete" leave the plate on a mode that no longer exists.
        std::vector<std::string> removed_names;
        const std::string current = m_rows[i].name ? into_u8(m_rows[i].name->GetValue())
                                                   : std::string();
        for (const std::string& n : {m_rows[i].orig_name, current})
            if (!n.empty() && std::find(removed_names.begin(), removed_names.end(), n) == removed_names.end())
                removed_names.push_back(n);

        m_rows_sizer->Detach(panel);
        m_rows.erase(m_rows.begin() + i);

        // A name a surviving row still carries is not stale: the user can rename row A to
        // row B's old name and then delete B. Plates holding it now resolve to A, so drop it
        // rather than reset them.
        auto still_in_use = [this](const std::string& n) {
            return std::any_of(m_rows.begin(), m_rows.end(), [&n](const Row& r) {
                return r.is_primary ? n == kImexPrimaryMode
                                    : r.name && into_u8(r.name->GetValue()) == n;
            });
        };
        removed_names.erase(std::remove_if(removed_names.begin(), removed_names.end(), still_in_use),
                            removed_names.end());

        Layout();
        // Defer widget destruction so any in-flight GTK events for
        // panel's children (including the × button we're inside) finish
        // processing before the wxEvtHandlers are freed.
        wxTheApp->CallAfter([panel]() { panel->Destroy(); });
        return removed_names;
    }
    return {};
}

void IMEXModesCtrl::clear_rows() {
    // Destroying a focused wxTextCtrl emits wxEVT_KILL_FOCUS, and the Name field's
    // handler would then read m_rows while it is half-destroyed. Flag the teardown
    // so that handler stays out of the way.
    m_clearing_rows = true;
    for (auto& r : m_rows) { m_rows_sizer->Detach(r.panel); r.panel->Destroy(); }
    m_rows.clear();
    m_clearing_rows = false;
}

std::string IMEXModesCtrl::active_tools_string(const Row& r) const {
    // Serialize from all_tool_roles (not just visible buttons) so assignments
    // for off-screen tools are preserved across grid size changes. Inactive tools are
    // simply absent from the map, so there is nothing to filter out here.
    //
    // The letters come from imex_role_letter(), the same table parse_imex_active_tools()
    // reads back — this used to be a second, independent int → letter switch, which is
    // exactly where the editor and the slicer could come to disagree about a mode string.
    std::string s;
    for (const auto& [idx, role] : r.all_tool_roles) {
        if (!s.empty()) s += ',';
        s += std::to_string(idx) + ':' + imex_role_letter(role);
    }
    return s;
}

void IMEXModesCtrl::notify() {
    refresh_reset_buttons();
    if (on_change) on_change();
}

void IMEXModesCtrl::refresh_reset_buttons() {
    for (size_t i = 0; i < m_rows.size(); ++i) {
        Row& r = m_rows[i];
        if (!r.reset_btn || !r.reset_btn->IsEnabled()) continue;
        const bool dirty = row_differs_from_parent(static_cast<int>(i));
        if (dirty == r.reset_dirty_cached) continue;
        r.reset_dirty_cached = dirty;
        r.reset_btn->SetBitmap_(dirty ? "undo" : "dot");
    }
}

void IMEXModesCtrl::reset_row_to_parent(wxPanel* panel) {
    if (!m_parent_lookup) return;
    const DynamicPrintConfig* parent = m_parent_lookup();
    if (!parent) return;
    auto* p_names  = parent->option<ConfigOptionStrings>("imex_mode_names");
    auto* p_tools  = parent->option<ConfigOptionStrings>("imex_mode_active_tools");
    auto* p_gcodes = parent->option<ConfigOptionStrings>("imex_mode_gcodes");
    for (size_t i = 0; i < m_rows.size(); ++i) {
        Row& r = m_rows[i];
        if (r.panel != panel) continue;
        if (!p_names || i >= p_names->values.size()) return;
        if (!r.is_primary && r.name)
            r.name->ChangeValue(from_u8(p_names->values[i]));
        if (p_gcodes && i < p_gcodes->values.size())
            r.gcode->ChangeValue(from_u8(p_gcodes->values[i]));
        if (p_tools && i < p_tools->values.size()) {
            // Reapply tool roles from the parent's serialized form. all_tool_roles
            // is the source of truth for round-tripping; rebuild it then re-paint
            // the visible buttons.
            r.all_tool_roles = roles_for_mode(p_tools->values[i]);
            for (size_t j = 0; j < r.btns.size(); ++j) {
                int tidx = r.btn_tool_idx[j];
                auto it = r.all_tool_roles.find(tidx);
                std::optional<ImexRole> role;
                if (it != r.all_tool_roles.end()) role = it->second;
                r.btn_roles[j] = role;
                apply_btn(r.btns[j], tidx, role);
            }
        }
        notify();
        return;
    }
}

}} // namespace Slic3r::GUI
