#include "SpoolProfileMapDialog.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <wx/colour.h>
#include <wx/defs.h>
#include <wx/dialog.h>
#include <wx/display.h>
#include <wx/event.h>
#include <wx/font.h>
#include <wx/gdicmn.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/toplevel.h>
#include <wx/window.h>
#include <wx/wrapsizer.h>

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "Plater.hpp"
#include "PresetComboBoxes.hpp"
#include "SpoolTile.hpp"
#include "Tab.hpp"
#include "format.hpp"
#include "wxExtensions.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/DropDown.hpp"
#include "Widgets/Label.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r { namespace GUI {

namespace {

constexpr int COMBO_WIDTH = 260;
constexpr int ROW_HEIGHT  = 40;
// A guess at how many chips share a line, only to size the window before it is shown: the chips
// are as wide as their profile names.
constexpr int CHIPS_PER_LINE     = 4;
constexpr int CHIP_LINE_HEIGHT   = 56;
constexpr int VISIBLE_CHIP_LINES = 6;
constexpr int CHIP_GAP           = 8;
// The filament's name and its profile menu share the top line, so the detail panel is wider than
// the shelf.
constexpr int DETAIL_WIDTH = 740;
// Five tiles and the gaps between them, plus the margins and the scroll bar.
constexpr int TILES_PER_LINE    = 5;
constexpr int TILE_GAP          = 8;
constexpr int TILES_WIDTH       = 680;
constexpr int SHELF_TILE_HEIGHT = 110; // a spool with its name under it
constexpr int PICK_TILE_HEIGHT  = 140; // a spool with its name and the menu that picks its profile under it
constexpr int VISIBLE_TILE_LINES = 3;
constexpr double MAIN_WINDOW_SHARE = 0.9;

const wxColour TOP_LINE_COLOR(166, 169, 170);

void add_heading(wxWindow* parent, wxBoxSizer* sizer, const wxString& text, int width)
{
    wxPanel* top_line = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    top_line->SetBackgroundColour(TOP_LINE_COLOR);
    sizer->Add(top_line, 0, wxEXPAND);

    Label* intro = new Label(parent, text, LB_AUTO_WRAP);
    intro->SetMinSize(wxSize(parent->FromDIP(width - 48), -1));
    // Break the text at the width it will actually get, before the dialog is fitted. A wrapping
    // label that is still one line at fit time makes the dialog one line too short, and the layout
    // takes that line back out of the buttons.
    intro->Wrap(parent->FromDIP(width - 48));
    sizer->Add(intro, 0, wxLEFT | wxRIGHT | wxTOP, parent->FromDIP(24));
}

// How tall a list showing `lines` lines of `line_height` should be: all of them, up to `max_lines`.
int list_height(size_t lines, int line_height, int max_lines)
{
    return std::clamp(static_cast<int>(lines), 1, max_lines) * line_height;
}

size_t lines_of(size_t items, size_t per_line)
{
    return (items + per_line - 1) / per_line;
}

wxScrolledWindow* add_list_area(wxWindow* parent, wxBoxSizer* sizer, int width, int height)
{
    wxScrolledWindow* scroll = new wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    scroll->SetScrollRate(0, parent->FromDIP(20));
    scroll->SetBackgroundColour(parent->GetBackgroundColour());
    scroll->SetMinSize(wxSize(parent->FromDIP(width - 48), parent->FromDIP(height)));
    sizer->Add(scroll, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, parent->FromDIP(24));
    return scroll;
}

// A tile is as tall as its name needs, one line or two, so the list is sized on the tiles once they
// are in: as tall as they are, never taller than add_list_area made it.
void fit_list_to_tiles(wxScrolledWindow* scroll)
{
    const int tiles_height = scroll->GetSizer()->GetMinSize().y;
    scroll->SetMinSize(wxSize(scroll->GetMinSize().x, std::min(tiles_height, scroll->GetMinSize().y)));
    scroll->FitInside();
}

// A long list of filaments is the normal case, so the window opens tall: most of the main window,
// never past the screen.
void fit_to_main_window_height(wxDialog* dialog)
{
    const wxWindow* main_window = wxGetApp().mainframe;
    if (main_window == nullptr)
        return;
    const int screen_height = wxDisplay(dialog).GetClientArea().GetHeight();
    const int wanted        = std::min(static_cast<int>(main_window->GetSize().y * MAIN_WINDOW_SHARE), screen_height);
    dialog->SetSize(wxSize(dialog->GetSize().x, std::max(wanted, dialog->GetSize().y)));
}

wxFlexGridSizer* make_tile_grid(wxWindow* parent)
{
    return new wxFlexGridSizer(0, TILES_PER_LINE, parent->FromDIP(TILE_GAP), parent->FromDIP(TILE_GAP));
}

void add_ok_cancel(wxDialog* dialog, wxBoxSizer* sizer, std::function<void()> on_ok)
{
    DialogButtons* buttons = new DialogButtons(dialog, {"OK", "Cancel"});
    buttons->GetOK()->Bind(wxEVT_BUTTON, [on_ok](wxCommandEvent&) { on_ok(); });
    buttons->GetCANCEL()->Bind(wxEVT_BUTTON, [dialog](wxCommandEvent&) { dialog->EndModal(wxID_CANCEL); });
    sizer->AddSpacer(dialog->FromDIP(16));
    sizer->Add(buttons, 0, wxEXPAND);
}

std::string preset_string(const Preset& preset, const char* key)
{
    return preset.config.has(key) ? preset.config.opt_string(key, 0u) : std::string();
}

// Every filament profile the user could pick right now: system and custom alike, but only the
// ones that fit the printer and nozzle currently selected.
std::vector<SpoolPresetCandidate> collect_candidates(const PresetCollection& filaments)
{
    std::vector<SpoolPresetCandidate> out;
    for (const Preset& preset : filaments.get_presets()) {
        if (!preset.is_visible || !preset.is_compatible || preset.is_default)
            continue;
        SpoolPresetCandidate candidate;
        candidate.name        = preset.name;
        candidate.material    = preset_string(preset, "filament_type");
        candidate.vendor      = preset_string(preset, "filament_vendor");
        candidate.color       = preset_string(preset, "default_filament_colour");
        candidate.filament_id = preset.filament_id;
        candidate.is_system   = preset.is_system;
        out.push_back(std::move(candidate));
    }
    return out;
}

// What to call a saved link, months after the printer that reported it was last switched on. The
// name the printer gave was saved with the link; a link saved before that was, or by a printer
// that names nothing, falls back to whatever its key still carries.
wxString link_title(const SpoolLink& link)
{
    if (!link.label.empty())
        return from_u8(link.label);

    const SpoolIdentity spool = spool_identity_from_key(link.key);
    const std::string   name  = spool_display_name(spool);
    if (!name.empty())
        return from_u8(name);
    if (!spool.uid.empty())
        // TRN: %1% is the id read from the tag on a filament spool.
        return format_wxstr(_L("Spool tag %1%"), spool.uid);
    if (link.key.rfind("slot:", 0) == 0)
        // TRN: %1% is the number of a filament lane on the printer.
        return format_wxstr(_L("Lane %1%"), spool.slot_index + 1);
    return from_u8(link.key);
}

// The sub menu the sidebar filament selector files this profile under.
wxString filament_sub_menu(const SpoolPresetCandidate& candidate, const std::string& grouping)
{
    if (candidate.is_system)
        return from_u8(candidate.vendor);
    if (grouping == "0")
        return _L("Custom") + " ";
    if (grouping == "2")
        return candidate.material.empty() ? _L("Unspecified") : from_u8(candidate.material);
    if (grouping == "3")
        return candidate.vendor.empty() ? _L("Unspecified") : from_u8(candidate.vendor);
    return wxString();
}

// Every color the filament has been seen in, plus the ones on a profile of their own.
std::vector<std::string> seen_colors(const SpoolLink& link)
{
    std::vector<std::string> colors = link.colors;
    for (const auto& own : link.color_presets)
        if (std::find(colors.begin(), colors.end(), own.first) == colors.end())
            colors.push_back(own.first);
    return colors;
}

// The material a link is filed under: what the printer reported, else what its profile prints.
std::string link_material(const SpoolLink& link)
{
    const std::string reported = spool_identity_from_key(link.key).material;
    if (!reported.empty())
        return reported;
    const Preset* preset = wxGetApp().preset_bundle->filaments.find_preset(link.preset_name);
    return preset != nullptr ? preset_string(*preset, "filament_type") : std::string();
}

// The vendor of the profile a link points at.
std::string link_vendor(const SpoolLink& link)
{
    const Preset* preset = wxGetApp().preset_bundle->filaments.find_preset(link.preset_name);
    return preset != nullptr ? preset_string(*preset, "filament_vendor") : std::string();
}

// The links filed the way the sidebar files user presets under "Group user filament presets":
// "2" by type, "3" by vendor, anything else in one group.
std::map<std::string, std::vector<SpoolLink>> links_by_group(const std::vector<SpoolLink>& links, const std::string& grouping)
{
    std::map<std::string, std::vector<SpoolLink>> groups;
    for (const SpoolLink& link : links) {
        const std::string group = grouping == "2" ? link_material(link) : grouping == "3" ? link_vendor(link) : std::string();
        groups[group].push_back(link);
    }
    return groups;
}

// "1" (None) lists the links with no heading, "0" (All) puts them under the sidebar's "Custom".
wxString group_heading(const std::string& group, const std::string& grouping)
{
    if (grouping == "1")
        return wxString();
    if (grouping == "0")
        return _L("Custom");
    return group.empty() ? _L("Unspecified") : from_u8(group);
}

std::vector<std::string> tile_variants(const SpoolLink& link)
{
    std::vector<std::string> variants = seen_colors(link);
    if (variants.empty())
        variants.emplace_back();
    return variants;
}

} // namespace

