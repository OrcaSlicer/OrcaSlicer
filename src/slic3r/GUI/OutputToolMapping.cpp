#include "OutputToolMapping.hpp"

#include "GUI_App.hpp"
#include "GUI_Utils.hpp"
#include "I18N.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"

#include "libslic3r/PresetBundle.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <vector>

#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <wx/arrstr.h>
#include <wx/button.h>
#include <wx/choice.h>
#include <wx/colour.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

namespace Slic3r {
namespace GUI {
namespace {

struct OutputToolFilament
{
    int      logical_tool = 0;
    wxString name;
    wxColour color;
};

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static wxColour parse_filament_color(const std::string& color)
{
    if (color.size() < 7 || color[0] != '#')
        return wxColour(38, 166, 154);

    int values[4] = {38, 166, 154, 255};
    for (int channel = 0; channel < 4; ++channel) {
        const size_t pos = 1 + size_t(channel) * 2;
        if (pos + 1 >= color.size())
            break;
        const int hi = hex_digit(color[pos]);
        const int lo = hex_digit(color[pos + 1]);
        if (hi < 0 || lo < 0)
            break;
        values[channel] = hi * 16 + lo;
    }

    return wxColour(values[0], values[1], values[2], values[3]);
}

static wxString filament_display_name(size_t logical_tool)
{
    auto* preset_bundle = wxGetApp().preset_bundle;
    if (preset_bundle == nullptr)
        return wxString::Format(_L("Filament %d"), int(logical_tool + 1));

    if (logical_tool < preset_bundle->filament_presets.size()) {
        const std::string& filament_name = preset_bundle->filament_presets[logical_tool];
        Preset* preset = preset_bundle->filaments.find_preset(filament_name);
        if (preset != nullptr) {
            std::string display_filament_type;
            preset->config.get_filament_type(display_filament_type);
            if (!display_filament_type.empty())
                return wxString::FromUTF8(display_filament_type.c_str());
            if (!preset->name.empty())
                return wxString::FromUTF8(preset->name.c_str());
        }
    }

    return wxString::Format(_L("Filament %d"), int(logical_tool + 1));
}

static PartPlate* plate_for_mapping(Plater* plater, int plate_idx)
{
    if (plater == nullptr)
        return nullptr;

    PartPlateList& plate_list = plater->get_partplate_list();
    if (plate_idx >= 0) {
        if (PartPlate* plate = plate_list.get_plate(plate_idx))
            return plate;
    }

    return plate_list.get_curr_plate();
}

static std::vector<OutputToolFilament> collect_output_tool_filaments(Plater* plater, int plate_idx)
{
    std::vector<OutputToolFilament> result;
    PartPlate* plate = plate_for_mapping(plater, plate_idx);
    if (plate == nullptr)
        return result;

    std::vector<int> extruders = plate->get_extruders(true);

    std::sort(extruders.begin(), extruders.end());
    extruders.erase(std::unique(extruders.begin(), extruders.end()), extruders.end());

    std::vector<std::string> colors;
    if (plater != nullptr)
        colors = plater->get_extruder_colors_from_plater_config(plate->get_slice_result());

    for (int extruder_id : extruders) {
        const int logical_tool = extruder_id - 1;
        if (logical_tool < 0)
            continue;

        OutputToolFilament filament;
        filament.logical_tool = logical_tool;
        filament.name         = filament_display_name(size_t(logical_tool));
        filament.color        = logical_tool < int(colors.size()) ? parse_filament_color(colors[size_t(logical_tool)]) :
                                                                     wxColour(38, 166, 154);
        result.emplace_back(std::move(filament));
    }

    return result;
}

static int physical_tool_count()
{
    return std::max(6, wxGetApp().filaments_cnt());
}

class OutputToolMappingDialog : public DPIDialog
{
public:
    OutputToolMappingDialog(wxWindow* parent, const std::vector<OutputToolFilament>& filaments, int tool_count)
        : DPIDialog(parent, wxID_ANY, _L("Select output tools"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
        , m_filaments(filaments)
        , m_tool_count(std::max(1, tool_count))
    {
        build();
    }

    std::map<int, int> mapping() const
    {
        std::map<int, int> result;
        for (size_t i = 0; i < m_choices.size() && i < m_filaments.size(); ++i)
            result[m_filaments[i].logical_tool] = m_choices[i]->GetSelection();
        return result;
    }

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override
    {
        Fit();
        Refresh();
    }

    void on_sys_color_changed() override
    {
        Refresh();
    }

private:
    void build()
    {
        SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));

        wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);
        main_sizer->AddSpacer(FromDIP(18));

