#include "LabeledStaticBox.hpp"
#include "libslic3r/Utils.hpp"
#include "../GUI.hpp"
#include "../GUI_Utils.hpp"
#include "Label.hpp"
#include <utility>
#include <wx/string.h>
#include <wx/gdicmn.h>
#include <wx/event.h>
#include <wx/dcclient.h>
#include "slic3r/GUI/Widgets/StateHandler.hpp"
#include <wx/dc.h>
#include <wx/peninfobase.h>
#include <algorithm>
#include "slic3r/GUI/Widgets/StateColor.hpp"
#include <wx/dcgraph.h>

#ifdef __WXGTK3__
#include <gtk/gtk.h>
#endif

LabeledStaticBox::LabeledStaticBox()
    : state_handler(this)
{
    m_radius       = 3;
    m_border_width = 1;
    m_font         = Label::Head_14;
    text_color = StateColor(
        std::make_pair(0x363636, (int) StateColor::Normal),
        std::make_pair(0x6B6B6B, (int) StateColor::Disabled)
    );
    background_color = StateColor(
        std::make_pair(0xFFFFFF, (int) StateColor::Normal),
        std::make_pair(0xF0F0F1, (int) StateColor::Disabled)
    );
    border_color = StateColor(
        std::make_pair(0xDBDBDB, (int) StateColor::Normal),
        std::make_pair(0xDBDBDB, (int) StateColor::Disabled)
    );
}

LabeledStaticBox::LabeledStaticBox(
    wxWindow*       parent,
    const wxString& label,
    const wxPoint&  pos,
    const wxSize&   size,
    long            style
)
    : LabeledStaticBox()
{
    Create(parent, label, pos, size, style);
}

bool LabeledStaticBox::Create(
    wxWindow*       parent,
    const wxString& label,
    const wxPoint&  pos,
    const wxSize&   size,
    long            style
)
{
    if (style & wxBORDER_NONE)
        m_border_width = 0;
    wxStaticBox::Create(parent, wxID_ANY, label, pos, size, style);
#ifdef __WXOSX__
    Slic3r::GUI::staticbox_remove_margin(this);
#endif

    m_label = label;
    m_scale = FromDIP(100) / 100.f;
    m_pos   = this->GetPosition();

    int tW,tH,descent,externalLeading;
    // empty label sets m_label_height as 0 that causes extra spacing at top
    GetTextExtent(m_label.IsEmpty() ? "Orca" : m_label, &tW, &tH, &descent, &externalLeading, &m_font);
    m_label_height = tH - externalLeading;
    m_label_width  = tW;

#ifdef __WXGTK3__
    // GTK never sends EVT_PAINT for wxStaticBox, so hook "draw" directly
    GtkWidget* frame = GetHandle();
    gtk_widget_set_app_paintable(frame, TRUE);
    g_signal_connect(frame, "draw", G_CALLBACK(+[](GtkWidget* widget, cairo_t* cr, LabeledStaticBox* self) -> gboolean {
        if (wxGraphicsContext* gc = wxGraphicsContext::CreateFromNative(cr)) {
            gc->SetContentScaleFactor(self->GetContentScaleFactor()); // keeps 1px borders sharp
            wxGCDC dc(gc); // takes ownership of gc
            self->DrawBorderAndLabel(dc);
        }
        // Draw the child ourselves since GtkFrame's drawing is skipped
        if (GtkWidget* child = gtk_bin_get_child(GTK_BIN(widget)))
            gtk_container_propagate_draw(GTK_CONTAINER(widget), child, cr);
        return TRUE; // skip the native GtkFrame drawing
    }), this);

    // Hide the native label so GtkFrame reserves no space for it
    if (GtkWidget* label = gtk_frame_get_label_widget(GTK_FRAME(frame)))
        gtk_widget_hide(label);
    UpdateGtkPadding();
#else
    Bind(wxEVT_PAINT,([this](wxPaintEvent e) {
        wxPaintDC dc(this);
        PickDC(dc);
    }));
#endif

    state_handler.attach({&text_color, &background_color, &border_color});
    state_handler.update_binds();
    #ifndef __WXOSX__
        SetBackgroundStyle(wxBG_STYLE_PAINT);
    #endif
    SetBackgroundColour(background_color.colorForStates(state_handler.states()));
    SetForegroundColour(      text_color.colorForStates(state_handler.states()));
    SetBorderColor(         border_color.colorForStates(state_handler.states()));
    SetCanFocus(false);
    DisableFocusFromKeyboard();
    return true;
}

void LabeledStaticBox::SetCornerRadius(int radius)
{
    this->m_radius = radius;
    Refresh();
}