// The very menu the sidebar filament selector puts up -- same color chips, same "User
// presets"/"System presets" split, same sub menus, same grouping preference -- holding only the
// profiles that fit this spool.
class SpoolPresetComboBox : public PresetComboBox
{
public:
    SpoolPresetComboBox(wxWindow* parent, const wxSize& size)
        : PresetComboBox(parent, Preset::TYPE_FILAMENT, size)
    {}

    void fill_for(const std::vector<SpoolPresetCandidate>& all, const SpoolIdentity& spool, bool include_all, const std::string& wanted)
    {
        const std::vector<SpoolPresetCandidate> list = spool_filter_candidates(all, spool, include_all, wanted);
        fill(list, spool_choose_preset(list, spool, wanted));
    }

    // The full preset name, never the shown text: the shown text can be shared by more than one
    // profile.
    std::string selected_preset_name() const
    {
        const int selection = GetSelection();
        if (selection < 0 || static_cast<unsigned int>(selection) >= GetCount())
            return std::string();
        const wxString alias = GetItemAlias(static_cast<unsigned int>(selection));
        return into_u8(alias.empty() ? GetString(static_cast<unsigned int>(selection)) : alias);
    }

private:
    void fill(const std::vector<SpoolPresetCandidate>& list, const std::string& preselect)
    {
        Freeze();
        Clear();
        invalidate_selection();

        // Same preference the sidebar reads, so both menus are grouped the way the user set them up.
        const std::string grouping = wxGetApp().app_config->get("group_filament_presets");
        append_group(list, false, _L("User presets"), grouping);
        append_group(list, true, _L("System presets"), grouping);

        int selected = item_of(preselect);
        if (selected < 0)
            selected = first_profile_item();
        if (selected >= 0) {
            m_last_selected = selected; // so a click on a group header falls back to this one
            SetSelection(selected);
            SetToolTip(GetString(static_cast<unsigned int>(selected)));
        }
        Thaw();
    }

