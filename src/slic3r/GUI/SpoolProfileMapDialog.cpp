#include "SpoolProfileMapDialog.hpp"

#include <algorithm>
#include <charconv>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <wx/brush.h>
#include <wx/colour.h>
#include <wx/cursor.h>
#include <wx/dcbuffer.h>
#include <wx/defs.h>
#include <wx/dialog.h>
#include <wx/event.h>
#include <wx/font.h>
#include <wx/gdicmn.h>
#include <wx/panel.h>
#include <wx/pen.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/tglbtn.h>
#include <wx/toplevel.h>
#include <wx/window.h>

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "Plater.hpp"
#include "PresetComboBoxes.hpp"
#include "SpoolTile.hpp"
#include "Tab.hpp"
#include "format.hpp"
#include "wxExtensions.hpp"
#include "Widgets/CheckBox.hpp"
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

using Chosen = SpoolProfileMapDialog::Chosen;

constexpr int SWATCH_SIZE    = 22;
constexpr int MARK_HEIGHT    = 5; // room under a chip for the mark that says "this color has its own profile"
constexpr int CHIPS_PER_LINE = 4; // a filament seen in many colors grows downwards, not sideways
constexpr int TOGGLE_WIDTH   = 16;
constexpr int ELBOW_WIDTH    = 18;
constexpr int COMBO_WIDTH    = 260;
constexpr int NAME_WIDTH     = 220;
constexpr int ROW_HEIGHT     = 40;
constexpr int VISIBLE_ROWS   = 8;
// The detail panel carries a color strip the tile panels do not, so it gets the room for it.
constexpr int DETAIL_WIDTH = 740;
// Three tiles and the gaps between them, plus the margins and the scroll bar.
constexpr int TILES_PER_LINE   = 3;
constexpr int TILE_GAP         = 12;
constexpr int TILES_WIDTH      = 680;
constexpr int SHELF_TILE_HEIGHT = 100; // a tile with its profile name under it
constexpr int PICK_TILE_HEIGHT  = 120; // a tile with the menu that picks its profile under it
constexpr int VISIBLE_TILE_LINES = 3;
// As MoonrakerPrinterAgent::build_ams_payload lays them out.
constexpr int SLOTS_PER_AMS = 4;

const wxColour TOP_LINE_COLOR(166, 169, 170);

// The elbow that hangs a color off the filament it belongs to: a line down the left, turning
// right at the middle of the row. The last color of a filament stops the line there.
class TreeElbow : public wxWindow
{
public:
    TreeElbow(wxWindow* parent, const wxSize& size, bool last)
        : wxWindow(parent, wxID_ANY, wxDefaultPosition, size)
        , m_last(last)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &TreeElbow::on_paint, this);
    }

private:
    void on_paint(wxPaintEvent& /*event*/)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
        dc.Clear();
        const wxSize size   = GetSize();
        const int    line_x = size.x / 3;
        const int    mid_y  = size.y / 2;
        dc.SetPen(wxPen(SPOOL_LINE_COLOR));
        dc.DrawLine(line_x, 0, line_x, m_last ? mid_y : size.y);
        dc.DrawLine(line_x, mid_y, size.x, mid_y);
    }

    bool m_last;
};

// The handle that folds a filament's colors away: an arrow pointing right when they are hidden and
// down when they are shown. Drawn rather than written, so it looks the same on every platform.
class TreeToggle : public wxWindow
{
public:
    TreeToggle(wxWindow* parent, const wxSize& size, bool open)
        : wxWindow(parent, wxID_ANY, wxDefaultPosition, size)
        , m_open(open)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetCursor(wxCursor(wxCURSOR_HAND));
        Bind(wxEVT_PAINT, &TreeToggle::on_paint, this);
    }

