#include "ExtraRenderers.hpp"
#include "wxExtensions.hpp"
#include "GUI.hpp"
#include "BitmapComboBox.hpp"
#include "Plater.hpp"
#include "Widgets/ComboBox.hpp"

#include <wx/dc.h>
#ifdef wxHAS_GENERIC_DATAVIEWCTRL
#include "wx/generic/private/markuptext.h"
#include "wx/generic/private/rowheightcache.h"
#include "wx/generic/private/widthcalc.h"
#endif
/*
#ifdef __WXGTK__
#include "wx/gtk/private.h"
#include "wx/gtk/private/value.h"
#endif
*/
#if wxUSE_ACCESSIBILITY
#include "wx/private/markupparser.h"
#endif // wxUSE_ACCESSIBILITY

using Slic3r::GUI::from_u8;
using Slic3r::GUI::into_u8;


//-----------------------------------------------------------------------------
// DataViewBitmapText
//-----------------------------------------------------------------------------

wxIMPLEMENT_DYNAMIC_CLASS(DataViewBitmapText, wxObject)

IMPLEMENT_VARIANT_OBJECT(DataViewBitmapText)

// ---------------------------------------------------------
// BitmapTextRenderer
// ---------------------------------------------------------

#if ENABLE_NONCUSTOM_DATA_VIEW_RENDERING
BitmapTextRenderer::BitmapTextRenderer(wxDataViewCellMode mode /*= wxDATAVIEW_CELL_EDITABLE*/, 
                                                 int align /*= wxDVR_DEFAULT_ALIGNMENT*/): 
wxDataViewRenderer(wxT("PrusaDataViewBitmapText"), mode, align)
{
    SetMode(mode);
    SetAlignment(align);
}
#endif // ENABLE_NONCUSTOM_DATA_VIEW_RENDERING

BitmapTextRenderer::~BitmapTextRenderer()
{
#ifdef SUPPORTS_MARKUP
    #ifdef wxHAS_GENERIC_DATAVIEWCTRL
    delete m_markupText;
    #endif //wxHAS_GENERIC_DATAVIEWCTRL
#endif // SUPPORTS_MARKUP
}

void BitmapTextRenderer::EnableMarkup(bool enable)
{
#ifdef SUPPORTS_MARKUP
#ifdef wxHAS_GENERIC_DATAVIEWCTRL
    if (enable) {
        if (!m_markupText)
            m_markupText = new wxItemMarkupText(wxString());
    }
    else {
        if (m_markupText) {
            delete m_markupText;
            m_markupText = nullptr;
        }
    }
#else
    is_markupText = enable;
#endif //wxHAS_GENERIC_DATAVIEWCTRL
#endif // SUPPORTS_MARKUP
}

bool BitmapTextRenderer::SetValue(const wxVariant &value)
{
    m_value << value;

#ifdef SUPPORTS_MARKUP
#ifdef wxHAS_GENERIC_DATAVIEWCTRL
    if (m_markupText)
        m_markupText->SetMarkup(m_value.GetText());
    /* 
#else 
#if defined(__WXGTK__)
   GValue gvalue = G_VALUE_INIT;
    g_value_init(&gvalue, G_TYPE_STRING);
    g_value_set_string(&gvalue, wxGTK_CONV_FONT(str.GetText(), GetOwner()->GetOwner()->GetFont()));
    g_object_set_property(G_OBJECT(m_renderer/ *.GetText()* /), is_markupText ? "markup" : "text", &gvalue);
    g_value_unset(&gvalue);
#endif // __WXGTK__
    */
#endif // wxHAS_GENERIC_DATAVIEWCTRL
#endif // SUPPORTS_MARKUP

    return true;
}

bool BitmapTextRenderer::GetValue(wxVariant& WXUNUSED(value)) const
{
    return false;
}

#if ENABLE_NONCUSTOM_DATA_VIEW_RENDERING && wxUSE_ACCESSIBILITY
wxString BitmapTextRenderer::GetAccessibleDescription() const
{
#ifdef SUPPORTS_MARKUP
    if (m_markupText)
        return wxMarkupParser::Strip(m_text);
#endif // SUPPORTS_MARKUP

    return m_value.GetText();
}
#endif // wxUSE_ACCESSIBILITY && ENABLE_NONCUSTOM_DATA_VIEW_RENDERING