    void append_group(const std::vector<SpoolPresetCandidate>& list, bool system, const wxString& header, const std::string& grouping)
    {
        bool has_header = false;
        for (const SpoolPresetCandidate& candidate : list) {
            if (candidate.is_system != system)
                continue;
            const Preset* preset = m_collection->find_preset(candidate.name);
            if (preset == nullptr)
                continue;
            if (!has_header) {
                set_label_marker(Append(header, wxNullBitmap, DD_ITEM_STYLE_SPLIT_ITEM));
                has_header = true;
            }
            const wxString label = from_u8(candidate.name);
            const int      index = Append(label, *get_bmp(*preset), filament_sub_menu(candidate, grouping));
            SetItemAlias(index, label);
        }
    }

    int item_of(const std::string& name) const
    {
        if (name.empty())
            return -1;
        const wxString wanted = from_u8(name);
        for (unsigned int item = 0; item < GetCount(); ++item)
            if (GetItemAlias(item) == wanted)
                return static_cast<int>(item);
        return -1;
    }

    // Headers carry no alias, so the first item with one is the first profile.
    int first_profile_item() const
    {
        for (unsigned int item = 0; item < GetCount(); ++item)
            if (!GetItemAlias(item).empty())
                return static_cast<int>(item);
        return -1;
    }
};

