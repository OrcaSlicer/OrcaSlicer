#include "FilamentMapRowsPanel.hpp"

#include <boost/log/trivial.hpp>
#include "FilamentMapPanel.hpp" // wxEVT_INVALID_MANUAL_MAP
#include "DragDropPanel.hpp"    // Hex2Color
#include "AmsMappingPopup.hpp"  // MaterialSyncItem, MappingItem, MappingContainer
#include "ActivePrinterSession.hpp"
#include "FilamentInventoryStore.hpp"
#include "GUI_App.hpp"
#include "Widgets/StateColor.hpp"

#include <wx/wrapsizer.h>

#include <algorithm>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace Slic3r { namespace GUI {

static const wxColour TextNormalGreyColor = wxColour("#6B6B6B");

// Orca: a placeholder target option (bootstrap-mode bare tool pick, or a tool with nothing
// physically recorded -- see FilamentMapRowsPanel::BuildTargetOptions) has no real colour yet;
// alpha 0 is MaterialItem/MappingItem's own convention for "draw the transparent placeholder
// swatch instead of a solid colour" (see doRender's `acolor.Alpha() == 0` branches).
// Opaque, deliberately: MaterialSyncItem::doRender treats an alpha-0 colour as "transparent
// filament" and paints the checkerboard texture with no header band at all -- on a fresh
// install (empty inventory, every picker option in bootstrap mode) the whole picker rendered
// as blank checkerboard tiles, reported as broken on Windows by two testers. A flat neutral
// grey reads as "no colour recorded yet", which is what this placeholder means.
static const wxColour PlaceholderColour = wxColour(0xCE, 0xCE, 0xCE);

// Orca: the tile's width the rest of this file resizes every MaterialSyncItem to (see WidenTile
// below), replacing MaterialItem::messure_size's hardcoded 65dip. Widened so the corner badge
// (BadgedSyncItem, below) has somewhere to sit without overlapping the centered "<idx> <type>"
// label MaterialSyncItem::render draws across the tile's own GetSize().x.
static constexpr int TILE_WIDTH_DIP = 82;

// Orca: messure_size() (AmsMappingPopup.cpp, read-only) hardcodes every MaterialItem/
// MaterialSyncItem to FromDIP(65) wide and re-asserts that size on every set_ams_info/
// reset_ams_info/set_nozzle_info call (each calls messure_size() again). Since MaterialSyncItem
// ignores the change until its next paint, this just needs to run once after construction and
// once after each such call -- see the ctor's per-row loop and ApplySelectionToTile below -- to
// win the last word before the next Refresh(). Height is left exactly as messure_size computed
// it (this panel never sets a nozzle string, so it's always FromDIP(50)).
static void WidenTile(MaterialSyncItem *tile)
{
    const wxSize target(tile->FromDIP(TILE_WIDTH_DIP), tile->GetSize().GetHeight());
    tile->SetMinSize(target);
    tile->SetMaxSize(target);
    tile->SetSize(target);
}

// Orca: small corner marker for a row whose current pick came from the
// auto-matcher rather than a hand pick (Row::auto_matched). Subclasses MaterialSyncItem locally
// (AmsMappingPopup.hpp/.cpp stay read-only, so this can't add a new virtual there) and overrides
// the inherited doRender hook to paint directly into the tile's own top-right corner after the
// base class's own painting. An in-place overlay rather than a sibling window on purpose: a child
// window has to paint some colour under the un-triangled part of its own rect, which shows as a
// stray square over the tile's coloured header, while painting straight onto the tile's DC only
// touches the pixels the triangle covers.
class BadgedSyncItem : public MaterialSyncItem
{
public:
    using MaterialSyncItem::MaterialSyncItem;

    // True only while this row's CURRENT selection is both present and came from the auto-matcher
    // (ApplySelectionToTile decides that and calls this after set_ams_info/reset_ams_info).
    void set_auto_matched(bool matched)
    {
        if (m_auto_matched == matched) return;
        m_auto_matched = matched;
        Refresh();
    }

protected:
    void doRender(wxDC &dc) override
    {
        // Field diagnostic (warning: Windows filters info): proves whether this tile's paint
        // actually EXECUTES. First-wrap-row tiles show blank on Windows while their geometry is
        // correct; this line splits "never painted" from "painted then overdrawn".
        BOOST_LOG_TRIVIAL(warning) << "tile doRender: idx=" << get_material_index_str()
                                   << " rect=" << GetRect().x << "," << GetRect().y
                                   << " size=" << GetSize().x << "x" << GetSize().y;
        // Runs through MaterialSyncItem::render's existing MSW double-buffer path unchanged --
        // that path calls doRender(dc2) polymorphically on the memory-DC-backed wxGCDC (or the
        // live dc directly off-MSW), so this override's extra painting is captured by the same
        // blit as the base class's.
        MaterialSyncItem::doRender(dc);
        if (!m_auto_matched) return;

        static constexpr int BADGE_SIZE_DIP = 16;
        const wxSize size  = GetSize();
        const int    badge = FromDIP(BADGE_SIZE_DIP);
        const int    x0    = size.x - badge;

        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(StateColor::darkModeColorFor(wxColour("#009688")))); // Orca accent teal
        wxPoint triangle[3] = {wxPoint(x0, 0), wxPoint(size.x, 0), wxPoint(size.x, badge)};
        dc.DrawPolygon(3, triangle);

