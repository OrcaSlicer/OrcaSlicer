#ifndef slic3r_GUI_HyperLink_hpp_
#define slic3r_GUI_HyperLink_hpp_

#include <wx/wx.h>
#include <wx/window.h>

namespace Slic3r { namespace GUI {

class HyperLink : public wxStaticText
{
public:
    HyperLink(wxWindow* parent, const wxString& label = wxEmptyString, const wxString& url = wxEmptyString, const long style = 0);

    void     SetURL(const wxString& url);
    wxString GetURL() const;

    bool SetFont(const wxFont& font);

private:
    wxString m_url;
    wxColour m_normalColor;
    wxColour m_hoverColor;
};

}} // namespace Slic3r::GUI
#endif // !slic3r_GUI_HyperLink_hpp_