bool BitmapTextRenderer::Render(wxRect rect, wxDC *dc, int state)
{
    int xoffset = 0;

    const wxBitmap& icon = m_value.GetBitmap();
    if (icon.IsOk())
    {
#ifdef __APPLE__
        wxSize icon_sz = icon.GetScaledSize();
#else
        wxSize icon_sz = icon.GetSize();
#endif
        dc->DrawBitmap(icon, rect.x, rect.y + (rect.height - icon_sz.y) / 2);
        xoffset = icon_sz.x + 4;
    }

#if defined(SUPPORTS_MARKUP) && defined(wxHAS_GENERIC_DATAVIEWCTRL)
    if (m_markupText)
    {
        rect.x += xoffset;
        m_markupText->Render(GetView(), *dc, rect, 0, GetEllipsizeMode());
    }
    else
#endif // SUPPORTS_MARKUP && wxHAS_GENERIC_DATAVIEWCTRL
#ifdef _WIN32 
        // workaround for Windows DarkMode : Don't respect to the state & wxDATAVIEW_CELL_SELECTED to avoid update of the text color
        RenderText(m_value.GetText(), xoffset, rect, dc, state & wxDATAVIEW_CELL_SELECTED ? 0 :state);
#else
        RenderText(m_value.GetText(), xoffset, rect, dc, state);
#endif

    return true;
}

wxSize BitmapTextRenderer::GetSize() const
{
    if (!m_value.GetText().empty())
    {
        wxSize size;
#if defined(SUPPORTS_MARKUP) && defined(wxHAS_GENERIC_DATAVIEWCTRL)
        if (m_markupText)
        {
            wxDataViewCtrl* const view = GetView();
            wxClientDC dc(view);
            if (GetAttr().HasFont())
                dc.SetFont(GetAttr().GetEffectiveFont(view->GetFont()));

            size = m_markupText->Measure(dc);

            int lines = m_value.GetText().Freq('\n') + 1;
            size.SetHeight(size.GetHeight() * lines);
        }
        else
#endif // SUPPORTS_MARKUP && wxHAS_GENERIC_DATAVIEWCTRL
        {
            size = GetTextExtent(m_value.GetText());
            size.x = size.x * 9 / 8;
        }

        if (m_value.GetBitmap().IsOk())
            size.x += m_value.GetBitmap().GetWidth() + 4;
        return size;
    }
    return wxSize(80, 20);
}


wxWindow* BitmapTextRenderer::CreateEditorCtrl(wxWindow* parent, wxRect labelRect, const wxVariant& value)
{
    if (can_create_editor_ctrl && !can_create_editor_ctrl())
        return nullptr;

    DataViewBitmapText data;
    data << value;

    m_was_unusable_symbol = false;

    wxPoint position = labelRect.GetPosition();
    if (data.GetBitmap().IsOk()) {
        const int bmp_width = data.GetBitmap().GetWidth();
        position.x += bmp_width;
        labelRect.SetWidth(labelRect.GetWidth() - bmp_width);
    }

#ifdef __WXMSW__
    // Case when from some reason we try to create next EditorCtrl till old one was not deleted
    if (auto children = parent->GetChildren(); children.GetCount() > 0)
        for (auto child : children)
            if (dynamic_cast<wxTextCtrl*>(child)) {
                parent->RemoveChild(child);
                child->Destroy();
                break;
            }
#endif // __WXMSW__

    wxTextCtrl* text_editor = new wxTextCtrl(parent, wxID_ANY, data.GetText(),
                                             position, labelRect.GetSize(), wxTE_PROCESS_ENTER);
    text_editor->SetInsertionPointEnd();
    text_editor->SelectAll();
    text_editor->SetBackgroundColour(parent->GetBackgroundColour());
    text_editor->SetForegroundColour(parent->GetForegroundColour());

    return text_editor;
}

bool BitmapTextRenderer::GetValueFromEditorCtrl(wxWindow* ctrl, wxVariant& value)
{
    wxTextCtrl* text_editor = wxDynamicCast(ctrl, wxTextCtrl);
    auto item = GetView()->GetModel()->GetParent(m_item);
    if (!text_editor || (item.IsOk() && text_editor->GetValue().IsEmpty()))
        return false;

    m_was_unusable_symbol = Slic3r::GUI::Plater::has_illegal_filename_characters(text_editor->GetValue());
    if (m_was_unusable_symbol)
        return false;

    // The icon can't be edited so get its old value and reuse it.
    wxVariant valueOld;
    GetView()->GetModel()->GetValue(valueOld, m_item, /*colName*/0); 
    
    DataViewBitmapText bmpText;
    bmpText << valueOld;

    // But replace the text with the value entered by user.
    bmpText.SetText(text_editor->GetValue());

    value << bmpText;
    return true;
}

// ----------------------------------------------------------------------------
// BitmapChoiceRenderer
// ----------------------------------------------------------------------------