        wxFont font = GetFont();
        font.SetPointSize(std::max(6, font.GetPointSize() - 3));
        font.SetWeight(wxFONTWEIGHT_BOLD);
        dc.SetFont(font);
        dc.SetTextForeground(StateColor::darkModeColorFor(*wxWHITE));
        const wxString label     = "A";
        const wxSize   text_size = dc.GetTextExtent(label);
        // Centered within the triangle's own visual mass, which sits toward the badge's
        // top-right third, not its geometric center.
        dc.DrawText(label, size.x - text_size.GetWidth() - FromDIP(2), FromDIP(1));
    }

private:
    bool m_auto_matched{false};
};

// Orca: lightweight, neutral picker popup used only by FilamentMapRowsPanel's tiles. Unlike
// AmsMapingPopup (AmsMappingPopup.hpp), this consumes FilamentMapRowsPanel::TargetOption
// directly -- already neutral (id/tool/label/colour), no MachineObject/device coupling -- so it
// composes MappingContainer + MappingItem directly, with no need for AmsMapingPopup's
// device-facing update() seam. Positioning mirrors
// SyncAmsInfoDialog's tile click handler (item->ClientToScreen(0,0) + tile height), the house
// pattern for "popup under a clicked tile" (SyncAmsInfoDialog.cpp:2700-2722).
class FilamentMapPickerPopup : public PopupWindow
{
public:
    // Orca: MappingContainer's own ctor rule (AmsMappingPopup.cpp, read-only) is binary -- a
    // slots_num of exactly 1 gets its 1-slot art/size (74dip wide), anything else gets its 4-slot
    // art/size (230dip wide) regardless of the actual count, so containers here are NOT uniform
    // width once a tool has more than one option. MAX_ROW_WIDTH_DIP caps how wide a row of
    // containers can grow (in Rebuild's greedy pack below) before wrapping to a new row: five
    // 1-slot containers' worth, so a plate with many single-option tools still wraps at roughly
    // the point the old fixed-5-column grid did.
    static constexpr int MAX_ROW_WIDTH_DIP = 5 * 74;

    explicit FilamentMapPickerPopup(wxWindow *parent) : PopupWindow(parent, wxBORDER_NONE)
    {
        SetBackgroundColour(*wxWHITE);
        // Orca: containers are non-uniform width (see MAX_ROW_WIDTH_DIP), which rules out a
        // wxGridSizer's fixed-cell model -- every cell would have to be as wide as the widest
        // (230dip) container even on rows made entirely of 74dip ones. Rebuild() below instead
        // builds one horizontal wxBoxSizer per row by hand (greedy-packed up to
        // MAX_ROW_WIDTH_DIP) and stacks the rows in this outer vertical box. Deterministic, no
        // sizer negotiation to fight.
        m_sizer = new wxBoxSizer(wxVERTICAL);
        SetSizer(m_sizer);
        wxGetApp().UpdateDarkUIWin(this);
    }

