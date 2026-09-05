#include "HyperLink.hpp"
#include "Label.hpp"

namespace Slic3r { namespace GUI {

HyperLink::HyperLink(wxWindow* parent, const wxString& label, const wxString& url, long style)
    : wxStaticText(parent, wxID_ANY, label, wxDefaultPosition, wxDefaultSize, style | wxUSE_MARKUP)
    , m_url(url)
    , m_normalColor(wxColour("#009687")) // used slightly different color otherwise automatically uses ColorForDark that not visible enough
    , m_hoverColor(wxColour("#26A69A"))
{
    SetForegroundColour(m_normalColor);
    HyperLink::SetFont(Label::Head_14);

    //SetCursor(wxCursor(wxCURSOR_HAND)); // drectly using on class sets curson on parent permanently on linux

    if (!m_url.IsEmpty())
        SetToolTip(m_url);

    Bind(wxEVT_LEFT_DOWN, ([this](wxMouseEvent& e) {
        if (!m_url.IsEmpty())
            wxLaunchDefaultBrowser(m_url);
    }));

    Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& e) {
        SetCursor(wxCursor(wxCURSOR_HAND));
        SetForegroundColour(m_normalColor);
        e.StopPropagation();
    });
    Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& e) {
        SetCursor(wxNullCursor);
        SetForegroundColour(m_hoverColor);
        e.StopPropagation();
    });
}

bool HyperLink::SetFont(const wxFont& font)
{ // ensure it stays underlined
    #ifdef __WXMAC__
        // SetUnderlined not works on macOS. set underline via markup
        bool result = wxStaticText::SetFont(font);
        SetLabelMarkup("<u>" + GetLabelText() + "</u>");
        return result;
    #else
        wxFont f = font;
        f.SetUnderlined(true);
        return wxStaticText::SetFont(f);
    #endif
}

void HyperLink::SetURL(const wxString& url)
{
    m_url = url;
    SetToolTip(m_url);
}

wxString HyperLink::GetURL() const { return m_url; }

}} // namespace Slic3r::GUI