bool BitmapChoiceRenderer::SetValue(const wxVariant& value)
{
    m_value << value;
    return true;
}

bool BitmapChoiceRenderer::GetValue(wxVariant& value) const 
{
    value << m_value;
    return true;
}

bool BitmapChoiceRenderer::Render(wxRect rect, wxDC* dc, int state)
{
//    int xoffset = 0;

    const wxBitmap& icon = m_value.GetBitmap();
    if (icon.IsOk())
    {
        dc->DrawBitmap(icon, rect.x, rect.y + (rect.height - icon.GetHeight()) / 2);
//        xoffset = icon.GetWidth() + 4;

        if (rect.height == 0)
          rect.height = icon.GetHeight();
    }

#ifdef _WIN32
    // workaround for Windows DarkMode : Don't respect to the state & wxDATAVIEW_CELL_SELECTED to avoid update of the text color
//    RenderText(m_value.GetText(), xoffset, rect, dc, state & wxDATAVIEW_CELL_SELECTED ? 0 : state);
#else
//    RenderText(m_value.GetText(), xoffset, rect, dc, state);
#endif

    return true;
}

wxSize BitmapChoiceRenderer::GetSize() const
{
    wxSize sz;// = GetTextExtent(m_value.GetText());

    if (m_value.GetBitmap().IsOk()) {
        sz.x += m_value.GetBitmap().GetWidth() + 4;
        sz.y = m_value.GetBitmap().GetHeight() + 4;
    }

    return sz;
}


wxWindow* BitmapChoiceRenderer::CreateEditorCtrl(wxWindow* parent, wxRect labelRect, const wxVariant& value)
{
    if (can_create_editor_ctrl && !can_create_editor_ctrl())
        return nullptr;

    std::vector<wxBitmap*> icons = get_extruder_color_icons();
    if (icons.empty())
        return nullptr;

    DataViewBitmapText data;
    data << value;

    ::ComboBox *c_editor = new ::ComboBox(parent, wxID_ANY, wxEmptyString,
        labelRect.GetTopLeft(), wxSize(labelRect.GetWidth(), -1),
        0, nullptr, wxCB_READONLY | CB_NO_DROP_ICON | CB_NO_TEXT);
    c_editor->GetDropDown().SetUseContentWidth(true);

    if (has_default_extruder && has_default_extruder())
        c_editor->Append(_L("default"), *get_default_extruder_color_icon());

    for (size_t i = 0; i < icons.size(); i++)
        c_editor->Append(wxString::Format("%d", i+1), *icons[i]);

    if (has_default_extruder && has_default_extruder())
        c_editor->SetSelection(atoi(data.GetText().c_str()));
    else
        c_editor->SetSelection(atoi(data.GetText().c_str()) - 1);

#ifdef __linux__
    c_editor->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent& evt) {
        // to avoid event propagation to other sidebar items
        evt.StopPropagation();
        // FinishEditing grabs new selection and triggers config update. We better call
        // it explicitly, automatic update on KILL_FOCUS didn't work on Linux.
        this->FinishEditing();
    });
#else
    // to avoid event propagation to other sidebar items
    c_editor->Bind(wxEVT_COMBOBOX, [](wxCommandEvent& evt) { evt.StopPropagation(); });
#endif

    return c_editor;
}

bool BitmapChoiceRenderer::GetValueFromEditorCtrl(wxWindow* ctrl, wxVariant& value)
{
    ::ComboBox*c         = static_cast<::ComboBox *>(ctrl);
    int selection = c->GetSelection();
    if (selection < 0)
        return false;
   
    DataViewBitmapText bmpText;

    bmpText.SetText(c->GetString(selection));
    bmpText.SetBitmap(c->GetItemBitmap(selection));

    value << bmpText;
    return true;
}

// ----------------------------------------------------------------------------
// TextRenderer
// ----------------------------------------------------------------------------

bool TextRenderer::SetValue(const wxVariant& value)
{
    m_value = value.GetString();
    return true;
}

bool TextRenderer::GetValue(wxVariant& value) const
{
    return false;
}

bool TextRenderer::Render(wxRect rect, wxDC* dc, int state)
{
#ifdef _WIN32
    // workaround for Windows DarkMode : Don't respect to the state & wxDATAVIEW_CELL_SELECTED to avoid update of the text color
    RenderText(m_value, 0, rect, dc, state & wxDATAVIEW_CELL_SELECTED ? 0 : state);
#else
    RenderText(m_value, 0, rect, dc, state);
#endif

    return true;
}

wxSize TextRenderer::GetSize() const
{
    return GetTextExtent(m_value);
}