        wxStaticText* title = new wxStaticText(this, wxID_ANY, _L("Output tool mapping"));
        wxFont title_font = title->GetFont();
        title_font.SetWeight(wxFONTWEIGHT_BOLD);
        title->SetFont(title_font);
        main_sizer->Add(title, 0, wxLEFT | wxRIGHT | wxEXPAND, FromDIP(22));

        wxStaticText* desc = new wxStaticText(this, wxID_ANY, _L("Choose the physical tool used by each sliced filament before sending the print."));
        desc->Wrap(FromDIP(470));
        main_sizer->Add(desc, 0, wxLEFT | wxRIGHT | wxTOP | wxEXPAND, FromDIP(22));
        main_sizer->AddSpacer(FromDIP(14));

        wxScrolledWindow* scroller = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxBORDER_SIMPLE);
        scroller->SetScrollRate(0, FromDIP(8));
        scroller->SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));

        wxBoxSizer* rows_sizer = new wxBoxSizer(wxVERTICAL);
        for (const OutputToolFilament& filament : m_filaments)
            rows_sizer->Add(create_row(scroller, filament), 0, wxEXPAND | wxALL, FromDIP(4));

        scroller->SetSizer(rows_sizer);
        scroller->SetMinSize(wxSize(FromDIP(510), FromDIP(std::min<int>(320, 58 * int(m_filaments.size()) + 10))));
        main_sizer->Add(scroller, 1, wxLEFT | wxRIGHT | wxEXPAND, FromDIP(22));

        wxBoxSizer* buttons = new wxBoxSizer(wxHORIZONTAL);
        wxButton* cancel = new wxButton(this, wxID_CANCEL, _L("Cancel"));
        wxButton* ok     = new wxButton(this, wxID_OK, _L("OK"));
        buttons->AddStretchSpacer(1);
        buttons->Add(cancel, 0, wxRIGHT, FromDIP(8));
        buttons->Add(ok, 0);
        main_sizer->Add(buttons, 0, wxLEFT | wxRIGHT | wxTOP | wxBOTTOM | wxEXPAND, FromDIP(22));

        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); }, wxID_CANCEL);
        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_OK); }, wxID_OK);

        SetSizer(main_sizer);
        SetMinSize(wxSize(FromDIP(560), FromDIP(300)));
        Fit();
        CentreOnParent();
    }

    wxWindow* create_row(wxWindow* parent, const OutputToolFilament& filament)
    {
        wxPanel* row = new wxPanel(parent, wxID_ANY);
        row->SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));

        wxBoxSizer* row_sizer = new wxBoxSizer(wxHORIZONTAL);

        wxPanel* swatch = new wxPanel(row, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(22), FromDIP(22)));
        swatch->SetMinSize(wxSize(FromDIP(22), FromDIP(22)));
        swatch->SetMaxSize(wxSize(FromDIP(22), FromDIP(22)));
        swatch->SetBackgroundColour(filament.color);
        row_sizer->Add(swatch, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));

        wxBoxSizer* text_sizer = new wxBoxSizer(wxVERTICAL);
        wxStaticText* logical = new wxStaticText(row, wxID_ANY, wxString::Format("T%d", filament.logical_tool));
        wxFont logical_font = logical->GetFont();
        logical_font.SetWeight(wxFONTWEIGHT_BOLD);
        logical->SetFont(logical_font);
        wxStaticText* name = new wxStaticText(row, wxID_ANY, filament.name);
        text_sizer->Add(logical, 0);
        text_sizer->Add(name, 0, wxTOP, FromDIP(2));
        row_sizer->Add(text_sizer, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(16));

        wxArrayString choices;
        for (int i = 0; i < m_tool_count; ++i)
            choices.Add(wxString::Format("T%d", i));

        wxChoice* choice = new wxChoice(row, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(95), wxDefaultCoord), choices);
        const int default_tool = filament.logical_tool >= 0 && filament.logical_tool < m_tool_count ? filament.logical_tool : 0;
        choice->SetSelection(default_tool);
        m_choices.push_back(choice);
        row_sizer->Add(choice, 0, wxALIGN_CENTER_VERTICAL);

        row->SetSizer(row_sizer);
        row->SetMinSize(wxSize(wxDefaultCoord, FromDIP(48)));
        return row;
    }