void LabeledStaticBox::SetBorderWidth(int width)
{
    this->m_border_width = width;
    Refresh();
}

void LabeledStaticBox::SetBorderColor(StateColor const &color)
{
    border_color = color;
    state_handler.update_binds();
    Refresh();
}

bool LabeledStaticBox::SetFont(const wxFont &set_font)
{
    m_font = set_font;

    int tW,tH,descent,externalLeading;
    // empty label sets m_label_height as 0 that causes extra spacing at top
    GetTextExtent(m_label.IsEmpty() ? "Orca" : m_label, &tW, &tH, &descent, &externalLeading, &m_font);
    m_label_height = tH - externalLeading;
    m_label_width  = tW;

#ifdef __WXGTK3__
    UpdateGtkPadding();
#endif
    Refresh();
    return true;
}

bool LabeledStaticBox::Enable(bool enable)
{
    bool result = this->wxStaticBox::Enable(enable);
    if (result) {
        wxCommandEvent e(EVT_ENABLE_CHANGED);
        e.SetEventObject(this);
        GetEventHandler()->ProcessEvent(e);
        this->SetForegroundColour(      text_color.colorForStates(state_handler.states()));
        this->SetBorderColor(         border_color.colorForStates(state_handler.states()));
    }
    return result;
}

void LabeledStaticBox::PickDC(wxDC& dc)
{
#ifdef __WXMSW__
    wxSize size = GetSize();
    if (size.x <= 0 || size.y <= 0)
        return;
    wxMemoryDC memdc(&dc);
    if (!memdc.IsOk()) {
        DrawBorderAndLabel(dc);
        return;
    }
    wxBitmap bmp(size.x, size.y);
    memdc.SelectObject(bmp);
    memdc.SetBackground(wxBrush(GetBackgroundColour()));
    memdc.Clear();
    {
        wxGCDC dc2(memdc);
        DrawBorderAndLabel(dc2);
    }

    memdc.SelectObject(wxNullBitmap);
    dc.DrawBitmap(bmp, 0, 0);
#else
    DrawBorderAndLabel(dc);
#endif
}

void LabeledStaticBox::DrawBorderAndLabel(wxDC& dc)
{
    // fill full background
    dc.SetBackground(wxBrush(background_color.colorForStates(0)));
    dc.Clear();

    wxSize wSz = GetSize();

    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.SetPen(wxPen(border_color.colorForStates(state_handler.states()), m_border_width, wxPENSTYLE_SOLID));
    dc.DrawRoundedRectangle( // Border
        std::max(0, m_pos.x),
        std::max(0, m_pos.y) + m_label_height * .5,
        wSz.GetWidth(),
        wSz.GetHeight() - m_label_height * .5,
        m_radius * m_scale
    );

    if (!m_label.IsEmpty()) {
        dc.SetFont(m_font);
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(background_color.colorForStates(0)));
        dc.DrawRectangle(wxRect(7 * m_scale,0 , m_label_width + 7 * m_scale, m_label_height)); // text background
        // NEEDFIX if text lenght > client size 
        dc.SetTextForeground(text_color.colorForStates(state_handler.states()));
        dc.DrawText(m_label, wxPoint(10 * m_scale, 0));
    }
}

#ifdef __WXGTK3__
// Controls parented to the box are placed by GtkFrame, so pad it to match GetBordersForSizer
void LabeledStaticBox::UpdateGtkPadding()
{
    int top, other;
    GetBordersForSizer(&top, &other);
    const wxString css = wxString::Format(
        "frame { margin: 0; padding: 0; border-width: 0; }"
        "frame > border { margin: 0; border-width: 0; padding: %dpx %dpx %dpx; }",
        top, other, other);

    GtkWidget* frame    = GetHandle();
    auto*      provider = static_cast<GtkCssProvider*>(g_object_get_data(G_OBJECT(frame), "orca-css"));
    if (!provider) {
        provider = gtk_css_provider_new();
        gtk_style_context_add_provider(gtk_widget_get_style_context(frame), GTK_STYLE_PROVIDER(provider),
                                       GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_set_data_full(G_OBJECT(frame), "orca-css", provider, g_object_unref);
    }
    gtk_css_provider_load_from_data(provider, css.utf8_str().data(), -1, nullptr);
}
#endif

void LabeledStaticBox::GetBordersForSizer(int* borderTop, int* borderOther) const {
    wxStaticBox::GetBordersForSizer(borderTop, borderOther);
#ifdef __WXOSX__
    *borderOther = 5; // Make sure macOS uses the same border padding as other platforms
#endif
#ifdef __WXGTK3__
    *borderOther = 5;
    *borderTop = m_label.IsEmpty() ? m_label_height / 2 + 5 : m_label_height + 2;
#endif
}