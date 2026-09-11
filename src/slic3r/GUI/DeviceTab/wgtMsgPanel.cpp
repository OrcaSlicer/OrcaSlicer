#include "wgtMsgPanel.h"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include "slic3r/GUI/Widgets/StateColor.hpp"
#include "slic3r/GUI/wxExtensions.hpp"

#include <wx/sizer.h>

namespace Slic3r::GUI
{

// ===== wgtMsgPanelItem ======================================================

wgtMsgPanelItem::wgtMsgPanelItem(wxWindow* parent,
                                 const wxColour& colour,
                                 const wxString& text,
                                 int max_width,
                                 const wxString& wiki_url)
    : wxPanel(parent, wxID_ANY)
    , m_max_width(max_width)
    , m_colour(colour)
    , m_text(text)
    , m_wiki_url(wiki_url)
{
    CreateGui();
}

void wgtMsgPanelItem::CreateGui()
{
    SetBackgroundColour(StateColor::darkModeColorFor(wxColour(*wxWHITE)));

    wxBoxSizer* main_sizer = new wxBoxSizer(wxHORIZONTAL);

    // Color bar on the left
    //wxPanel* color_bar = new wxPanel(this, wxID_ANY);
    //color_bar->SetMinSize(wxSize(FromDIP(4), -1));
    //color_bar->SetBackgroundColour(m_colour);
    //main_sizer->Add(color_bar, 0, wxEXPAND | wxRIGHT, FromDIP(4));

    // Text and optional wiki link
    wxBoxSizer* text_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_text_label = new ::Label(this, m_text);
    m_text_label->SetFont(::Label::Body_13);
    m_text_label->SetBackgroundColour(StateColor::darkModeColorFor(wxColour(*wxWHITE)));
    m_text_label->SetForegroundColour(m_colour);
    if (!m_wiki_url.IsEmpty()) {
        m_text_label->Wrap(m_max_width - FromDIP(20)); // Wrap text to fit within a reasonable width
    } else {
        m_text_label->Wrap(m_max_width - FromDIP(10)); // Wrap text to fit within a reasonable width
    }

    text_sizer->Add(m_text_label, 0, wxALIGN_CENTER_VERTICAL);

    if (!m_wiki_url.IsEmpty()) {
        m_wiki_link = new wxHyperlinkCtrl(this, wxID_ANY, "Wiki->", m_wiki_url);
        m_wiki_link->SetNormalColour(m_colour);
        m_wiki_link->SetHoverColour(wxColour(0, 0, 200));
        m_wiki_link->SetVisitedColour(*wxBLUE);
        Bind(wxEVT_HYPERLINK, &wgtMsgPanelItem::OnClickWiki, this, m_wiki_link->GetId());
        text_sizer->AddSpacer(FromDIP(4));
        text_sizer->Add(m_wiki_link, 0, wxALIGN_CENTER_VERTICAL | wxEXPAND);
    }

    main_sizer->Add(text_sizer, 0, wxALIGN_LEFT | wxEXPAND);

    SetSizer(main_sizer);
    Layout();
    Fit();
}

void wgtMsgPanelItem::SetColour(const wxColour& colour)
{
    m_colour = colour;
    // Simple handling: refresh the whole control to redraw the color bar
    Refresh();
}

void wgtMsgPanelItem::SetText(const wxString& text)
{
    m_text = text;
    if (m_text_label) {
        m_text_label->SetLabel(m_text);
    }
}

void wgtMsgPanelItem::SetWiki(const wxString& wiki_url)
{
    m_wiki_url = wiki_url;
    if (m_wiki_link) {
        m_wiki_link->SetURL(m_wiki_url);
    }
}

void wgtMsgPanelItem::OnClickWiki(wxHyperlinkEvent& evt)
{
    if (!m_wiki_url.IsEmpty()) {
        wxLaunchDefaultBrowser(m_wiki_url);
    }
}

// ===== wgtMsgPanel ==========================================================

wgtMsgPanel::wgtMsgPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    CreateGui();
}

void wgtMsgPanel::CreateGui()
{
    SetBackgroundColour(StateColor::darkModeColorFor(wxColour(*wxWHITE)));

    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);

    m_label_title = new Label(this, _L("Note:"));
    m_label_title->SetFont(::Label::Body_14);
    main_sizer->Add(m_label_title, 0, wxEXPAND | wxALL, FromDIP(4));

    m_list_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->Add(m_list_sizer, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(4));

    SetSizer(main_sizer);
    Layout();
}

void wgtMsgPanel::AddMessage(const wxString& text,
                             const wxColour& colour,
                             const wxString& wiki_url)
{
    if (!m_list_sizer) return;

    auto* label = new ::Label(this, wxString::Format("%d. ", GetMessageCount() + 1));
    label->SetFont(::Label::Body_14);
    label->SetBackgroundColour(StateColor::darkModeColorFor(wxColour(*wxWHITE)));
    label->SetForegroundColour(colour);

    auto* item = new wgtMsgPanelItem(this, colour, text, GetSize().GetWidth(), wiki_url);
    auto h_sizer = new wxBoxSizer(wxHORIZONTAL);
    h_sizer->Add(label, 0, wxEXPAND);
    h_sizer->Add(item, 0, wxEXPAND);
    m_list_sizer->Add(h_sizer, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(2));
    Layout();
}

void wgtMsgPanel::Clear()
{
    if (!m_list_sizer) return;

    Freeze();
    wxSizerItemList children = m_list_sizer->GetChildren();
    for (auto it = children.begin(); it != children.end(); ++it) {
        if (wxWindow* win = (*it)->GetWindow()) {
            win->Destroy();
        }
    }
    m_list_sizer->Clear(true);
    Thaw();
    Layout();
}

} // namespace Slic3r::GUI
