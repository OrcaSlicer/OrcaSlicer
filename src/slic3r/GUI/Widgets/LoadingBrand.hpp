#ifndef slic3r_LoadingBrand_hpp_
#define slic3r_LoadingBrand_hpp_

#include "../GUI_App.hpp"
#include "../wxExtensions.hpp"

#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>

namespace Slic3r { namespace GUI {

// Native controls keep the brand visible before OpenGL or WebView is ready.
inline void add_loading_brand(wxWindow* parent, wxBoxSizer* layout)
{
    auto* logo = new wxStaticBitmap(parent, wxID_ANY,
        create_scaled_bitmap("OrcaSlicer_gradient_circle", parent, 96));
    layout->Add(logo, 0, wxALIGN_CENTER_HORIZONTAL | wxALL, parent->FromDIP(12));
#ifdef __WXMSW__
    logo->Bind(wxEVT_DPI_CHANGED, [logo](wxDPIChangedEvent& event) {
        logo->SetBitmap(create_scaled_bitmap("OrcaSlicer_gradient_circle", logo, 96));
        logo->GetParent()->Layout();
        event.Skip();
    });
#endif
    auto* title = new wxStaticText(parent, wxID_ANY, "OrcaSlicer");
    title->SetFont(wxGetApp().bold_font().Scaled(1.5));
    title->SetForegroundColour(wxGetApp().get_label_clr_default());
    layout->Add(title, 0, wxALIGN_CENTER_HORIZONTAL | wxALL, parent->FromDIP(12));
}

inline void paint_loading_content(wxWindow* panel)
{
    panel->Refresh();
    panel->Update();
    // On MSW, UpdateWindow only flushes the current HWND, not its children.
    for (wxWindow* child : panel->GetChildren())
        child->Update();
}

}} // namespace Slic3r::GUI

#endif
