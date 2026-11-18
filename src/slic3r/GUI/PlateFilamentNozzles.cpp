#include "PlateFilamentNozzles.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"
#include "format.hpp"
#include "wxExtensions.hpp"

#include "libslic3r/CommonDefs.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/FilamentCompaction.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <wx/defs.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/string.h>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {
namespace GUI {

PlateFilamentNozzlesPanel::PlateFilamentNozzlesPanel(wxWindow* parent, std::function<void()> on_rebuilt)
    : wxPanel(parent, wxID_ANY), m_on_rebuilt(std::move(on_rebuilt))
{
    SetBackgroundColour(parent->GetBackgroundColour());
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    m_grid      = new wxFlexGridSizer(4, FromDIP(6), FromDIP(8));
    m_grid->AddGrowableCol(1, 1);
    m_grid->AddGrowableCol(2, 1);
    sizer->Add(m_grid, 0, wxEXPAND);

    auto* reset = new Button(this, _L("Reset to automatic"));
    reset->SetStyle(ButtonStyle::Regular, ButtonType::Parameter);
    reset->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { this->reset(); });
    m_reset = reset;
    sizer->Add(m_reset, 0, wxALIGN_RIGHT | wxTOP, FromDIP(6));
    SetSizer(sizer);
}

std::vector<PlateFilamentNozzlesPanel::Row> PlateFilamentNozzlesPanel::rows_of(const PartPlate& plate) const
{
    std::vector<Row> rows;
    const Print* print = const_cast<PartPlate&>(plate).fff_print();
    if (print == nullptr)
        return rows;
    const FilamentCompaction& compaction = print->filament_compaction();
    // The Print's resolution dates from its last apply; a manual map set since then is what the
    // next apply will pin, so show it as soon as it is set.
    const std::vector<int> manual = plate.get_filament_map_mode() == FilamentMapMode::fmmManual ? plate.get_filament_maps() : std::vector<int>();
    for (int slot : compaction.used_slots) {
        int tool = compaction.slot_of_tool.empty() ? slot : compaction.tool_of_slot(slot);
        if (size_t(slot) < manual.size() && manual[slot] >= 1)
            tool = manual[slot] - 1;
        if (tool >= 0)
            rows.push_back({slot, tool});
    }
    return rows;
}

PartPlate* PlateFilamentNozzlesPanel::live_plate() const
{
    if (m_plate == nullptr)
        return nullptr;
    PartPlateList& plates = wxGetApp().plater()->get_partplate_list();
    for (int i = 0; i < plates.get_plate_count(); ++i)
        if (plates.get_plate(i) == m_plate)
            return m_plate;
    return nullptr;
}

void PlateFilamentNozzlesPanel::update(PartPlate* plate)
{
    m_plate = plate;
    if (m_rebuild_pending)
        return;
    m_rebuild_pending = true;
    // Deferred: see the class comment. Pending calls die with the panel (wxEvtHandler drops them).
    CallAfter([this] {
        m_rebuild_pending = false;
        rebuild();
        if (m_on_rebuilt)
            m_on_rebuilt();
    });
}