    // Rebuilds the popup for one row: one MappingContainer per tool that has at least one target
    // option (options grouped by TargetOption::tool, ascending), sized for that tool's actual
    // option count (see MAX_ROW_WIDTH_DIP) and filled with a MappingItem per option. current_index's
    // item starts checked; on_pick(index) fires (and the popup dismisses itself) on click -- clicks
    // are bound directly here rather than through MappingItem::send_event's pipe-delimited protocol,
    // since this popup has no send_win consumer to satisfy.
    //
    // bootstrap_mode: true when the whole inventory has nothing recorded (see
    // FilamentMapRowsPanel::m_bootstrap_mode) OR the active printer session isn't live -- in either
    // case a bare tool pick (option.id <= 0) is allowed and every such option stays pickable
    // (offline, "no filament loaded" is unknowable rather than known-empty). Outside those cases an
    // id<=0 option is a single still-empty tool (BuildTargetOptions' per-tool fallback): shown so the
    // tool head stays visible/identifiable, but grayed via IsEnabled()/doRender and its click is
    // swallowed instead of invoking on_pick. This only affects which chips are clickable here; it
    // does not touch FilamentMapRowsPanel's own bootstrap-mode-gated merge/warning logic elsewhere.
    void Rebuild(const std::vector<FilamentMapRowsPanel::TargetOption> &options, int current_index,
                 bool bootstrap_mode, const std::string &row_type, std::function<void(int)> on_pick)
    {
        m_on_pick = std::move(on_pick);
        m_sizer->Clear(true);
        m_items.clear();

        std::map<int, std::vector<int>> by_tool; // tool -> option indices, ascending tool order
        for (int i = 0; i < (int) options.size(); ++i)
            by_tool[options[i].tool].push_back(i);

        const int border      = FromDIP(4); // matches the old wxALL FromDIP(4) per container
        const int max_row_w   = FromDIP(MAX_ROW_WIDTH_DIP);
        wxBoxSizer *row_sizer = nullptr; // current row being packed; flushed to m_sizer on wrap
        int row_w             = 0;       // current row's accumulated width, including borders
        int container_h       = 0;       // MappingContainer's own height is constant (82dip)
                                          // regardless of slots_num, so one measurement covers
                                          // every row -- see MAX_ROW_WIDTH_DIP's doc comment.
        int total_w           = 0;
        int row_count         = 0;

        auto flush_row = [&]() {
            if (!row_sizer) return;
            m_sizer->Add(row_sizer, 0);
            total_w = std::max(total_w, row_w);
            ++row_count;
            row_sizer = nullptr;
            row_w     = 0;
        };

        for (const auto &kv : by_tool) {
            wxString label     = wxString::Format(_L("Tool %d"), kv.first + 1);
            // Orca: pass the tool's real option count (loaded + swap slots, etc.) instead of a
            // hardcoded 1, so a multi-option tool gets MappingContainer's wider 4-slot art/size
            // instead of being squeezed into the 1-slot art. See the class comment above for the
            // binary 1-vs-4 rule this relies on.
            auto    *container = new MappingContainer(this, label, (int) kv.second.size());
            auto    *item_sizer = new wxBoxSizer(wxHORIZONTAL);
            item_sizer->Add(0, 0, 0, wxLEFT, FromDIP(6));

            for (int idx : kv.second) {
                const auto &opt = options[idx];

                auto *item = new MappingItem(container);
                item->SetSize(wxSize(FromDIP(48), FromDIP(60)));
                item->SetMinSize(wxSize(FromDIP(48), FromDIP(60)));
                item->SetMaxSize(wxSize(FromDIP(48), FromDIP(60)));
                item->set_tray_index(wxString::Format("T%d", opt.tool + 1));

                TrayData data;
                data.type    = opt.id > 0 ? TrayType::NORMAL : TrayType::EMPTY;
                data.id      = opt.id;
                data.ams_id  = opt.tool;
                data.slot_id = idx;
                data.colour  = opt.colour;

                // The tray-index text above already identifies the tool; the option's full
                // label (preset name + loaded/swap/empty suffix) goes in the tooltip instead of
                // the tile's own name field, which truncates hard past 5 characters
                // (MappingItem::render).
                bool disabled = !bootstrap_mode && opt.id <= 0;
                // Orca: hard material-family gate (field request) -- any material could be
                // mapped onto any tool here while the sync dialog and the auto-matcher both
                // restrict by family. Uses the same libslic3r type_compatible as the
                // auto-matcher so "family" means one thing everywhere; options with an
                // unknown type on either side stay pickable (nothing to compare).
                bool wrong_type = !disabled && opt.id > 0 && !row_type.empty() && !opt.type.empty() &&
                                  !Slic3r::type_compatible(row_type, opt.type);
                item->set_data(opt.label, opt.colour, wxString(), /*remain_dect=*/false, data,
                               /*unmatch=*/wrong_type, opt.label);
                item->set_checked(idx == current_index);
                item->Enable(!disabled);
                if (disabled)
                    item->SetToolTip(_L("No filament is loaded on this tool -- load one, or sync from the printer, before selecting it."));
                else if (wrong_type)
                    // Stays ENABLED (a disabled control never shows its tooltip on Windows);
                    // the click is swallowed below and the unmatch paint greys the tile.
                    item->SetToolTip(wxString::Format(
                        _L("Material mismatch: this row prints %s but the tool holds %s. Only matching material families can be mapped."),
                        from_u8(row_type), from_u8(opt.type)));

                item->Bind(wxEVT_LEFT_DOWN, [this, idx, disabled, wrong_type](wxMouseEvent &) {
                    if (disabled || wrong_type) return; // swallow the click -- empty tool or material-family mismatch
                    if (m_on_pick) m_on_pick(idx);
                    Dismiss();
                });

                item_sizer->Add(item, 0, wxTOP, FromDIP(1));
                item_sizer->Add(0, 0, 0, wxRIGHT, FromDIP(6));
                m_items.push_back(item);
            }

            // SetSizerAndFit (not SetSizer+Layout): the container's own best size must be known
            // below to size the popup, and a container never Fit only measures 0 until asked.
            container->SetSizerAndFit(item_sizer);

            // Orca: greedy row pack -- flush the current row (start a new one) if this container
            // wouldn't fit within MAX_ROW_WIDTH_DIP, then always add it to whatever row is now
            // current. A single container wider than the cap on its own (can't happen today --
            // 230dip < 5*74dip -- but kept for correctness if the art/columns ever change) still
            // gets its own row rather than being dropped.
            const wxSize cs           = container->GetBestSize();
            const int    container_w = cs.GetWidth() + 2 * border;
            container_h              = std::max(container_h, cs.GetHeight() + 2 * border);
            if (row_sizer && row_w + container_w > max_row_w) flush_row();
            if (!row_sizer) row_sizer = new wxBoxSizer(wxHORIZONTAL);
            row_sizer->Add(container, 0, wxALL, border);
            row_w += container_w;
        }
        flush_row();

        // Orca: compute the popup's client size explicitly from the rows just packed -- every
        // row's own width is already known (row_w tracked above, folded into total_w by
        // flush_row), and every row is the same height (container_h -- MappingContainer's height
        // is constant regardless of slots_num, see MAX_ROW_WIDTH_DIP's doc comment), so the total
        // is just rows * container_h. Sizing by hand rather than through Fit(), whose negotiation
        // clips a hand-packed row layout.
        if (row_count > 0) {
            const wxSize popup_size(total_w, row_count * container_h);
            SetSize(popup_size);
            SetMinSize(popup_size);
        }

        Layout();
    }

private:
    wxBoxSizer                 *m_sizer{nullptr};
    std::vector<MappingItem *> m_items;
    std::function<void(int)>   m_on_pick;
};

