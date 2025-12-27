#include "RecenterDialog.hpp"
#include "GUI_App.hpp"
#include <slic3r/GUI/I18N.hpp>
#include <wx/dcgraph.h>
#include <wx/dcmemory.h>
#include <slic3r/GUI/Widgets/Label.hpp>
#include <slic3r/GUI/Widgets/DialogButtons.hpp>

#define BORDER FromDIP(25)
#define DRAW_PANEL_SIZE wxSize(FromDIP(475), FromDIP(100))

const wxColour text_color(107, 107, 107);

namespace Slic3r { namespace GUI {
RecenterDialog::RecenterDialog(wxWindow* parent, wxWindowID id, const wxString& title, const wxPoint& pos, const wxSize& size, long style)
      : DPIDialog(parent, id, _L("Confirm"), pos, size, style)
{
    hint1 = _L("Please home all axes (click ");
    hint2 = _L(") to locate the toolhead's position. This prevents device moving beyond the printable boundary and causing equipment wear.");

    init_bitmap();

    SetBackgroundColour(*wxWHITE);

    auto* main_sizer = new wxBoxSizer(wxVERTICAL);
    auto* button_sizer = new wxBoxSizer(wxHORIZONTAL);

    wxPanel* m_line_top = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1), wxTAB_TRAVERSAL);
    m_line_top->SetBackgroundColour(wxColour(166, 169, 170));

    auto dlg_btns = new DialogButtons(this, {"OK", "Cancel"});
    dlg_btns->GetOK()->SetLabel(_L("Go Home"));
    dlg_btns->GetCANCEL()->SetLabel(_L("Close"));

    main_sizer->Add(m_line_top, 0, wxEXPAND, 0);
    main_sizer->AddSpacer(DRAW_PANEL_SIZE.y);
    main_sizer->Add(dlg_btns, 0, wxBOTTOM | wxRIGHT | wxEXPAND, FromDIP(ButtonProps::ChoiceButtonGap()));

    SetSizer(main_sizer);

    CenterOnParent();

    this->SetSize(wxSize(DRAW_PANEL_SIZE.x, -1));
    this->SetMinSize(wxSize(DRAW_PANEL_SIZE.x, -1));
    Layout();
    Fit();
    this->Bind(wxEVT_PAINT, &RecenterDialog::OnPaint, this);
    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, &RecenterDialog::on_button_confirm, this);
    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, &RecenterDialog::on_button_close, this);

    wxGetApp().UpdateDlgDarkUI(this);
}

RecenterDialog::~RecenterDialog() {}

void RecenterDialog::init_bitmap() {
    m_home_bmp = ScalableBitmap(this, "monitor_axis_home_icon", 24);
}

void RecenterDialog::OnPaint(wxPaintEvent& event){
    wxPaintDC dc(this);
    render(dc);
}
  
void RecenterDialog::render(wxDC& dc) {
    wxSize     size = GetSize();

    dc.SetFont(Label::Body_14);
    dc.SetTextForeground(text_color);
    wxPoint pos_start = wxPoint(BORDER, BORDER);

    wxSize hint1_size = dc.GetTextExtent(hint1);
    wxPoint pos_hint1 = pos_start;
    pos_hint1.y += (m_home_bmp.GetBmpWidth() - hint1_size.y) / 2;
    dc.DrawText(hint1, pos_hint1);

    wxPoint pos_bmp = pos_start;
    pos_bmp.x += hint1_size.x;
    dc.DrawBitmap(m_home_bmp.bmp(), pos_bmp);

    wxSize hint2_size = dc.GetTextExtent(hint2);
    wxPoint pos_hint2 = pos_hint1;
    pos_hint2.x = pos_hint2.x + hint1_size.x + m_home_bmp.GetBmpWidth();

    if (hint2_size.x + pos_hint2.x + BORDER > DRAW_PANEL_SIZE.x) {
        bool is_ch = false;
        if (hint2[0] > 0x80 && hint2[1] > 0x80) 
            is_ch = true;

        wxString fisrt_line;
        wxString remaining_line;

        wxString  count_txt;
        int new_line_pos = 0;
        for (int i = 0; i < hint2.length(); i++) {
            count_txt += hint2[i];
            auto text_size = dc.GetTextExtent(count_txt);
            if (text_size.x + pos_hint2.x + BORDER < DRAW_PANEL_SIZE.x)
            {
                if (hint2[i] == ' ' ||  hint2[i] == '\n')
                    new_line_pos = i;
            }
            else {
                if (!is_ch) {
                    fisrt_line = hint2.SubString(0, new_line_pos);
                    remaining_line = hint2.SubString(new_line_pos + 1, hint2.length());
                    break;
                }
                else {
                    fisrt_line = hint2.SubString(0, i - 1);
                    remaining_line = hint2.SubString(i, hint2.length());
                    break;
                }
                count_txt = "";
            }
        }
        dc.DrawText(fisrt_line, pos_hint2);


        count_txt = "";
        new_line_pos = 0;
        for (int i = 0; i < remaining_line.length(); i++) {
            count_txt += remaining_line[i];
            auto text_size = dc.GetTextExtent(count_txt);
            if (text_size.x + BORDER + BORDER < DRAW_PANEL_SIZE.x) 
            {
                if (remaining_line[i] == ' ' || remaining_line[i] == '\n')
                    new_line_pos = i;
            }
            else {
                if (!is_ch){
                    remaining_line[new_line_pos] = '\n';
                }
                else {
                    remaining_line.insert(i, '\n');
                }
                count_txt = "";
            }
        }
        wxPoint pos_txt = pos_hint1;
        pos_txt.y += dc.GetCharHeight();
        dc.DrawText(remaining_line, pos_txt);
    }
    else
        dc.DrawText(hint2, pos_hint2);
}

void RecenterDialog::on_button_confirm(wxCommandEvent& event) {
    if (this->IsModal())
        this->EndModal(wxID_OK);
    else
        this->Close();
}

void RecenterDialog::on_button_close(wxCommandEvent& event) {
    this->Close();
}

void RecenterDialog::on_dpi_changed(const wxRect& suggested_rect) {
    init_bitmap();
    // ORCA no need to reapply button sizes
    Layout();
}

}} // namespace Slic3r::GUI