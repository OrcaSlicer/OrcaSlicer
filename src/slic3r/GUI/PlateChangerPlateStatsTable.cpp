#include "PlateChangerPlateStatsTable.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "PartPlate.hpp"
#include "wxExtensions.hpp"
#include "Widgets/Label.hpp"

#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintBase.hpp"
#include "libslic3r/Utils.hpp"

#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>

namespace Slic3r { namespace GUI {

void populate_plate_changer_time_weight_grid(wxFlexGridSizer* grid_sizer,
                                             wxWindow*        table_panel,
                                             wxWindow*        icon_bitmap_parent,
                                             PartPlateList&   partplate_list)
{
    if (!grid_sizer || !table_panel || !icon_bitmap_parent)
        return;

    const bool use_inches = wxGetApp().app_config->get("use_inches") == "1";

    const int           n_plates = partplate_list.get_plate_count();
    std::vector<double> plate_times(n_plates, 0.0);
    std::vector<double> plate_weights(n_plates, 0.0);
    double              total_time_s   = 0.0;
    double              total_weight_g = 0.0;

    for (int i = 0; i < n_plates; ++i) {
        PartPlate* p = partplate_list.get_plate(i);
        if (p && p->get_slice_result())
            plate_times[i] =
                p->get_slice_result()->print_statistics.modes[static_cast<size_t>(Slic3r::PrintEstimatedStatistics::ETimeMode::Normal)].time;
        PrintBase* pbase = nullptr;
        if (p)
            p->get_print(&pbase, nullptr, nullptr);
        if (Slic3r::Print* print = dynamic_cast<Slic3r::Print*>(pbase))
            plate_weights[i] = print->print_statistics().total_weight;
        total_time_s += plate_times[i];
        total_weight_g += plate_weights[i];
    }

    grid_sizer->Clear(true);

    const wxColour table_text_colour(0x98, 0x98, 0x98);
    const wxColour table_header_colour(0xBC, 0xBC, 0xBC);
    const wxColour table_bg = table_panel->GetBackgroundColour();
    const wxColour table_gridline_colour(0x40, 0x40, 0x40);
    wxFont         header_font(Label::Body_13);
    header_font.MakeSmaller();
    header_font.MakeBold();

    auto make_line_cell = [table_panel, table_gridline_colour]() {
        wxPanel* cell = new wxPanel(table_panel, wxID_ANY, wxDefaultPosition, wxSize(-1, table_panel->FromDIP(1)), wxBORDER_NONE);
        cell->SetBackgroundColour(table_gridline_colour);
        cell->SetMinSize(wxSize(-1, table_panel->FromDIP(1)));
        return cell;
    };

    auto make_cell = [table_panel, table_bg, icon_bitmap_parent](const wxString& text, const wxFont* font, const wxColour& text_colour,
                                                                 const char* icon_name = nullptr) {
        wxPanel*    cell       = new wxPanel(table_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
        wxBoxSizer* cell_sizer = new wxBoxSizer(wxHORIZONTAL);
        cell->SetBackgroundColour(table_bg);
        if (icon_name) {
            wxStaticBitmap* icon = new wxStaticBitmap(cell, wxID_ANY, create_scaled_bitmap(icon_name, icon_bitmap_parent, 18),
                                                      wxDefaultPosition, wxSize(table_panel->FromDIP(18), table_panel->FromDIP(18)), 0);
            cell_sizer->Add(icon, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, table_panel->FromDIP(4));
        }
        wxStaticText* st = new wxStaticText(cell, wxID_ANY, text, wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
        st->SetForegroundColour(text_colour);
        st->SetBackgroundColour(table_bg);
        if (font)
            st->SetFont(*font);
        cell_sizer->Add(st, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, table_panel->FromDIP(4));
        cell->SetSizer(cell_sizer);
        return cell;
    };

    grid_sizer->Add(make_cell(_L("Plate"), &header_font, table_header_colour, nullptr), 0, wxEXPAND | wxALL, 0);
    grid_sizer->Add(make_cell(_L("Time"), &header_font, table_header_colour, "print-time"), 0, wxEXPAND | wxALL, 0);
    grid_sizer->Add(make_cell(_L("Weight"), &header_font, table_header_colour, "print-weight"), 0, wxEXPAND | wxALL, 0);

    auto add_line_row = [grid_sizer, make_line_cell]() {
        grid_sizer->Add(make_line_cell(), 0, wxEXPAND | wxALL, 0);
        grid_sizer->Add(make_line_cell(), 0, wxEXPAND | wxALL, 0);
        grid_sizer->Add(make_line_cell(), 0, wxEXPAND | wxALL, 0);
    };

    add_line_row();

    auto add_row = [grid_sizer, make_cell, use_inches, table_text_colour, table_header_colour](const wxString& plate_label, double time_s,
                                                                                               double weight_g, bool highlight) {
        const wxColour& row_colour = highlight ? table_header_colour : table_text_colour;
        grid_sizer->Add(make_cell(plate_label, nullptr, row_colour), 0, wxEXPAND | wxALL, 0);
        grid_sizer->Add(make_cell(wxString::Format("%s", short_time(get_time_dhms(static_cast<float>(time_s))).c_str()), nullptr,
                                  row_colour),
                        0, wxEXPAND | wxALL, 0);
        wxString wstr = use_inches ? wxString::Format("%.2f oz", weight_g * 0.035274) : wxString::Format("%.2f g", weight_g);
        grid_sizer->Add(make_cell(wstr, nullptr, row_colour), 0, wxEXPAND | wxALL, 0);
    };

    add_row(_L("All"), total_time_s, total_weight_g, true);
    add_line_row();
    for (int i = 0; i < n_plates; ++i) {
        add_row(wxString::Format("%d", i + 1), plate_times[i], plate_weights[i], false);
        add_line_row();
    }

    table_panel->Layout();
}

}} // namespace Slic3r::GUI