FilamentMapRowsPanel::FilamentMapRowsPanel(wxWindow                        *parent,
                                           const std::vector<std::string> &filament_colors,
                                           const std::vector<std::string> &filament_types,
                                           const std::vector<std::string> &filament_names,
                                           const std::vector<int>          &plate_filaments,
                                           const std::vector<int>          &proposal,
                                           const FilamentInventory          &inventory,
                                           const std::map<int, std::string> &slot_preset_names,
                                           size_t                            tool_count,
                                           bool                              auto_matched_proposal)
    : wxPanel(parent)
    , m_filament_count(filament_colors.size())
    , m_filament_types(filament_types)
    , m_tool_count(tool_count)
{
    SetName(wxT("FilamentMapRowsPanel"));
    SetBackgroundColour(*wxWHITE);
    m_base_map.assign(m_filament_count, 1);
    m_base_physical_map.assign(m_filament_count, 0);

    BuildTargetOptions(inventory, slot_preset_names);

    auto *top_sizer = new wxBoxSizer(wxVERTICAL);
    // Orca: tiles wrap horizontally instead of stacking one-per-row -- a plate with a handful of
    // filaments otherwise left most of the dialog's width empty (see AmsMappingPopup's
    // m_groups_sizer for the same wxWrapSizer pattern this mirrors). Footer stays below, added
    // to top_sizer separately once the whole grid is built.
    // Deliberately NOT wxWRAPSIZER_DEFAULT_FLAGS: that includes wxEXTEND_LAST_ON_EACH_LINE,
    // which stretches the last item of every wrapped row to fill the remaining line width.
    // AmsMappingPopup's mirrored usage gets away with that because it wraps container panels
    // (extra width just becomes padding inside the card); these tiles are the fixed-size
    // (WidenTile above, TILE_WIDTH_DIP x 50) leaf widgets themselves, so stretching one distorts
    // its paint directly -- the enlarged last-tile-per-row bug. Keep wxREMOVE_LEADING_SPACES so
    // a fresh row still doesn't start with a stray spacer.
    auto *tiles_sizer = new wxWrapSizer(wxHORIZONTAL, wxREMOVE_LEADING_SPACES);

    m_rows.reserve(plate_filaments.size());
    for (size_t row_i = 0; row_i < plate_filaments.size(); ++row_i) {
        int f = plate_filaments[row_i];
        if (f < 1 || f > (int) filament_colors.size()) continue;

        wxColour mcolour = PlaceholderColour;
        try {
            if (!filament_colors[f - 1].empty()) mcolour = Hex2Color(filament_colors[f - 1]);
        } catch (const std::exception &) {
            mcolour = PlaceholderColour;
        }

        wxString mname = from_u8(filament_types[f - 1]);

        // Orca: BadgedSyncItem (this file, above) paints its own auto-matched corner badge in
        // doRender, so the tile parents directly to `this` again -- no wrapper panel needed now
        // that the badge isn't a sibling window.
        auto *tile = new BadgedSyncItem(this, mcolour, mname);
        WidenTile(tile); // messure_size() (called by the ctor above) leaves it at 65dip; see WidenTile's comment
        tile->set_material_index_str(std::to_string(f));
        wxString full_name = filament_names.size() > (size_t) (f - 1) ? from_u8(filament_names[f - 1]) : wxString();
        tile->SetToolTip(full_name.IsEmpty() ? mname : (mname + " - " + full_name));

        // Orca: FromDIP(5) matches the tile spacing SyncAmsInfoDialog uses for the same
        // MaterialSyncItem widget (m_sizer_ams_mapping->Add(item, 0, wxALL, FromDIP(5))).
        tiles_sizer->Add(tile, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));

        size_t row_index = m_rows.size();
        m_rows.push_back({f, tile, -1, false});

        int prop = (row_i < proposal.size()) ? proposal[row_i] : 0;
        int sel  = wxNOT_FOUND;
        if (prop < 0) {
            // Orca: compute_physical_map_proposal sentinel-encodes a bare tool pick as
            // -(1-based tool) -- every row in bootstrap mode, or a confirmed pick of an
            // empty tool in normal mode. Decode to the matching id-less option's tool.
            int tool = -prop - 1;
            for (size_t oi = 0; oi < m_target_options.size(); ++oi)
                if (m_target_options[oi].id <= 0 && m_target_options[oi].tool == tool) { sel = (int) oi; break; }
        } else if (!m_bootstrap_mode && prop > 0) {
            for (size_t oi = 0; oi < m_target_options.size(); ++oi)
                if (m_target_options[oi].id == prop) { sel = (int) oi; break; }
        }
        m_rows[row_index].selected_index = sel;
        m_rows[row_index].auto_matched   = auto_matched_proposal && sel != wxNOT_FOUND;
        ApplySelectionToTile(row_index);

        tile->Bind(wxEVT_LEFT_DOWN, [this, row_index](wxMouseEvent &) { OnTileClicked(row_index); });
    }

    top_sizer->Add(tiles_sizer, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(4));

    top_sizer->AddSpacer(FromDIP(10));
    m_footer = new Label(this, Label::Body_13);
    m_footer->SetForegroundColour(TextNormalGreyColor);
    top_sizer->Add(m_footer, 0, wxALIGN_LEFT);

    // Orca: the profile-mismatch warning (SetFooterWarning) gets its own line below the stats
    // line, in the house warning color, instead of being appended inline to m_footer where it
    // was easy to miss next to the routine idle/swap counts.
    m_warning_label = new Label(this, Label::Body_13);
    m_warning_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#FF6F00")));
    m_warning_label->Hide();
    top_sizer->Add(m_warning_label, 0, wxALIGN_LEFT | wxTOP, FromDIP(2));

    SetSizer(top_sizer);
    Layout();
    Fit();

    // Orca: seed the warning flags and footer text, but do NOT fire wxEVT_INVALID_MANUAL_MAP
    // here -- the owning dialog queries AllRowsAssigned() itself right after construction to
    // set the initial OK button state (see FilamentMapPanel.hpp's event contract).
    UpdateFooter();

    GUI::wxGetApp().UpdateDarkUIWin(this);
}

