// Standalone stub for "Send print job" plate-changer table layout.
// Placeholders for: project name, "Printing N plates.", table (Plate / Time / Weight).
// Build and run to iterate on layout without compiling OrcaSlicer or running a real slice.

#include <wx/wx.h>
#include <wx/frame.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

// DIP scale helper (wx 3.0–compatible; pass a window when available for scaling)
static int FromDIP(int px, const wxWindow* w = nullptr) {
#if wxCHECK_VERSION(3, 1, 1) && defined(__WXMSW__)
    if (w) return wxRound(px * (double)w->GetDPIScaleFactor());
#endif
    (void)w;
    return px;
}

class LayoutFrame : public wxFrame {
public:
    LayoutFrame()
        : wxFrame(nullptr, wxID_ANY, "Send print job (layout stub)", wxDefaultPosition, wxSize(520, 420)) {
        wxPanel* panel = new wxPanel(this, wxID_ANY);
        panel->SetBackgroundColour(wxColour(0x2D, 0x2D, 0x2D));

        wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);

        // Placeholder: thumbnail area (simplified as a fixed-size panel)
        wxPanel* thumb = new wxPanel(panel, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(256), FromDIP(256)));
        thumb->SetBackgroundColour(wxColour(0x3A, 0x3A, 0x3A));
        main_sizer->Add(thumb, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(10));

        main_sizer->AddSpacer(FromDIP(10));

        // Table area: left column (project name + plate count) | table
        wxPanel* table_panel = new wxPanel(panel, wxID_ANY);
        table_panel->SetBackgroundColour(panel->GetBackgroundColour());
        table_panel->SetMinSize(wxSize(FromDIP(480), -1));

        wxBoxSizer* table_hsizer = new wxBoxSizer(wxHORIZONTAL);

        wxBoxSizer* left_col = new wxBoxSizer(wxVERTICAL);
        wxStaticText* project_name = new wxStaticText(table_panel, wxID_ANY, "clip plates",
                                                     wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
        project_name->SetFont(project_name->GetFont().Bold());
        project_name->SetForegroundColour(wxColour(0x26, 0x2E, 0x30));
        left_col->Add(project_name, 0, wxBOTTOM, FromDIP(4, table_panel));

        wxStaticText* plate_count = new wxStaticText(table_panel, wxID_ANY, "Printing 3 plates.");
        plate_count->SetForegroundColour(wxColour(0x98, 0x98, 0x98));
        left_col->Add(plate_count, 0);

        table_hsizer->Add(left_col, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(20, table_panel));

        // Table: header + separator row + All + separator + plate rows
        const wxColour table_bg = table_panel->GetBackgroundColour();
        const wxColour table_text(0x98, 0x98, 0x98);
        const wxColour table_header(0xBC, 0xBC, 0xBC);
        const wxColour gridline(0x40, 0x40, 0x40);

        wxFont header_font(plate_count->GetFont());
        header_font.MakeSmaller();
        header_font.MakeBold();

        auto make_cell = [&](const wxString& text, const wxFont* font, const wxColour& colour) {
            wxPanel* cell = new wxPanel(table_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
            cell->SetBackgroundColour(table_bg);
            wxStaticText* st = new wxStaticText(cell, wxID_ANY, text, wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
            st->SetForegroundColour(colour);
            st->SetBackgroundColour(table_bg);
            if (font) st->SetFont(*font);
            wxBoxSizer* cs = new wxBoxSizer(wxHORIZONTAL);
            cs->Add(st, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(4, table_panel));
            cell->SetSizer(cs);
            return cell;
        };

        auto make_line_cell = [&]() {
            wxPanel* cell = new wxPanel(table_panel, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(1, table_panel)), wxBORDER_NONE);
            cell->SetBackgroundColour(gridline);
            cell->SetMinSize(wxSize(-1, FromDIP(1, table_panel)));
            return cell;
        };

        wxFlexGridSizer* grid = new wxFlexGridSizer(0, 3, FromDIP(1, table_panel), FromDIP(1, table_panel));

        grid->Add(make_cell("Plate", &header_font, table_header), 0, wxEXPAND | wxALL, 0);
        grid->Add(make_cell("Time", &header_font, table_header), 0, wxEXPAND | wxALL, 0);
        grid->Add(make_cell("Weight", &header_font, table_header), 0, wxEXPAND | wxALL, 0);

        for (int i = 0; i < 3; i++)
            grid->Add(make_line_cell(), 0, wxEXPAND | wxALL, 0);

        const char* rows[][3] = {
            {"All", "52m55s", "9.96 g"},
            {"1", "11m15s", "1.37 g"},
            {"2", "9m6s", "0.72 g"},
            {"3", "32m33s", "7.86 g"},
        };
        for (size_t r = 0; r < 4; r++) {
            bool highlight = (r == 0);
            const wxColour& row_colour = highlight ? table_header : table_text;
            grid->Add(make_cell(rows[r][0], nullptr, row_colour), 0, wxEXPAND | wxALL, 0);
            grid->Add(make_cell(rows[r][1], nullptr, row_colour), 0, wxEXPAND | wxALL, 0);
            grid->Add(make_cell(rows[r][2], nullptr, row_colour), 0, wxEXPAND | wxALL, 0);
            if (r < 3)
                for (int i = 0; i < 3; i++)
                    grid->Add(make_line_cell(), 0, wxEXPAND | wxALL, 0);
        }

        table_hsizer->Add(grid, 1, wxEXPAND);
        table_panel->SetSizer(table_hsizer);

        main_sizer->Add(table_panel, 0, wxALIGN_CENTER_HORIZONTAL);

        // Placeholder: printer row
        main_sizer->AddSpacer(FromDIP(12));
        wxStaticText* printer_label = new wxStaticText(panel, wxID_ANY, "Printer");
        printer_label->SetForegroundColour(wxColour(0x26, 0x2E, 0x30));
        main_sizer->Add(printer_label, 0, wxLEFT, FromDIP(5));
        main_sizer->AddSpacer(FromDIP(4));
        wxStaticText* printer_value = new wxStaticText(panel, wxID_ANY, "Betzalel II(LAN)  —  Bambu Lab A1 mini 0.4mm");
        printer_value->SetForegroundColour(wxColour(0x98, 0x98, 0x98));
        main_sizer->Add(printer_value, 0, wxLEFT, FromDIP(5));

        panel->SetSizer(main_sizer);
    }
};

class LayoutApp : public wxApp {
public:
    bool OnInit() override {
        (new LayoutFrame())->Show();
        return true;
    }
};

wxIMPLEMENT_APP(LayoutApp);
