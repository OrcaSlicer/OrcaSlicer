#include "ProgressBar.hpp"
#include "../I18N.hpp"
#include <wx/dcclient.h>
#include <wx/dcgraph.h>
#include "Label.hpp"



wxDEFINE_EVENT(wxCUSTOMEVT_SET_TEMP_FINISH, wxCommandEvent);
BEGIN_EVENT_TABLE(ProgressBar, wxWindow)
EVT_PAINT(ProgressBar::paintEvent)
END_EVENT_TABLE()

ProgressBar::ProgressBar(wxWindow *parent, wxWindowID id, int max, const wxPoint &pos, const wxSize &size, bool shown)
{
    m_shownumber = shown;
    SetBackgroundColour(wxColour(255,255,255));

    if (size.y >= miniHeight) {
        m_miniHeight = size.y;
    } else {
        m_miniHeight = miniHeight;
    }

    m_max = max;
    m_radius = m_miniHeight / 2;
    wxSize temp_size(size.x, m_miniHeight);

    SetFont(Label::Head_12);
    create(parent, id, pos, temp_size);
}


ProgressBar::~ProgressBar() {}


void ProgressBar::create(wxWindow *parent, wxWindowID id, const wxPoint &pos,  wxSize &size)
{
    wxWindow::Create(parent, id, pos, size);
    // m_static_info = new wxStaticText(this, wxID_ANY,wxT(""),wxPoint(this->padding, 20), wxSize(GetSize().GetWidth() - this->padding * 3, -1), wxST_ELLIPSIZE_END);
    // m_static_info->Wrap(-1);

   /* wxBoxSizer *m_sizer_body  = new wxBoxSizer(wxHORIZONTAL);

     auto m_progress_bk = new StaticBox(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
     m_progress_bk->SetBackgroundColour(wxColour(238, 130, 238));
     StateColor btn_bg_green(std::pair<wxColour, int>(wxColour(0, 137, 123), StateColor::Pressed), std::pair<wxColour, int>(wxColour(38, 166, 154), StateColor::Hovered),
                             std::pair<wxColour, int>(wxColour(0, 150, 136), StateColor::Normal));

     wxBoxSizer *m_sizer_progress= new wxBoxSizer(wxHORIZONTAL);

     auto m_progress = new wxPanel(m_progress_bk, wxID_ANY, wxDefaultPosition, wxSize(50, -1), wxTAB_TRAVERSAL);
     m_progress->SetBackgroundColour(wxColour(128, 0, 255));

     m_sizer_progress->Add(m_progress, 0, wxEXPAND, 0);

     m_progress_bk->SetSizer(m_sizer_progress);
     m_progress_bk->Layout();
     m_sizer_progress->Fit(m_progress_bk);
     m_sizer_body->Add(m_progress_bk, 1, wxEXPAND, 0);

     this->SetSizer(m_sizer_body);
     this->Layout();*/
}


void ProgressBar::SetRadius(double radius) {
    m_radius = radius;
    Refresh();
}

void ProgressBar::SetProgressForedColour(wxColour colour)
{
    m_progress_background_colour = colour;
    Refresh();
}

void ProgressBar::SetProgressBackgroundColour(wxColour colour)
{
    m_progress_colour = colour;
     Refresh();
}

void ProgressBar::Rescale()
{
    ;
}

void ProgressBar::ShowNumber(bool shown)
{
    m_shownumber = shown;
    Refresh();
}

void ProgressBar::Disable(wxString text)
{
    if (m_disable) return;
    m_disable_text = text;
    m_disable = true;
    Refresh();
}

void ProgressBar::SetValue(int step)
{
    m_disable = false;
    SetProgress(step);
}

void ProgressBar::Reset()
{
    m_step = 0;
    SetValue(0);
}

void ProgressBar::SetProgress(int step)
{
    if (step < 0) return;
    if (m_disable == false && m_step == step)
    {
        return;
    }

    m_disable = false;
    m_step = step;
    Refresh();
}