void PlateFilamentNozzlesPanel::rebuild()
{
    PartPlate* plate = live_plate();
    m_plate          = plate;
    PresetBundle& bundle = *wxGetApp().preset_bundle;
    const DynamicPrintConfig& printer_config = bundle.printers.get_edited_preset().config;
    const std::vector<Row> rows = plate != nullptr && decouples_filaments_from_tools(printer_config) ? rows_of(*plate) : std::vector<Row>();

    Freeze();
    m_grid->Clear(true);
    if (rows.empty()) {
        Thaw();
        Show(false);
        return;
    }

    const auto* nozzles = printer_config.option<ConfigOptionFloats>("nozzle_diameter");
    const auto* types   = printer_config.option<ConfigOptionEnumsGenericNullable>("nozzle_type");
    const size_t tool_count = nozzles == nullptr ? 0 : nozzles->size();
    const ConfigOptionDef* type_def = print_config_def.get("nozzle_type");
    const auto tool_label = [&](size_t tool) {
        if (types != nullptr && tool < types->size() && !types->is_nil(tool)) {
            const int type = types->values[tool];
            if (type != int(NozzleType::ntUndefine) && type_def != nullptr && size_t(type) < type_def->enum_labels.size())
                // TRN: a tool and its nozzle, e.g. "T3: 0.6 mm, Hardened steel"
                return format_wxstr(_L("T%1%: %2% mm, %3%"), tool + 1, nozzles->values[tool], _(type_def->enum_labels[size_t(type)]));
        }
        // TRN: a tool and its nozzle, e.g. "T3: 0.6 mm"
        return format_wxstr(_L("T%1%: %2% mm"), tool + 1, nozzles->values[tool]);
    };

    const auto* colours = bundle.project_config.option<ConfigOptionStrings>("filament_colour");
    const DynamicPrintConfig full_config  = bundle.full_config(false);
    const auto*              required_hrc = full_config.option<ConfigOptionInts>("required_nozzle_HRC");
    for (const Row& row : rows) {
        const std::string colour = colours != nullptr && size_t(row.slot) < colours->size() ? colours->values[row.slot] : std::string("#FFFFFF");
        auto* swatch = new wxStaticBitmap(this, wxID_ANY, *get_extruder_color_icon(colour, std::to_string(row.slot + 1), FromDIP(20), FromDIP(20)));
        m_grid->Add(swatch, 0, wxALIGN_CENTER_VERTICAL);

        const wxString name = size_t(row.slot) < bundle.filament_presets.size() ? from_u8(bundle.filament_presets[row.slot]) : wxString();
        auto* label = new Label(this, Label::Body_13, name, wxST_ELLIPSIZE_END);
        label->SetForegroundColour(StateColor::darkModeColorFor("#262E30"));
        m_grid->Add(label, 1, wxALIGN_CENTER_VERTICAL | wxEXPAND);

        auto* combo = new ::ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(220), -1), 0, nullptr, wxCB_READONLY);
        for (size_t tool = 0; tool < tool_count; ++tool)
            combo->Append(tool_label(tool));
        combo->SetSelection(row.tool);
        combo->Bind(wxEVT_COMBOBOX, [this, slot = row.slot](wxCommandEvent& e) {
            this->assign(slot, e.GetSelection());
            e.Skip();
        });
        m_grid->Add(combo, 1, wxALIGN_CENTER_VERTICAL | wxEXPAND);

        // Softer filament than the nozzle is fine; a harder one is warned about, as the slicer does.
        wxString warning;
        if (required_hrc != nullptr && size_t(row.slot) < required_hrc->size() && types != nullptr && size_t(row.tool) < types->size() &&
            !types->is_nil(size_t(row.tool))) {
            const int  required = required_hrc->values[row.slot];
            const auto type     = NozzleType(types->values[size_t(row.tool)]);
            if (required > 0 && type != NozzleType::ntUndefine && Print::get_hrc_by_nozzle_type(type) < required)
                warning = format_wxstr(_L("This filament needs a nozzle of %1% HRC or harder."), required);
        }
        auto* icon = new wxStaticBitmap(this, wxID_ANY, create_scaled_bitmap("obj_warning", this, 16));
        icon->SetToolTip(warning);
        icon->Show(!warning.empty());
        m_grid->Add(icon, 0, wxALIGN_CENTER_VERTICAL);
    }
    m_reset->Show(plate->get_filament_map_mode() == FilamentMapMode::fmmManual);
    Show(true);
    Layout();
    Thaw();
    if (wxWindow* top = GetParent())
        top->Layout();
}

void PlateFilamentNozzlesPanel::rescale()
{
    update(m_plate);
}

void PlateFilamentNozzlesPanel::assign(int slot, int tool)
{
    Plater*    plater = wxGetApp().plater();
    PartPlate* plate  = live_plate();
    if (plate == nullptr)
        return;
    const std::vector<Row> rows = rows_of(*plate);
    auto chosen = std::find_if(rows.begin(), rows.end(), [slot](const Row& r) { return r.slot == slot; });
    if (chosen == rows.end() || chosen->tool == tool)
        return;

    // The full resolution, 1-based, with the change applied; the filament holding the tool swaps.
    const size_t filament_count = wxGetApp().preset_bundle->filament_presets.size();
    std::vector<int> map = plate->get_filament_maps();
    if (map.size() != filament_count)
        map.assign(filament_count, 1);
    for (const Row& row : rows)
        if (size_t(row.slot) < map.size())
            map[row.slot] = row.tool + 1;
    for (const Row& row : rows)
        if (row.tool == tool && size_t(row.slot) < map.size())
            map[row.slot] = chosen->tool + 1;
    if (size_t(slot) < map.size())
        map[slot] = tool + 1;

    plater->take_snapshot(std::string("Change filament tool"));
    plate->set_filament_map_mode(FilamentMapMode::fmmManual);
    plate->set_filament_maps(map);
    plate->update_slice_result_valid_state(false);
    plater->set_plater_dirty(true);
    plater->update();
    update(plate);
}

void PlateFilamentNozzlesPanel::reset()
{
    Plater*    plater = wxGetApp().plater();
    PartPlate* plate  = live_plate();
    if (plate == nullptr || plate->get_filament_map_mode() != FilamentMapMode::fmmManual)
        return;
    plater->take_snapshot(std::string("Reset filament tools"));
    plate->set_filament_map_mode(FilamentMapMode::fmmAutoForFlush);
    plate->clear_filament_map();
    plate->update_slice_result_valid_state(false);
    plater->set_plater_dirty(true);
    plater->update();
    update(plate);
}

} // namespace GUI
} // namespace Slic3r