private:
    void on_paint(wxPaintEvent& /*event*/)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
        dc.Clear();
        const wxSize size     = GetSize();
        const int    side     = std::min(size.x, size.y) / 2;
        const int    center_x = size.x / 2;
        const int    center_y = size.y / 2;
        wxPoint      arrow[3];
        if (m_open) {
            arrow[0] = wxPoint(center_x - side, center_y - side / 2);
            arrow[1] = wxPoint(center_x + side, center_y - side / 2);
            arrow[2] = wxPoint(center_x, center_y + side);
        } else {
            arrow[0] = wxPoint(center_x - side / 2, center_y - side);
            arrow[1] = wxPoint(center_x - side / 2, center_y + side);
            arrow[2] = wxPoint(center_x + side, center_y);
        }
        dc.SetPen(wxPen(SPOOL_LINE_COLOR));
        dc.SetBrush(wxBrush(SPOOL_LINE_COLOR));
        dc.DrawPolygon(3, arrow);
    }

    bool m_open;
};

// One "[x] some text" option line, where clicking the text toggles the box the way Orca's own
// option rows do.
struct CheckRow
{
    ::CheckBox* check = nullptr;
    Label*      text  = nullptr;
};

CheckRow add_check_row(wxWindow* parent, wxBoxSizer* sizer, const wxString& text, bool value, std::function<void(bool)> on_toggle)
{
    CheckRow row;
    // ::CheckBox is Orca's toggle widget; Slic3r::GUI::CheckBox is the settings Field of the same name.
    row.check = new ::CheckBox(parent);
    row.check->SetValue(value);
    row.text = new Label(parent, text);

    ::CheckBox* check = row.check;
    row.check->Bind(wxEVT_TOGGLEBUTTON, [check, on_toggle](wxCommandEvent& evt) {
        on_toggle(check->GetValue());
        evt.Skip();
    });
    row.text->Bind(wxEVT_LEFT_DOWN, [check, on_toggle](wxMouseEvent&) {
        if (!check->IsEnabled())
            return;
        check->SetValue(!check->GetValue());
        on_toggle(check->GetValue());
    });

    wxBoxSizer* line = new wxBoxSizer(wxHORIZONTAL);
    line->Add(row.check, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, parent->FromDIP(8));
    line->Add(row.text, 0, wxALIGN_CENTER_VERTICAL);
    sizer->Add(line, 0, wxLEFT | wxRIGHT | wxTOP, parent->FromDIP(24));
    return row;
}

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