void ProgressBar::SetMinSize(const wxSize &size)
{
    if (size.y >= miniHeight) {
        m_miniHeight = size.y;
    } else {
        return;
    }

    m_radius = m_miniHeight / 2.4;
    wxWindow::SetMinSize({size.x, m_miniHeight});
    // SetSize(size);
    SetRadius(m_radius);
}


void ProgressBar::paintEvent(wxPaintEvent &evt)
{

    wxPaintDC dc(this);
    render(dc);
}

void ProgressBar::render(wxDC &dc)
{
#ifdef __WXMSW__
    wxSize     size = GetSize();
    wxMemoryDC memdc;
    wxBitmap   bmp(size.x, size.y);
    memdc.SelectObject(bmp);
    memdc.Blit({0, 0}, size, &dc, {0, 0});

    {
        wxGCDC dc2(memdc);
        doRender(dc2);
    }

    memdc.SelectObject(wxNullBitmap);
    dc.DrawBitmap(bmp, 0, 0);
#else
    doRender(dc);
#endif
}

void ProgressBar::doRender(wxDC &dc)
{
    if (m_step >= m_max) m_step = m_max;
    wxSize size   = GetSize();
    dc.SetPen(wxPen(m_progress_background_colour, 1));
    dc.SetBrush(wxBrush(m_progress_background_colour));
    if (m_radius == 0) {
        dc.DrawRectangle(0, 0, size.x, size.y);
    } else {
        dc.DrawRoundedRectangle(0, 0, size.x, size.y, m_radius);
    }

    //draw progress
    if (m_disable) {
        m_proportion = float(size.x * float(this->m_step) / float(this->m_max));
        if (m_proportion < m_radius * 2 && m_proportion != 0) { m_proportion = m_radius * 2; }

        dc.SetPen(wxPen(m_progress_colour_disable, 1));
        dc.SetBrush(wxBrush(m_progress_colour_disable));
        if (m_radius == 0) {
            dc.DrawRectangle(0, 0, m_proportion, size.y);
        } else {
            dc.DrawRoundedRectangle(0, 0, m_proportion, size.y, m_radius);
        }

        dc.SetFont(::Label::Head_12);
        auto textSize = dc.GetMultiLineTextExtent(m_disable_text);
        dc.SetTextForeground(wxColour(144, 144, 144));
        auto pt = wxPoint();
        pt.x    = (size.x - textSize.x) / 2;
        pt.y    = (size.y - textSize.y) / 2;
        dc.DrawText(m_disable_text, pt);

    } else {
        m_proportion = float(size.x * float(this->m_step) / float(this->m_max));
        if (m_proportion < m_radius * 2  && m_proportion != 0) { m_proportion = m_radius * 2; }

        dc.SetPen(wxPen(m_progress_colour, 1));
        dc.SetBrush(wxBrush(m_progress_colour));
        if (m_radius == 0) {
            dc.DrawRectangle(0, 0, m_proportion, size.y);
        } else {
            dc.DrawRoundedRectangle(0, 0, m_proportion, size.y, m_radius);
        }

        dc.SetFont(GetFont());
        auto textSize = dc.GetMultiLineTextExtent(wxString("000%"));
        dc.SetTextForeground(wxColour(144, 144, 144));
        auto pt = wxPoint();
        pt.x    = (size.x - textSize.x) / 2;
        pt.y    = (size.y - textSize.y) / 2;

        auto text = wxString("");
        if (m_step < 10) {
            text = wxString::Format("%d", m_step);
        } else {
            text = wxString::Format("%d", m_step);
        }

        if (m_shownumber) {
            dc.DrawText(text + wxString("%"), pt);
        }
    }

}


void ProgressBar::DoSetSize(int x, int y, int width, int height, int sizeFlags)
{
    wxWindow::DoSetSize(x, y, width, height, sizeFlags);
}