void FilamentMapRowsPanel::BuildTargetOptions(const FilamentInventory &inventory, const std::map<int, std::string> &slot_preset_names)
{
    m_target_options.clear();

    // Orca: shares its "is there anything recorded" definition with FilamentMapDialog's record-
    // checkbox gating and libslic3r's compute_physical_map_proposal (same canonical
    // Slic3r::inventory_all_unset) so all three agree on when a plate is in bootstrap mode --
    // see that function's doc comment for why a loaded-only check would disagree here.
    m_bootstrap_mode = inventory_all_unset(inventory);

    if (m_bootstrap_mode) {
        // Orca: nothing physically recorded anywhere yet (a fresh machine profile) -- fall back
        // to picking a bare tool, same as the pre-physical-targets dialog, so the "Remember
        // these as the loaded filaments" offer on OK still has something to bootstrap from.
        for (size_t t = 0; t < m_tool_count; ++t) {
            TargetOption opt;
            opt.id     = 0;
            opt.tool   = (int) t;
            opt.label  = wxString::Format(_L("Tool %d"), (int) t + 1);
            opt.colour = PlaceholderColour;
            m_target_options.push_back(std::move(opt));
        }
        return;
    }

    for (size_t t = 0; t < m_tool_count && t < inventory.tools.size(); ++t) {
        const auto &slots = inventory.tools[t];
        for (size_t si = 0; si < slots.size(); ++si) {
            const PhysicalFilament &pf = slots[si];
            // pf.id <= 0 here (non-empty but no valid id) shouldn't happen through any normal
            // save path (FilamentInventoryEditor::on_ok and the v1/v2 migrations always mint an
            // id for a non-empty slot) -- only reachable via a hand-edited/corrupted app config.
            // m_bootstrap_mode is already false (inventory_all_unset only looks at content), so
            // such a slot is silently omitted rather than falling back to bootstrap; that's the
            // safer of two bad options for corrupt data (no silent "record over it" offer either).
            if (pf.empty() || pf.id <= 0) continue; // nothing physically recorded there

            TargetOption opt;
            opt.id   = pf.id;
            opt.tool = (int) t;
            opt.type = pf.type;

            // Orca: prefer the slot's resolved preset name (installed exact preset, or a
            // "Generic <type>" fallback -- see resolve_slot_preset) over the bare type, since it's
            // the more specific/useful thing to show once a preset is known; falls back to the
            // type-only label when the dialog couldn't resolve one. The tool identity goes at
            // the END, matching the picker popup's tray-index label which is per-tool already.
            wxString name;
            auto preset_it = slot_preset_names.find(pf.id);
            if (preset_it != slot_preset_names.end() && !preset_it->second.empty())
                name = from_u8(preset_it->second);
            else if (!pf.type.empty())
                name = from_u8(pf.type);
            wxString tool_part = wxString::Format(_L("Tool %d"), (int) t + 1);
            // FromUTF8 for the en dash: a raw literal goes through the ANSI conversion on
            // Windows and renders as mojibake in the option tooltip (same class of bug as the
            // stats separator).
            wxString label = name.IsEmpty() ? tool_part
                                             : wxString::Format("%s%s%s", name, wxString::FromUTF8(" – "), tool_part);
            label += (si == 0) ? _L(" (loaded)") : _L(" (swap)");
            opt.label = label;

            opt.colour = PlaceholderColour;
            if (!pf.color.empty()) {
                try {
                    opt.colour = Hex2Color(pf.color);
                } catch (const std::exception &) {
                    opt.colour = PlaceholderColour;
                }
            }

            m_target_options.push_back(std::move(opt));
        }
    }

    // Every tool head must be representable even when nothing is recorded for it -- a
    // tool whose slots are all empty gets a bare pick (id 0, like a
    // bootstrap option) so project filaments can still be routed -- and merged -- onto it.
    // GetPhysicalMaps writes 0 for such a pick; GetFilamentMaps carries the tool.
    for (size_t t = 0; t < m_tool_count; ++t) {
        bool has_option = std::any_of(m_target_options.begin(), m_target_options.end(),
                                      [t](const TargetOption &o) { return o.tool == (int) t; });
        if (has_option) continue;
        TargetOption opt;
        opt.id     = 0;
        opt.tool   = (int) t;
        opt.label  = wxString::Format(_L("Tool %d"), (int) t + 1) + _L(" (empty)");
        opt.colour = PlaceholderColour;
        m_target_options.push_back(std::move(opt));
    }
}