size_t tile_lines(size_t tiles)
{
    return (tiles + TILES_PER_LINE - 1) / TILES_PER_LINE;
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

wxFlexGridSizer* make_list_grid(wxWindow* parent, int columns)
{
    wxFlexGridSizer* grid = new wxFlexGridSizer(0, columns, parent->FromDIP(8), parent->FromDIP(12));
    grid->AddGrowableCol(1, 1);
    grid->AddGrowableCol(2, 1);
    grid->SetFlexibleDirection(wxHORIZONTAL);
    return grid;
}

wxFlexGridSizer* make_tile_grid(wxWindow* parent)
{
    return new wxFlexGridSizer(0, TILES_PER_LINE, parent->FromDIP(TILE_GAP), parent->FromDIP(TILE_GAP));
}

void add_title(wxWindow* parent, wxFlexGridSizer* grid, const wxFont& font, const wxString& text)
{
    Label* title = new Label(parent, font, text);
    title->SetMinSize(wxSize(parent->FromDIP(NAME_WIDTH), -1));
    grid->Add(title, 1, wxALIGN_CENTER_VERTICAL);
}

ScalableButton* add_delete_button(wxWindow* parent, wxFlexGridSizer* grid, const wxString& tooltip)
{
    ScalableButton* button = new ScalableButton(parent, wxID_ANY, "delete");
    button->SetToolTip(tooltip);
    grid->Add(button, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, parent->FromDIP(4));
    return button;
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

wxString spool_title(const SpoolIdentity& spool)
{
    const std::string name = spool_display_name(spool);
    return name.empty() ? _L("Unknown filament") : from_u8(name);
}

// A lane number out of one of the AMS entry's own string options. -1 for anything that is
// missing, of another type or not a number: an entry we cannot place is an entry we skip.
int config_index(const DynamicPrintConfig& config, const char* key)
{
    const ConfigOptionStrings* option = config.option<ConfigOptionStrings>(key);
    if (option == nullptr || option->values.empty())
        return -1;
    const std::string& text  = option->values.front();
    int                index = -1;
    const auto         result = std::from_chars(text.data(), text.data() + text.size(), index);
    return result.ec == std::errc() ? index : -1;
}

DynamicPrintConfig* ams_entry_for(PresetBundle& bundle, int slot_index)
{
    for (auto& item : bundle.filament_ams_list) {
        const int ams_id  = config_index(item.second, "ams_id");
        const int slot_id = config_index(item.second, "slot_id");
        if (ams_id >= 0 && slot_id >= 0 && ams_id * SLOTS_PER_AMS + slot_id == slot_index)
            return &item.second;
    }
    return nullptr;
}

Chosen make_chosen(const PresetCollection& filaments, int slot_index, const std::string& preset_name)
{
    const Preset* preset = filaments.find_preset(preset_name);
    return Chosen{slot_index, preset_name, preset != nullptr ? preset->filament_id : std::string()};
}

// Whether a link may be kept for this spool, and read back for it.
bool is_linkable(const SpoolIdentity& spool)
{
    return !spool_is_generic(spool) || SpoolProfileMap::get().settings().remember_generic;
}

bool any_new_spool(const std::vector<SpoolIdentity>& spools)
{
    return std::any_of(spools.begin(), spools.end(), [](const SpoolIdentity& spool) {
        if (!spool.has_filament)
            return false;
        return !is_linkable(spool) || SpoolProfileMap::get().is_new_spool(spool_mapping_key(spool), spool_variant_color(spool));
    });
}

std::vector<Chosen> remembered_choices(const PresetCollection& filaments, const std::vector<SpoolIdentity>& spools)
{
    std::vector<Chosen> choices;
    for (const SpoolIdentity& spool : spools) {
        if (!spool.has_filament)
            continue;
        const std::string name = SpoolProfileMap::get().remembered(spool_mapping_key(spool), spool_variant_color(spool));
        if (!name.empty())
            choices.push_back(make_chosen(filaments, spool.slot_index, name));
    }
    return choices;
}

// Point each reported lane at the profile the user picked, so the commit that follows resolves
// that profile instead of falling back to a Generic one.
void point_ams_list_at(PresetBundle& bundle, const std::vector<Chosen>& choices)
{
    for (const Chosen& choice : choices) {
        const Preset* preset = bundle.filaments.find_preset(choice.preset_name);
        if (preset == nullptr || choice.filament_id.empty())
            continue;
        DynamicPrintConfig* entry = ams_entry_for(bundle, choice.slot_index);
        if (entry == nullptr)
            continue;
        entry->set_key_value("filament_id", new ConfigOptionStrings{choice.filament_id});
        entry->set_key_value("filament_type", new ConfigOptionStrings{preset_string(*preset, "filament_type")});
    }
}

int unplaced_pick_for(const std::vector<Chosen>& choices, const std::vector<bool>& placed, const std::string& filament_id)
{
    for (size_t pick = 0; pick < choices.size(); ++pick)
        if (!placed[pick] && !choices[pick].filament_id.empty() && choices[pick].filament_id == filament_id)
            return static_cast<int>(pick);
    return -1;
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

std::map<std::string, std::vector<SpoolLink>> links_by_material(const std::vector<SpoolLink>& links)
{
    std::map<std::string, std::vector<SpoolLink>> groups;
    for (const SpoolLink& link : links)
        groups[link_material(link)].push_back(link);
    return groups;
}

std::vector<std::string> tile_variants(const SpoolLink& link)
{
    std::vector<std::string> variants = seen_colors(link);
    if (variants.empty())
        variants.emplace_back();
    return variants;
}

wxWindow* make_color_head(wxWindow* parent, const std::string& color, bool last)
{
    wxPanel*    head       = new wxPanel(parent, wxID_ANY);
    wxBoxSizer* head_sizer = new wxBoxSizer(wxHORIZONTAL);
    head->SetBackgroundColour(parent->GetBackgroundColour());
    head_sizer->AddSpacer(parent->FromDIP(TOGGLE_WIDTH + 4));
    head_sizer->Add(new TreeElbow(head, wxSize(parent->FromDIP(ELBOW_WIDTH), parent->FromDIP(SWATCH_SIZE + MARK_HEIGHT)), last), 0);
    ColorSwatch* chip = new ColorSwatch(head, color, wxSize(parent->FromDIP(SWATCH_SIZE), parent->FromDIP(SWATCH_SIZE)));
    chip->SetToolTip(from_u8(color));
    head_sizer->Add(chip, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, parent->FromDIP(4));
    head->SetSizerAndFit(head_sizer);
    return head;
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

std::optional<std::vector<Chosen>> SpoolProfileMapDialog::run_link(wxWindow* parent, const std::string& dev_id)
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (parent == nullptr || bundle == nullptr)
        return std::vector<Chosen>();

    // A color-only sync keeps the project's profiles, so there is nothing to pick.
    if (wxGetApp().app_config->get("sync_ams_filament_mode") == "1")
        return std::vector<Chosen>();

    // Switched off on the saved-links panel: synchronizing takes exactly the path it took before
    // this picker existed.
    const SpoolMapSettings settings = SpoolProfileMap::get().settings();
    if (!settings.enabled)
        return std::vector<Chosen>();

    std::vector<SpoolIdentity> spools = SpoolProfileMap::get().spools(dev_id);
    if (std::none_of(spools.begin(), spools.end(), [](const SpoolIdentity& spool) { return spool.has_filament; }))
        return std::vector<Chosen>();

    // Every filament on the printer is one the user has already answered for: apply what they
    // chose and do not put a panel in front of them again.
    if (settings.ask_only_new && !any_new_spool(spools)) {
        std::vector<Chosen> choices = remembered_choices(bundle->filaments, spools);
        point_ams_list_at(*bundle, choices);
        return choices;
    }

    SpoolProfileMapDialog dialog(parent, std::move(spools));
    if (dialog.ShowModal() != wxID_OK)
        return std::nullopt;
    point_ams_list_at(*bundle, dialog.chosen());
    return dialog.chosen();
}

void SpoolProfileMapDialog::apply_links(const std::vector<Chosen>& chosen)
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (chosen.empty() || bundle == nullptr)
        return;

    std::vector<bool> placed(chosen.size(), false);
    for (const size_t index : bundle->physical_filament_config_indices()) {
        const Preset* current = bundle->filaments.find_preset(bundle->filament_presets[index]);
        if (current == nullptr)
            continue;
        const int pick = unplaced_pick_for(chosen, placed, current->filament_id);
        if (pick < 0)
            continue;
        placed[pick]                    = true;
        bundle->filament_presets[index] = chosen[pick].preset_name;
    }
    bundle->update_multi_material_filament_presets();
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

    m_scroll = add_list_area(parent, main_sizer, TILES_WIDTH,
                             list_height(tile_lines(m_rows.size()), PICK_TILE_HEIGHT, VISIBLE_TILE_LINES));
    wxFlexGridSizer* grid = make_tile_grid(m_scroll);
    for (Row& row : m_rows)
        add_row(m_scroll, grid, row);
    m_scroll->SetSizer(grid);
    m_scroll->FitInside();

    add_check_row(parent, main_sizer, _L("Show profiles of every material"), m_show_all, [this](bool value) {
        m_show_all = value;
        refill_combos();
    });
    add_check_row(parent, main_sizer, _L("Remember the profile chosen for each filament"), m_remember,
                  [this](bool value) { m_remember = value; });

    add_ok_cancel(this, main_sizer, [this]() { on_confirm(); });
    SetSizerAndFit(main_sizer);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

void SpoolProfileMapDialog::add_row(wxWindow* parent, wxFlexGridSizer* grid, Row& row)
{
    wxBoxSizer* cell = new wxBoxSizer(wxVERTICAL);
    cell->Add(new SpoolTile(parent, {spool_variant_color(row.spool)}, spool_title(row.spool), from_u8(row.spool.material)), 0,
              wxBOTTOM, FromDIP(4));

    row.combo = new SpoolPresetComboBox(parent, wxSize(FromDIP(SPOOL_TILE_WIDTH), -1));
    cell->Add(row.combo, 0);
    grid->Add(cell, 0);
    const std::string remembered = is_linkable(row.spool) ? SpoolProfileMap::get().remembered(row.key, spool_variant_color(row.spool)) : std::string();
    row.combo->fill_for(m_candidates, row.spool, m_show_all, remembered);
}

void SpoolProfileMapDialog::refill_combos()
{
    for (Row& row : m_rows)
        row.combo->fill_for(m_candidates, row.spool, m_show_all, row.combo->selected_preset_name());
    Layout();
}

void SpoolProfileMapDialog::remember_row(const Row& row, const std::string& preset_name) const
{
    if (!is_linkable(row.spool))
        return;
    // Unticked means forget: the link on disk goes away with the box the user just cleared.
    if (!m_remember)
        SpoolProfileMap::get().remember(row.key, std::string());
    else
        SpoolProfileMap::get().remember_spool(row.key, preset_name, spool_display_name(row.spool), spool_variant_color(row.spool));
}

void SpoolProfileMapDialog::on_confirm()
{
    const PresetCollection& filaments = wxGetApp().preset_bundle->filaments;
    m_chosen.clear();
    for (const Row& row : m_rows) {
        const std::string preset_name = row.combo->selected_preset_name();
        if (preset_name.empty())
            continue;
        m_chosen.push_back(make_chosen(filaments, row.spool.slot_index, preset_name));
        remember_row(row, preset_name);
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

void SpoolProfileLinksDialog::run_manage(wxWindow* parent, SpoolLook look)
{
    if (parent == nullptr || wxGetApp().preset_bundle == nullptr)
        return;
    SpoolProfileLinksDialog dialog(parent, look);
    dialog.ShowModal();
}

SpoolProfileLinksDialog::SpoolProfileLinksDialog(wxWindow* parent, SpoolLook look)
    : DPIDialog(parent, wxID_ANY, _L("Saved filament links"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER)
    , m_look(look)
{
    SetBackgroundColour(*wxWHITE);
    m_settings = SpoolProfileMap::get().settings();
    build(this);
}

void SpoolProfileLinksDialog::build(wxWindow* parent)
{
    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);
    add_heading(parent, main_sizer,
                _L("The profile each spool is linked to. Click a spool to see its colors and change its profile."), TILES_WIDTH);

    const size_t links = SpoolProfileMap::get().all_remembered().size();
    m_scroll = add_list_area(parent, main_sizer, TILES_WIDTH, list_height(tile_lines(links), SHELF_TILE_HEIGHT, VISIBLE_TILE_LINES));
    fill_shelf();

    add_check_row(parent, main_sizer, _L("Link filaments to profiles when synchronizing with a printer"), m_settings.enabled,
                  [this](bool value) {
                      m_settings.enabled = value;
                      // Nothing is being linked, so there is nothing left to choose about when to ask.
                      m_only_new_check->Enable(value);
                      m_only_new_label->Enable(value);
                  });
    const CheckRow only_new = add_check_row(parent, main_sizer, _L("Only ask about a filament or a color not seen before"),
                                            m_settings.ask_only_new, [this](bool value) { m_settings.ask_only_new = value; });
    m_only_new_check        = only_new.check;
    m_only_new_label        = only_new.text;
    m_only_new_check->Enable(m_settings.enabled);
    m_only_new_label->Enable(m_settings.enabled);

    add_check_row(parent, main_sizer, _L("Also link spools the printer describes by material only"), m_settings.remember_generic,
                  [this](bool value) { m_settings.remember_generic = value; });
    Label* generic_note = new Label(parent, _L("Some printers only say \"PLA\" for every PLA spool, whatever its brand. Linking those puts all your "
                                               "PLA on one profile, which is wrong if you use whites from several brands. Off, you are asked "
                                               "about them on every sync."));
    generic_note->Wrap(parent->FromDIP(TILES_WIDTH - 48 - 32));
    main_sizer->Add(generic_note, 0, wxLEFT | wxRIGHT | wxTOP, parent->FromDIP(24 + 32));

    add_ok_cancel(this, main_sizer, [this]() { on_confirm(); });
    SetSizerAndFit(main_sizer);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

// Drawn again after a spool was edited or forgotten in its detail panel, from what is on disk.
void SpoolProfileLinksDialog::fill_shelf()
{
    m_scroll->DestroyChildren();
    const std::vector<SpoolLink> links = SpoolProfileMap::get().all_remembered();

    wxBoxSizer* shelf = new wxBoxSizer(wxVERTICAL);
    for (const auto& [material, group] : links_by_material(links))
        add_material_group(m_scroll, shelf, material, group);
    if (links.empty())
        add_empty_note(m_scroll, shelf);

    m_scroll->SetSizer(shelf);
    // The tiles are built after the dialog was themed, so they are themed here too: without this a
    // redrawn shelf comes back with black text on the dark background.
    wxGetApp().UpdateDarkUIWin(m_scroll);
    m_scroll->FitInside();
    m_scroll->Layout();
}

void SpoolProfileLinksDialog::add_material_group(wxWindow*                     parent,
                                                 wxSizer*                      shelf,
                                                 const std::string&            material,
                                                 const std::vector<SpoolLink>& links)
{
    const wxString name = material.empty() ? _L("Unknown material") : from_u8(material);
    shelf->Add(new Label(parent, Label::Head_14, name), 0, wxBOTTOM, parent->FromDIP(TILE_GAP / 2));

    wxFlexGridSizer* grid = make_tile_grid(parent);
    for (const SpoolLink& link : links)
        add_tile(parent, grid, link, from_u8(material));
    shelf->Add(grid, 0, wxBOTTOM, parent->FromDIP(TILE_GAP * 2));
}

void SpoolProfileLinksDialog::add_tile(wxWindow* parent, wxFlexGridSizer* grid, const SpoolLink& link, const wxString& material)
{
    // A link to a profile that is not installed any more is shown as it is: the profile may well
    // come back with its vendor.
    const bool     installed = wxGetApp().preset_bundle->filaments.find_preset(link.preset_name) != nullptr;
    const wxString profile   = from_u8(link.preset_name);
    // TRN: %1% is the name of a filament profile the printer's spool was linked to.
    const wxString subtitle = installed ? profile : format_wxstr(_L("%1% (profile not installed)"), profile);

    SpoolTile* tile = new SpoolTile(parent, tile_variants(link), link_title(link), subtitle, m_look, material);
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

void SpoolProfileLinksDialog::on_confirm()
{
    SpoolProfileMap::get().set_settings(m_settings);
    EndModal(wxID_OK);
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

    // Every color the filament has been seen in is a chip; the ones already on a profile of
    // their own also start as a row under it, so nothing the user set is hidden from them.
    for (const std::string& color : seen_colors(row.link)) {
        ColorRow entry;
        entry.color    = color;
        const auto own = row.link.color_presets.find(color);
        if (own != row.link.color_presets.end()) {
            entry.initial = own->second;
            entry.split   = true;
        }
        entry.preset = entry.initial.empty() ? row.link.preset_name : entry.initial;
        row.colors.push_back(std::move(entry));
    }
    if (row.colors.size() >= 2)
        return row;

    // A filament seen in one color only IS that color: there is nothing to tell apart from
    // the rest of it, so a profile saved for the color becomes the filament's own.
    for (ColorRow& color : row.colors) {
        if (!color.initial.empty()) {
            row.preset  = color.initial;
            row.initial = color.initial;
            row.filled  = true;
        }
        color.split = false;
    }
    return row;
}

void SpoolDetailDialog::build(wxWindow* parent)
{
    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);
    add_heading(parent, main_sizer,
                _L("Every color this spool was seen in. Click a color to put it on a profile of its own, or forget the link with the "
                   "button on the right of the spool's profile."),
                DETAIL_WIDTH);

    m_scroll = add_list_area(parent, main_sizer, DETAIL_WIDTH, list_height(1 + m_row.colors.size(), ROW_HEIGHT, VISIBLE_ROWS));
    rebuild_rows();

    add_ok_cancel(this, main_sizer, [this]() { on_confirm(); });
    SetSizerAndFit(main_sizer);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

// What every menu is showing right now, so the redrawn list carries the user's edits instead of
// what happens to be on disk.
void SpoolDetailDialog::take_menu_choices()
{
    if (m_row.combo != nullptr)
        m_row.preset = m_row.combo->selected_preset_name();
    m_row.combo = nullptr;
    for (ColorRow& color : m_row.colors) {
        if (color.combo != nullptr)
            color.preset = color.combo->selected_preset_name();
        color.combo = nullptr;
    }
}

// A color keeps a row of its own only while it prints on a different profile. Put back on the
// filament's own, it goes back in the line of chips. A color just clicked keeps its row until
// the user has had the chance to change it.
void SpoolDetailDialog::settle_color_rows()
{
    for (ColorRow& color : m_row.colors) {
        if (!color.split)
            continue;
        // Only a filament seen in several colors has one color to tell from the others.
        if (m_row.colors.size() < 2)
            color.split = false;
        else if (color.pending)
            color.pending = false;
        else if (color.filled && color.preset == m_row.preset)
            color.split = false;
    }
}

void SpoolDetailDialog::rebuild_rows()
{
    take_menu_choices();
    settle_color_rows();

    m_scroll->DestroyChildren();
    wxFlexGridSizer* grid = make_list_grid(this, 4);
    add_row(m_scroll, grid);
    if (!m_row.collapsed)
        add_color_rows(m_scroll, grid);

    m_scroll->SetSizer(grid);
    // The rows are built after the dialog was themed, so they are themed here too: without this a
    // redrawn list comes back with black text on the dark background.
    wxGetApp().UpdateDarkUIWin(m_scroll);
    m_scroll->FitInside();
    m_scroll->Layout();
    Layout();
}

wxWindow* SpoolDetailDialog::make_row_head(wxWindow* parent)
{
    const bool has_split = std::any_of(m_row.colors.begin(), m_row.colors.end(),
                                       [](const ColorRow& color) { return color.split && !color.removed; });

    wxPanel*    head       = new wxPanel(parent, wxID_ANY);
    wxBoxSizer* head_sizer = new wxBoxSizer(wxHORIZONTAL);
    head->SetBackgroundColour(parent->GetBackgroundColour());

    // The fold handle, and the same room kept empty for a filament with nothing folded under it, so
    // every name in the list still starts at the same place.
    if (has_split) {
        TreeToggle* toggle = new TreeToggle(head, wxSize(FromDIP(TOGGLE_WIDTH), FromDIP(SWATCH_SIZE)), !m_row.collapsed);
        toggle->SetToolTip(m_row.collapsed ? _L("Show the colors on their own profile") : _L("Hide the colors"));
        toggle->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
            m_row.collapsed = !m_row.collapsed;
            CallAfter(&SpoolDetailDialog::rebuild_rows);
        });
        head_sizer->Add(toggle, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    } else {
        head_sizer->AddSpacer(FromDIP(TOGGLE_WIDTH + 4));
    }

    add_chips(head, head_sizer);
    head->SetSizerAndFit(head_sizer);
    return head;
}

// Every color this filament has been seen in. A link saved before colors were kept shows the
// empty chip until the next sync fills it in.
void SpoolDetailDialog::add_chips(wxWindow* head, wxBoxSizer* head_sizer)
{
    const wxSize chip_size(FromDIP(SWATCH_SIZE), FromDIP(SWATCH_SIZE + MARK_HEIGHT));
    wxGridSizer* chips = new wxGridSizer(0, CHIPS_PER_LINE, FromDIP(4), FromDIP(4));
    if (m_row.colors.empty())
        chips->Add(new ColorSwatch(head, std::string(), chip_size), 0);

    for (ColorRow& color : m_row.colors) {
        ColorSwatch* chip = new ColorSwatch(head, color.color, chip_size);
        // Folded away, the mark under a chip is what says this color is not on the filament's
        // own profile.
        chip->set_marked(color.split && !color.removed && color.preset != m_row.preset);
        chips->Add(chip, 0);
        // One color is the whole filament, so there is nothing to give a profile of its own to
        // and the chip is there to look at, not to click.
        if (m_row.colors.size() < 2) {
            chip->SetToolTip(from_u8(color.color));
            continue;
        }
        // TRN: %1% is a color as #RRGGBB, or several of them for a multicolor filament.
        chip->SetToolTip(format_wxstr(_L("%1%\nClick to put this color on a profile of its own"), color.color));
        chip->SetCursor(wxCursor(wxCURSOR_HAND));
        chip->Bind(wxEVT_LEFT_DOWN, [this, &color](wxMouseEvent&) { split_color(color); });
    }
    head_sizer->Add(chips, 0, wxALIGN_CENTER_VERTICAL);
}

void SpoolDetailDialog::add_row(wxWindow* parent, wxFlexGridSizer* grid)
{
    grid->Add(make_row_head(parent), 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(4));

    add_title(parent, grid, Label::Body_14, link_title(m_row.link));

    m_row.combo = new SpoolPresetComboBox(parent, wxSize(FromDIP(COMBO_WIDTH), -1));
    grid->Add(m_row.combo, 1, wxALIGN_CENTER_VERTICAL | wxEXPAND);
    m_row.combo->fill_for(m_candidates, spool_identity_from_key(m_row.link.key), true, m_row.filled ? m_row.preset : m_row.link.preset_name);
    m_row.preset = m_row.combo->selected_preset_name();
    if (!m_row.filled) {
        m_row.initial = m_row.preset;
        m_row.filled  = true;
    }
    // Which colors count as variations, and the marks under the chips, both hang off this profile.
    m_row.combo->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent& evt) {
        evt.Skip();
        CallAfter(&SpoolDetailDialog::rebuild_rows);
    });

    add_delete_button(parent, grid, _L("Forget this link"))->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { forget_link(); });
}

void SpoolDetailDialog::add_color_rows(wxWindow* parent, wxFlexGridSizer* grid)
{
    size_t last = m_row.colors.size();
    for (size_t index = 0; index < m_row.colors.size(); ++index)
        if (m_row.colors[index].split && !m_row.colors[index].removed)
            last = index;
    for (size_t index = 0; index < m_row.colors.size(); ++index)
        if (m_row.colors[index].split && !m_row.colors[index].removed)
            add_color_row(parent, grid, m_row.colors[index], index == last);
}

void SpoolDetailDialog::add_color_row(wxWindow* parent, wxFlexGridSizer* grid, ColorRow& color, bool last)
{
    grid->Add(make_color_head(parent, color.color, last), 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(4));
    add_title(parent, grid, Label::Body_13, from_u8(color.color));

    color.combo = new SpoolPresetComboBox(parent, wxSize(FromDIP(COMBO_WIDTH), -1));
    grid->Add(color.combo, 1, wxALIGN_CENTER_VERTICAL | wxEXPAND);
    // A color with no profile of its own yet starts on the filament's, so the menu opens on what
    // this color prints with today.
    color.combo->fill_for(m_candidates, spool_identity_from_key(m_row.link.key), true, color.preset.empty() ? m_row.preset : color.preset);
    color.preset = color.combo->selected_preset_name();
    color.filled = true;
    // Picking the filament's own profile here is the user saying this color is no longer a
    // variation of it, so the row goes away and the color goes back in the line of chips.
    color.combo->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent& evt) {
        evt.Skip();
        CallAfter(&SpoolDetailDialog::rebuild_rows);
    });

    add_delete_button(parent, grid, _L("Put this color back on the filament's own profile"))
        ->Bind(wxEVT_BUTTON, [this, &color](wxCommandEvent&) {
            color.removed = true;
            CallAfter(&SpoolDetailDialog::rebuild_rows);
        });
}

void SpoolDetailDialog::split_color(ColorRow& color)
{
    // A filament that has only ever been seen in one color has nothing to split off.
    if (m_row.colors.size() < 2)
        return;
    // Clicking a color that already has a row of its own just brings that row back into view.
    color.split     = true;
    color.pending   = true;
    color.removed   = false;
    m_row.collapsed = false;
    CallAfter(&SpoolDetailDialog::rebuild_rows);
}

void SpoolDetailDialog::forget_link()
{
    SpoolProfileMap::get().remember(m_row.link.key, std::string());
    EndModal(wxID_OK);
}

void SpoolDetailDialog::remember_colors(const std::string& picked) const
{
    for (const ColorRow& color : m_row.colors) {
        if (!color.filled)
            continue;
        const std::string own = color.combo != nullptr ? color.combo->selected_preset_name() : color.preset;
        // Removed, folded back onto the filament's own profile, or never moved off it: this
        // color is not a variation and is stored as none.
        const bool variation = !color.removed && color.split && !own.empty() && own != picked;
        SpoolProfileMap::get().remember_color(m_row.link.key, color.color, variation ? own : std::string());
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
