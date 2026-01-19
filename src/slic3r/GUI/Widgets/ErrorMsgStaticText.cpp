#include "ErrorMsgStaticText.hpp"
#include <wx/dcclient.h>

ErrorMsgStaticText::ErrorMsgStaticText() {}

ErrorMsgStaticText::ErrorMsgStaticText(wxWindow *      parent,
                             wxWindowID      id,
                             const wxPoint & pos,
                             const wxSize &  size)
{
    Create(parent, id, pos, size);
    Bind(wxEVT_PAINT, &ErrorMsgStaticText::paintEvent, this);
}

void ErrorMsgStaticText::paintEvent(wxPaintEvent &evt)
{
    auto size = GetSize();
    wxPaintDC dc(this);
    auto text_height = dc.GetCharHeight();
    wxString  out_txt = m_msg;
    wxString  count_txt  = "";
    int line_count  = 1;
    int new_line_pos = 0;
    bool is_ch = false;

    if (m_msg[0] > 0x80 && m_msg[1] > 0x80)is_ch = true;

    for (int i = 0; i < m_msg.length(); i++) {
        auto text_size = dc.GetTextExtent(count_txt);
        if (text_size.x < (size.x)) {
            count_txt += m_msg[i];
            if (m_msg[i] == ' ' ||
                m_msg[i] == ',' ||
                m_msg[i] == '.' ||
                m_msg[i] == '\n')
            {
                new_line_pos = i;
            }
        } else {
            if (!is_ch)
            {
                out_txt[new_line_pos] = '\n';
                i = new_line_pos;
            } else {
                out_txt.insert(i-1,'\n');
            }
            count_txt = "";
            line_count++;
        }
    }
    SetSize(wxSize(-1, line_count * text_height));
    SetMinSize(wxSize(-1, line_count * text_height));
    SetMaxSize(wxSize(-1, line_count * text_height));
    dc.DrawText(out_txt, 0, 0);
}