void FilamentMapRowsPanel::OnTileClicked(size_t row_index)
{
    if (row_index >= m_rows.size()) return;
    Row &row = m_rows[row_index];

    if (!m_picker_popup)
        m_picker_popup = new FilamentMapPickerPopup(this);
    if (m_picker_popup->IsShown())
        return;

    // Orca: offline (no live printer context), an unset tool is
    // "unknown", not "known empty" -- allow the same bare tool pick bootstrap mode does. See
    // FilamentMapPickerPopup::Rebuild's doc for why this is safe to fold into the same parameter.
    const bool allow_bare_pick = m_bootstrap_mode || !active_printer_session().live();
    // The row's project material type drives the picker's family gate; out-of-range ids
    // (defensive) or projects without type data yield an empty string = no restriction.
    std::string row_type;
    if (row.filament_id >= 1 && row.filament_id <= (int) m_filament_types.size())
        row_type = m_filament_types[row.filament_id - 1];
    m_picker_popup->Rebuild(m_target_options, row.selected_index, allow_bare_pick, row_type,
                            [this, row_index](int idx) { ApplyPick(row_index, idx); });

    wxPoint pos = row.tile->ClientToScreen(wxPoint(0, 0));
    pos.y += row.tile->GetRect().height;
    m_picker_popup->Move(pos);
    // The tile's tooltip is still the topmost surface while this click is being dispatched, and
    // Wayland drops the connection when a popup is mapped under one (GDK: "Tried to map a popup
    // with a non-top most parent"). Show it once the tooltip has been taken down.
    CallAfter([this] {
        if (m_picker_popup && !m_picker_popup->IsShown())
            m_picker_popup->Popup();
    });
}

void FilamentMapRowsPanel::ApplyPick(size_t row_index, int option_index)
{
    if (row_index >= m_rows.size()) return;
    m_rows[row_index].selected_index = option_index;
    // Orca: a hand pick always clears the auto-matched badge, even
    // if it happens to land back on the exact same target the auto-matcher had proposed.
    m_rows[row_index].auto_matched   = false;
    ApplySelectionToTile(row_index);
    UpdateFooter();

    wxCommandEvent evt(wxEVT_INVALID_MANUAL_MAP);
    evt.SetInt(AllRowsAssigned() ? 1 : 0);
    ProcessEvent(evt);
}

void FilamentMapRowsPanel::ApplySelectionToTile(size_t row_index)
{
    if (row_index >= m_rows.size()) return;
    Row &row = m_rows[row_index];

    if (row.selected_index >= 0 && (size_t) row.selected_index < m_target_options.size()) {
        const TargetOption &opt = m_target_options[row.selected_index];
        row.tile->set_ams_info(opt.colour, wxString::Format("T%d", opt.tool + 1));
    } else {
        // Orca: MaterialItem::render derives m_match from m_ams_name every repaint ("-" => not
        // matched), so reset_ams_info's dash is itself the "unassigned" glyph -- no separate
        // warning label needed.
        row.tile->reset_ams_info();
    }
    // set_ams_info/reset_ams_info above each call messure_size() internally (AmsMappingPopup.cpp,
    // read-only), which re-asserts the hardcoded 65dip width -- win the last word before Refresh.
    WidenTile(row.tile);
    row.tile->Refresh();

    // Orca: the badge marks a row whose CURRENT selection came from the
    // auto-matcher and hasn't been hand-picked since -- row.auto_matched is set by whichever
    // caller drove this selection (the ctor's row-seeding loop, or OnTileClicked's pick callback)
    // before calling this function; an unassigned row never shows it even if auto_matched is
    // stale-true from an earlier assignment this row no longer has. row.tile is always
    // constructed as a BadgedSyncItem (see the ctor's row-seeding loop); the static_cast is safe.
    static_cast<BadgedSyncItem *>(row.tile)->set_auto_matched(row.selected_index >= 0 && row.auto_matched);
}

