#pragma once

#include <wx/panel.h>
#include <wx/hyperlink.h>
#include <wx/sizer.h>

#include <string>

class Label;

namespace Slic3r
{
namespace GUI
{

// Single message item, including color, text and optional wiki link
class wgtMsgPanelItem : public wxPanel
{
public:
    wgtMsgPanelItem(wxWindow* parent,
                    const wxColour& colour,
                    const wxString& text,
                    int max_width,
                    const wxString& wiki_url = wxEmptyString);

    void SetColour(const wxColour& colour);
    void SetText(const wxString& text);
    void SetWiki(const wxString& wiki_url);

private:
    void CreateGui();
    void OnClickWiki(wxHyperlinkEvent& evt);

private:
    int m_max_width;
    wxColour m_colour;
    wxString m_text;
    wxString m_wiki_url;

    Label* m_text_label{ nullptr };
    wxHyperlinkCtrl* m_wiki_link{ nullptr };
};

// Message panel based on wxWidget, shows wgtMsgPanelItem in a list
class wgtMsgPanel : public wxPanel
{
public:
    explicit wgtMsgPanel(wxWindow* parent);
    ~wgtMsgPanel() override = default;

    // Add a message (preferred API)
    void AddMessage(const wxString& text,
                    const wxColour& colour,
                    const wxString& wiki_url = wxEmptyString);
    void Clear();

    // Const API
    int GetMessageCount() const { return m_list_sizer ? m_list_sizer->GetItemCount() : 0; }

private:
    void CreateGui();

private:
    Label*            m_label_title{ nullptr };
    wxBoxSizer*       m_list_sizer{ nullptr };
};

}
} // namespace Slic3r::GUI