std::optional<std::vector<SpoolChoice>> SpoolProfileMapDialog::run_link(wxWindow* parent, const std::string& dev_id)
{
    if (parent == nullptr || !spool_sync_needs_asking(dev_id))
        return spool_linked_choices(dev_id);

    SpoolProfileMapDialog dialog(parent, SpoolProfileMap::get().spools(dev_id));
    if (dialog.ShowModal() != wxID_OK)
        return std::nullopt;
    point_sync_at(dialog.chosen());
    return dialog.chosen();
}

SpoolProfileMapDialog::SpoolProfileMapDialog(wxWindow* parent, std::vector<SpoolIdentity> spools)
    : DPIDialog(parent, wxID_ANY, _L("Filaments on the printer"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER)
{
    SetBackgroundColour(*wxWHITE);
    m_candidates = collect_candidates(wxGetApp().preset_bundle->filaments);
    for (SpoolIdentity& spool : spools) {
        if (!spool.has_filament)
            continue;
        std::string key = spool_mapping_key(spool);
        m_rows.push_back(Row{std::move(spool), std::move(key)});
    }
    build(this);
}

void SpoolProfileMapDialog::build(wxWindow* parent)
{
    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);
    add_heading(parent, main_sizer, _L("Choose the filament profile for each spool the printer reports."), TILES_WIDTH);

    m_scroll = add_list_area(parent, main_sizer, TILES_WIDTH, PICK_TILE_HEIGHT * VISIBLE_TILE_LINES);
    wxFlexGridSizer* grid = make_tile_grid(m_scroll);
    for (Row& row : m_rows)
        add_row(m_scroll, grid, row);
    m_scroll->SetSizer(grid);
    fit_list_to_tiles(m_scroll);

    add_ok_cancel(this, main_sizer, [this]() { on_confirm(); });
    SetSizerAndFit(main_sizer);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

void SpoolProfileMapDialog::add_row(wxWindow* parent, wxFlexGridSizer* grid, Row& row)
{
    const std::string name = spool_display_name(row.spool);
    wxBoxSizer*       cell = new wxBoxSizer(wxVERTICAL);
    cell->Add(new SpoolTile(parent, {spool_variant_color(row.spool)}, name.empty() ? _L("Unknown filament") : from_u8(name)), 0,
              wxBOTTOM, FromDIP(4));

    row.combo = new SpoolPresetComboBox(parent, wxSize(FromDIP(SPOOL_TILE_WIDTH), -1));
    cell->Add(row.combo, 0);
    grid->Add(cell, 0);
    // A generic spool is never linked, see SpoolProfileLinker: its pick is for this sync only.
    const std::string remembered = spool_is_generic(row.spool) ? std::string() :
                                                                  SpoolProfileMap::get().remembered(row.key, spool_variant_color(row.spool));
    row.combo->fill_for(m_candidates, row.spool, true, remembered);
}

void SpoolProfileMapDialog::on_confirm()
{
    const PresetCollection& filaments = wxGetApp().preset_bundle->filaments;
    m_chosen.clear();
    for (const Row& row : m_rows) {
        const std::string preset_name = row.combo->selected_preset_name();
        const Preset*     preset      = filaments.find_preset(preset_name);
        if (preset == nullptr)
            continue;
        m_chosen.push_back(SpoolChoice{row.spool.slot_index, preset_name, preset->filament_id});
        if (!spool_is_generic(row.spool))
            SpoolProfileMap::get().remember_spool(row.key, preset_name, spool_display_name(row.spool), spool_variant_color(row.spool));
    }

    if (m_chosen.empty()) {
        MessageDialog dialog(this, _L("No filament profile was picked, so nothing was synchronized."), _L("Filaments on the printer"), wxOK);
        dialog.ShowModal();
        return;
    }
    EndModal(wxID_OK);
}

void SpoolProfileMapDialog::on_dpi_changed(const wxRect& /*suggested_rect*/)
{
    Fit();
    Refresh();
}

void SpoolProfileLinksDialog::run_manage(wxWindow* parent)
{
    if (parent == nullptr || wxGetApp().preset_bundle == nullptr)
        return;
    SpoolProfileLinksDialog dialog(parent);
    dialog.ShowModal();
}

SpoolProfileLinksDialog::SpoolProfileLinksDialog(wxWindow* parent)
    : DPIDialog(parent, wxID_ANY, _L("Saved filament links"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER)
{
    SetBackgroundColour(*wxWHITE);
    build(this);
}

void SpoolProfileLinksDialog::build(wxWindow* parent)
{
    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);
    add_heading(parent, main_sizer,
                _L("The filaments linked to a profile. Click a filament to see its colors and the profile each one prints with."),
                TILES_WIDTH);

    const size_t links = SpoolProfileMap::get().all_remembered().size();
    m_scroll = add_list_area(parent, main_sizer, TILES_WIDTH, list_height(lines_of(links, TILES_PER_LINE), SHELF_TILE_HEIGHT, VISIBLE_TILE_LINES));
    fill_shelf();

    add_ok_cancel(this, main_sizer, [this]() { EndModal(wxID_OK); });
    SetSizerAndFit(main_sizer);
    fit_to_main_window_height(this);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

// Drawn again after a spool was edited or forgotten in its detail panel, from what is on disk.
void SpoolProfileLinksDialog::fill_shelf()
{
    m_scroll->DestroyChildren();
    const std::vector<SpoolLink> links = SpoolProfileMap::get().all_remembered();

    const std::string grouping = wxGetApp().app_config->get("group_filament_presets");
    wxBoxSizer*       shelf    = new wxBoxSizer(wxVERTICAL);
    for (const auto& [group, group_links] : links_by_group(links, grouping))
        add_group(m_scroll, shelf, group, group_heading(group, grouping), group_links);
    if (links.empty())
        add_empty_note(m_scroll, shelf);

    m_scroll->SetSizer(shelf);
    // The tiles are built after the dialog was themed, so they are themed here too: without this a
    // redrawn shelf comes back with black text on the dark background.
    wxGetApp().UpdateDarkUIWin(m_scroll);
    m_scroll->FitInside();
    m_scroll->Layout();
}

void SpoolProfileLinksDialog::add_group(
    wxWindow* parent, wxSizer* shelf, const std::string& group, const wxString& heading, const std::vector<SpoolLink>& links)
{
    if (!heading.empty())
        shelf->Add(new Label(parent, Label::Head_14, heading), 0, wxBOTTOM, parent->FromDIP(TILE_GAP / 2));

    wxFlexGridSizer* grid = make_tile_grid(parent);
    for (const SpoolLink& link : links)
        add_tile(parent, grid, group, link);
    shelf->Add(grid, 0, wxBOTTOM, parent->FromDIP(TILE_GAP));
}

void SpoolProfileLinksDialog::add_tile(wxWindow* parent, wxFlexGridSizer* grid, const std::string& group, const SpoolLink& link)
{
    // The heading above already names the group, so the tile only says what tells it apart.
    const wxString title = from_u8(spool_name_without_group(into_u8(link_title(link)), group));
    SpoolTile*     tile  = new SpoolTile(parent, tile_variants(link), title);
    // Opened after the click is handled: the shelf is redrawn when the detail closes, and the tile
    // that was clicked is gone by then.
    tile->on_click([this, link]() { CallAfter(&SpoolProfileLinksDialog::open_detail, link); });
    grid->Add(tile, 0);
}

void SpoolProfileLinksDialog::add_empty_note(wxWindow* parent, wxSizer* shelf)
{
    Label* note = new Label(parent, _L("No filament has been linked to a profile yet. Synchronize with a printer and the choices you make "
                                       "there are listed here."));
    note->Wrap(parent->FromDIP(TILES_WIDTH - 72));
    shelf->Add(note, 0);
}

void SpoolProfileLinksDialog::open_detail(SpoolLink link)
{
    SpoolDetailDialog dialog(this, std::move(link));
    dialog.ShowModal();
    fill_shelf();
}

void SpoolProfileLinksDialog::on_dpi_changed(const wxRect& /*suggested_rect*/)
{
    Fit();
    Refresh();
}

SpoolDetailDialog::SpoolDetailDialog(wxWindow* parent, SpoolLink link)
    : DPIDialog(parent, wxID_ANY, _L("Spool details"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER)
{
    SetBackgroundColour(*wxWHITE);
    m_candidates = collect_candidates(wxGetApp().preset_bundle->filaments);
    m_row        = make_row(std::move(link));
    build(this);
}

SpoolDetailDialog::Row SpoolDetailDialog::make_row(SpoolLink link)
{
    Row row;
    row.link   = std::move(link);
    row.preset = row.link.preset_name;

    for (const std::string& color : seen_colors(row.link)) {
        ColorRow entry;
        entry.color    = color;
        const auto own = row.link.color_presets.find(color);
        if (own != row.link.color_presets.end())
            entry.initial = own->second;
        entry.own = entry.initial;
        row.colors.push_back(std::move(entry));
    }
    if (row.colors.size() >= 2)
        return row;

    // A filament seen in one color only IS that color: there is nothing to tell apart from the rest
    // of it, so a profile saved for the color becomes the filament's own. The link still says the
    // old profile, so that is what the menu starts from, and OK writes the move.
    for (ColorRow& color : row.colors) {
        if (color.own.empty())
            continue;
        row.preset  = color.own;
        row.initial = row.link.preset_name;
        row.filled  = true;
        color.own.clear();
    }
    return row;
}

void SpoolDetailDialog::build(wxWindow* parent)
{
    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);
    add_heading(parent, main_sizer,
                _L("The profile this filament is linked to, and every color it was seen in with the profile that color prints "
                   "with. Click a color to choose a profile of its own for it, or forget the link with the button next to the "
                   "filament's profile."),
                DETAIL_WIDTH);

    const int chips_height = list_height(lines_of(m_row.colors.size(), CHIPS_PER_LINE), CHIP_LINE_HEIGHT, VISIBLE_CHIP_LINES);
    m_scroll               = add_list_area(parent, main_sizer, DETAIL_WIDTH, ROW_HEIGHT + chips_height);
    rebuild_rows();

    add_ok_cancel(this, main_sizer, [this]() { on_confirm(); });
    SetSizerAndFit(main_sizer);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

// What the filament's menu is showing right now, so the redrawn list carries the user's edits
// instead of what happens to be on disk.
void SpoolDetailDialog::take_menu_choice()
{
    if (m_row.combo != nullptr)
        m_row.preset = m_row.combo->selected_preset_name();
    m_row.combo = nullptr;
}

// A color put on the profile the filament prints with is no longer a variation of it.
void SpoolDetailDialog::settle_colors()
{
    for (ColorRow& color : m_row.colors)
        if (color.own == m_row.preset)
            color.own.clear();
}

void SpoolDetailDialog::rebuild_rows()
{
    take_menu_choice();
    settle_colors();

    m_scroll->DestroyChildren();
    // The chips come after the filament's line: their labels read the profile its menu settled on.
    wxBoxSizer* list = new wxBoxSizer(wxVERTICAL);
    list->Add(make_filament_line(m_scroll), 0, wxEXPAND);
    list->Add(make_variant_chips(m_scroll), 0, wxEXPAND | wxTOP, FromDIP(12));
    m_scroll->SetSizer(list);
    // The list is built after the dialog was themed, so it is themed here too: without this a
    // redrawn list comes back with black text on the dark background.
    wxGetApp().UpdateDarkUIWin(m_scroll);
    m_scroll->FitInside();
    m_scroll->Layout();
    Layout();
}

wxSizer* SpoolDetailDialog::make_filament_line(wxWindow* parent)
{
    wxBoxSizer* line = new wxBoxSizer(wxHORIZONTAL);
    line->Add(new Label(parent, Label::Body_14, link_title(m_row.link)), 0, wxALIGN_CENTER_VERTICAL);

    m_row.combo = new SpoolPresetComboBox(parent, wxSize(FromDIP(COMBO_WIDTH), -1));
    line->Add(m_row.combo, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(12));
    m_row.combo->fill_for(m_candidates, spool_identity_from_key(m_row.link.key), true, m_row.preset);
    m_row.preset = m_row.combo->selected_preset_name();
    if (!m_row.filled) {
        m_row.initial = m_row.preset;
        m_row.filled  = true;
    }
    // The chips of the colors that follow the filament say this profile.
    m_row.combo->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent& evt) {
        evt.Skip();
        CallAfter(&SpoolDetailDialog::rebuild_rows);
    });

    ScalableButton* forget = new ScalableButton(parent, wxID_ANY, "delete");
    forget->SetToolTip(_L("Forget this link"));
    forget->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { forget_link(); });
    line->Add(forget, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    return line;
}

// Every color this filament has been seen in, each with the profile it prints with. A link saved
// before colors were kept shows the empty chip until the next sync fills it in.
wxSizer* SpoolDetailDialog::make_variant_chips(wxWindow* parent)
{
    // Without the flags, the wrap sizer stretches the last chip of every line to the edge.
    wxWrapSizer* chips = new wxWrapSizer(wxHORIZONTAL, wxREMOVE_LEADING_SPACES);
    if (m_row.colors.empty())
        chips->Add(new VariantChip(parent, std::string(), from_u8(m_row.preset), false), 0, wxRIGHT | wxBOTTOM, FromDIP(CHIP_GAP));
    for (ColorRow& color : m_row.colors)
        chips->Add(make_variant_chip(parent, color), 0, wxRIGHT | wxBOTTOM, FromDIP(CHIP_GAP));
    return chips;
}

VariantChip* SpoolDetailDialog::make_variant_chip(wxWindow* parent, ColorRow& color)
{
    const bool   own  = !color.own.empty();
    VariantChip* chip = new VariantChip(parent, color.color, from_u8(own ? color.own : m_row.preset), own);
    // One color is the whole filament, so there is nothing to give a profile of its own to and the
    // chip is there to look at, not to click.
    if (m_row.colors.size() < 2) {
        chip->SetToolTip(from_u8(color.color));
        return chip;
    }

    SpoolPresetComboBox* menu = add_chip_menu(chip, color);
    chip->on_pick([menu]() { menu->ForceDropdownOpen(); });
    if (!own) {
        // TRN: %1% is a color as #RRGGBB, or several of them for a multicolor filament.
        chip->SetToolTip(format_wxstr(_L("%1%\nClick to choose the profile this color prints with"), color.color));
        return chip;
    }
    // TRN: %1% is a color as #RRGGBB, or several of them for a multicolor filament.
    chip->SetToolTip(format_wxstr(_L("%1%\nClick to choose the profile this color prints with, or the delete icon to put it back "
                                     "on the filament's profile"),
                                  color.color));
    chip->on_forget([this, &color]() {
        color.own.clear();
        CallAfter(&SpoolDetailDialog::rebuild_rows);
    });
    return chip;
}

// The menu a chip opens, never shown itself: it sits under the chip's profile name so its list
// drops from there.
SpoolPresetComboBox* SpoolDetailDialog::add_chip_menu(VariantChip* chip, ColorRow& color)
{
    const wxRect         area = chip->profile_area();
    SpoolPresetComboBox* menu = new SpoolPresetComboBox(chip, area.GetSize());
    menu->SetPosition(area.GetPosition());
    menu->Hide();
    menu->GetDropDown().SetUseContentWidth(true);
    menu->fill_for(m_candidates, spool_identity_from_key(m_row.link.key), true, color.own.empty() ? m_row.preset : color.own);
    menu->Bind(wxEVT_COMBOBOX, [this, menu, &color](wxCommandEvent& evt) {
        evt.Skip();
        const std::string picked = menu->selected_preset_name();
        color.own                = picked == m_row.preset ? std::string() : picked;
        CallAfter(&SpoolDetailDialog::rebuild_rows);
    });
    return menu;
}

void SpoolDetailDialog::forget_link()
{
    SpoolProfileMap::get().remember(m_row.link.key, std::string());
    EndModal(wxID_OK);
}

void SpoolDetailDialog::remember_colors(const std::string& picked) const
{
    for (const ColorRow& color : m_row.colors) {
        // A color on the profile the filament ends up on is not a variation and is stored as none.
        const std::string stored = color.own != picked ? color.own : std::string();
        if (stored != color.initial)
            SpoolProfileMap::get().remember_color(m_row.link.key, color.color, stored);
    }
}

void SpoolDetailDialog::on_confirm()
{
    const std::string picked = m_row.combo != nullptr ? m_row.combo->selected_preset_name() : m_row.preset;
    // Only what the user actually changed is written back, so a spool they never touched keeps
    // exactly the profile name it had on disk.
    if (!picked.empty() && picked != m_row.initial) {
        SpoolProfileMap::get().remember(m_row.link.key, picked, m_row.link.label);
        follow_link_in_project(m_row.initial, picked);
    }
    remember_colors(picked);
    EndModal(wxID_OK);
}

// The link and the project have to say the same thing: a filament sitting in the project on the
// profile the user has just moved away from is moved with it, so nobody has to redo the change by
// hand in the sidebar.
void SpoolDetailDialog::follow_link_in_project(const std::string& old_name, const std::string& new_name)
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    // A profile that is no longer installed cannot be selected, so the project is left alone.
    if (old_name.empty() || bundle == nullptr || bundle->filaments.find_preset(new_name) == nullptr)
        return;

    bool changed = false;
    for (const size_t index : bundle->physical_filament_config_indices()) {
        std::string& name = bundle->filament_presets[index];
        if (name != old_name)
            continue;
        name    = new_name;
        changed = true;
    }
    if (!changed)
        return;

    bundle->update_multi_material_filament_presets();
    // The same refresh the sync flow runs once it has rewritten the selected profiles.
    if (Tab* tab = wxGetApp().get_tab(Preset::TYPE_FILAMENT); tab != nullptr && !bundle->filament_presets.empty())
        tab->select_preset(bundle->filament_presets[0]);
    bundle->export_selections(*wxGetApp().app_config);
    if (Plater* plater = wxGetApp().plater(); plater != nullptr)
        plater->sidebar().update_presets(Preset::TYPE_FILAMENT);
}

void SpoolDetailDialog::on_dpi_changed(const wxRect& /*suggested_rect*/)
{
    Fit();
    Refresh();
}

}} // namespace Slic3r::GUI