bool FilamentMapRowsPanel::ResolveRow(const Row &row, int &tool, int &id) const
{
    int sel = row.selected_index;
    if (sel < 0 || (size_t) sel >= m_target_options.size())
        return false;
    tool = m_target_options[sel].tool;
    id   = m_target_options[sel].id;
    return true;
}

bool FilamentMapRowsPanel::RowTargetsEmptyTool(const Row &row) const
{
    if (m_bootstrap_mode) return false; // every option is a bare tool pick here -- expected, not a warning
    int sel = row.selected_index;
    if (sel < 0 || (size_t) sel >= m_target_options.size())
        return false;
    return m_target_options[sel].id <= 0;
}

wxString FilamentMapRowsPanel::EmptyToolWarningText() const
{
    std::set<int> empty_tools; // 0-based, de-duplicated, ascending
    for (const Row &row : m_rows)
        if (RowTargetsEmptyTool(row))
            empty_tools.insert(m_target_options[row.selected_index].tool);
    if (empty_tools.empty())
        return wxString();

    wxString tool_list;
    for (int t : empty_tools) {
        if (!tool_list.empty()) tool_list += ", ";
        tool_list += wxString::Format(_L("Tool %d"), t + 1);
    }
    return empty_tools.size() == 1 ? wxString::Format(_L("%s has no filament loaded"), tool_list)
                                    : wxString::Format(_L("%s have no filament loaded"), tool_list);
}

void FilamentMapRowsPanel::ComputeStats(int &idle, int &swaps, int &merged) const
{
    std::vector<int>                    assigned_counts(m_tool_count, 0);
    std::vector<std::unordered_set<int>> tool_ids(m_tool_count);
    std::unordered_map<int, int>        id_counts;
    int synthetic_id = 0; // see the id <= 0 case below

    for (const Row &row : m_rows) {
        int tool, id;
        if (!ResolveRow(row, tool, id)) continue;
        if (tool < 0 || tool >= (int) m_tool_count) continue;
        ++assigned_counts[tool];
        if (m_bootstrap_mode) continue; // no physical identity to merge on yet
        if (id <= 0) {
            // A bare tool pick onto an empty tool (mixed mode): id 0 is not a real physical slot,
            // so it must never join id_counts as a merge group -- two unrelated empty-tool picks
            // share no identity. It still occupies its tool though, and two distinct id<=0 picks
            // landing on the SAME tool are still two distinct picks there (not one shared target),
            // so it gets a per-row synthetic id for the swap count below instead of being skipped
            // outright.
            tool_ids[tool].insert(--synthetic_id);
            continue;
        }
        tool_ids[tool].insert(id);
        ++id_counts[id];
    }

    idle  = 0;
    swaps = 0;
    for (size_t t = 0; t < m_tool_count; ++t) {
        if (assigned_counts[t] == 0) { ++idle; continue; }
        int distinct_targets = m_bootstrap_mode ? assigned_counts[t] : (int) tool_ids[t].size();
        swaps += std::max(0, distinct_targets - 1);
    }

    merged = 0;
    if (!m_bootstrap_mode)
        for (const auto &kv : id_counts)
            merged += std::max(0, kv.second - 1);
}

void FilamentMapRowsPanel::UpdateFooter()
{
    for (Row &row : m_rows) {
        int tool, id;
        // Orca: an empty-tool selection paints the same warning state as no selection at
        // all (see RowTargetsEmptyTool) -- it isn't a real assignment either.
        if (ResolveRow(row, tool, id) && !RowTargetsEmptyTool(row))
            row.tile->on_normal();
        else
            row.tile->on_warning();
    }

    int idle, swaps, merged;
    ComputeStats(idle, swaps, merged);

    // Orca: only report what's actually non-zero -- "0 swaps on this plate" (the common case for
    // a straightforward 1:1 plate) was permanent noise next to the counts that actually matter.
    wxString text;
    auto append_stat = [&text](const wxString &segment) {
        if (segment.empty()) return;
        // FromUTF8: a raw "·" literal goes through the ANSI conversion on Windows and renders
        // as mojibake ("Â·" -- field report); the translation macros handle UTF-8, plain
        // char* concatenation does not.
        if (!text.empty()) text += wxString::FromUTF8(" · ");
        text += segment;
    };
    if (idle > 0) append_stat(wxString::Format(_L("%d tools idle"), idle));
    if (swaps > 0) append_stat(wxString::Format(_L("%d swaps on this plate"), swaps));
    if (merged > 0) append_stat(wxString::Format(_L("%d merged"), merged));
    m_footer->SetLabel(text);

    // The mismatch warning lives on its own line (m_warning_label, orange) below the stats line
    // -- see the constructor's comment -- rather than appended to m_footer's text. Orca:
    // the empty-tool warning (EmptyToolWarningText) is a second, independent condition on the
    // same line -- both derive from live row state, so this recombines them every call rather
    // than only reacting to SetFooterWarning's dialog-driven half.
    if (m_warning_label) {
        wxString combined       = m_footer_warning;
        wxString empty_tool_txt = EmptyToolWarningText();
        if (!empty_tool_txt.empty()) {
            if (!combined.empty()) combined += "\n";
            combined += empty_tool_txt;
        }
        m_warning_label->SetLabel(combined);
        // Wrap to the panel's width -- long preset names ran off the dialog's right edge
        // (field report). Wrap() inserts hard newlines into the CURRENT label, so it must
        // run after every SetLabel of fresh text; before the first real layout the panel
        // has no meaningful width yet, so fall back to a fixed sane measure.
        if (!combined.empty()) {
            int wrap_w = GetClientSize().GetWidth();
            if (wrap_w < FromDIP(200))
                wrap_w = FromDIP(520);
            m_warning_label->Wrap(wrap_w);
        }
        m_warning_label->Show(!combined.empty());
    }

    Layout();
    // Orca: deliberately NO Fit() here. This panel's best size is its tile wrap sizer's
    // CalcMin, which stacks one tile per row (see FilamentMapRowsView::repin_rows_min_height),
    // so fitting to it inflated the panel into a tall narrow stack -- and every later content
    // measurement then happened at that distorted geometry. That is how one tile pick that
    // toggled the merge warning blew the dialog up to twice its height, and where the blank
    // band after Reset came from (field reports from Windows). Every event that lands here
    // also runs the hosting view's seed_status, which re-measures honestly and re-fits the
    // host, so a plain relayout is all that is needed.
    if (GetParent())
        GetParent()->Layout();
}

