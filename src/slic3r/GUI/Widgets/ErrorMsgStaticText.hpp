
#ifndef _WX_ERRORMSGSTATTEXT_H_
#define _WX_ERRORMSGSTATTEXT_H_

#include <wx/panel.h>
#include "wx/stattext.h"

class WXDLLIMPEXP_CORE ErrorMsgStaticText : public wxPanel
{
public:
    wxString m_msg;
    ErrorMsgStaticText();
    ErrorMsgStaticText(wxWindow *parent,
                 wxWindowID id = wxID_ANY,
                 const wxPoint& pos = wxDefaultPosition,
                 const wxSize& size = wxSize(0,0));

    void paintEvent(wxPaintEvent &evt);

    void SetLabel(wxString msg){m_msg = msg;};

};
#endif