private:
    std::vector<OutputToolFilament> m_filaments;
    int                             m_tool_count = 6;
    std::vector<wxChoice*>          m_choices;
};

static std::string remap_command_part(const std::string& command, const std::map<int, int>& output_tool_mapping)
{
    std::string remapped = command;
    size_t i = 0;
    while (i < remapped.size()) {
        const bool token_start = i == 0 || std::isspace(static_cast<unsigned char>(remapped[i - 1])) != 0;
        if (token_start && (remapped[i] == 'T' || remapped[i] == 't') && i + 1 < remapped.size() &&
            std::isdigit(static_cast<unsigned char>(remapped[i + 1])) != 0) {
            size_t end = i + 2;
            while (end < remapped.size() && std::isdigit(static_cast<unsigned char>(remapped[end])) != 0)
                ++end;

            const int source_tool = std::stoi(remapped.substr(i + 1, end - i - 1));
            auto it = output_tool_mapping.find(source_tool);
            if (it != output_tool_mapping.end()) {
                const std::string target = std::to_string(it->second);
                remapped.replace(i + 1, end - i - 1, target);
                i += 1 + target.size();
            } else {
                i = end;
            }
            continue;
        }
        ++i;
    }
    return remapped;
}

static std::string remap_gcode_line(const std::string& line, const std::map<int, int>& output_tool_mapping)
{
    const size_t comment_pos = line.find(';');
    if (comment_pos == std::string::npos)
        return remap_command_part(line, output_tool_mapping);

    return remap_command_part(line.substr(0, comment_pos), output_tool_mapping) + line.substr(comment_pos);
}

} // namespace

bool has_non_identity_tool_mapping(const std::map<int, int>& output_tool_mapping)
{
    for (const auto& item : output_tool_mapping)
        if (item.first != item.second)
            return true;
    return false;
}

bool show_output_tool_mapping_dialog(wxWindow* parent, Plater* plater, int plate_idx, std::map<int, int>& output_tool_mapping)
{
    output_tool_mapping.clear();

    std::vector<OutputToolFilament> filaments = collect_output_tool_filaments(plater, plate_idx);
    if (filaments.empty())
        return true;

    OutputToolMappingDialog dialog(parent, filaments, physical_tool_count());
    if (dialog.ShowModal() != wxID_OK)
        return false;

    output_tool_mapping = dialog.mapping();
    return true;
}

bool remap_gcode_file_tools(const boost::filesystem::path& input_path,
                            const boost::filesystem::path& output_path,
                            const std::map<int, int>& output_tool_mapping,
                            std::string* error)
{
    boost::nowide::ifstream input(input_path.string(), std::ios::binary);
    if (!input) {
        if (error)
            *error = "failed to open input gcode";
        return false;
    }

    boost::nowide::ofstream output(output_path.string(), std::ios::binary | std::ios::trunc);
    if (!output) {
        if (error)
            *error = "failed to open output gcode";
        return false;
    }

    std::string line;
    while (std::getline(input, line)) {
        output << remap_gcode_line(line, output_tool_mapping);
        if (!input.eof())
            output << '\n';
    }

    if (!output) {
        if (error)
            *error = "failed to write output gcode";
        return false;
    }

    return true;
}

} // namespace GUI
} // namespace Slic3r