void FilamentMapRowsPanel::SetFooterWarning(const wxString &text)
{
    m_footer_warning = text;
    UpdateFooter();
}

void FilamentMapRowsPanel::SetBaseMap(const std::vector<int> &full_map) { m_base_map = full_map; }
void FilamentMapRowsPanel::SetBasePhysicalMap(const std::vector<int> &full_map) { m_base_physical_map = full_map; }

std::vector<int> FilamentMapRowsPanel::GetFilamentMaps() const
{
    std::vector<int> result(m_filament_count, 1);
    for (size_t i = 0; i < result.size() && i < m_base_map.size(); ++i)
        result[i] = m_base_map[i];

    for (const Row &row : m_rows) {
        int tool, id;
        if (!ResolveRow(row, tool, id)) continue;
        if (tool < 0 || tool >= (int) m_tool_count) continue;
        if (row.filament_id >= 1 && row.filament_id <= (int) result.size())
            result[row.filament_id - 1] = tool + 1;
    }
    return result;
}

std::vector<int> FilamentMapRowsPanel::GetPhysicalMaps() const
{
    std::vector<int> result(m_filament_count, 0);
    for (size_t i = 0; i < result.size() && i < m_base_physical_map.size(); ++i)
        result[i] = m_base_physical_map[i];

    for (const Row &row : m_rows) {
        int tool, id;
        if (!ResolveRow(row, tool, id)) continue;
        // A resolved normal-mode row always has id > 0 (a real physical filament); id <= 0 here
        // means this is a bootstrap row (no physical filament exists yet for this tool pick). Write
        // 0 rather than `continue` so a re-picked row clears any stale id left over from a previous
        // (e.g. merged) assignment -- otherwise the plate's old physical id for this slot survives
        // a bootstrap re-mapping that was meant to replace it.
        if (row.filament_id >= 1 && row.filament_id <= (int) result.size())
            result[row.filament_id - 1] = id > 0 ? id : 0;
    }
    return result;
}

void FilamentMapRowsPanel::LogRowGeometry() const
{
    // Warning level on purpose: Windows configs commonly run log_severity_level=warning, and
    // this line exists precisely for Windows field reports of invisible-but-clickable rows.
    std::string detail;
    for (const Row& row : m_rows) {
        if (row.tile == nullptr) continue;
        const wxRect r = row.tile->GetRect();
        detail += "[f" + std::to_string(row.filament_id) + " shown=" + (row.tile->IsShown() ? "1" : "0") +
                  " rect=" + std::to_string(r.x) + "," + std::to_string(r.y) + "," +
                  std::to_string(r.width) + "x" + std::to_string(r.height) + "] ";
    }
    BOOST_LOG_TRIVIAL(warning) << "mapping row geometry: " << detail;
}

bool FilamentMapRowsPanel::AllRowsAssigned() const
{
    return std::all_of(m_rows.begin(), m_rows.end(), [this](const Row &row) {
        int tool, id;
        // Orca: a row resolved onto a now-empty tool (RowTargetsEmptyTool) is not a real
        // assignment -- see that method's doc -- so OK stays disabled until the user re-picks or
        // presses Automatic.
        return ResolveRow(row, tool, id) && !RowTargetsEmptyTool(row);
    });
}

int FilamentMapRowsPanel::SwapCount() const
{
    int idle, swaps, merged;
    ComputeStats(idle, swaps, merged);
    return swaps;
}

}} // namespace Slic3r::GUI
