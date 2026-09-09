#pragma once

#include "slic3r/GUI/GUI_App.hpp"
#include <wx/window.h>

namespace Slic3r::GUI {

// Only for AI feature subtrees. Palette swatches are content, not theme tokens.
inline void refresh_ai_appearance(wxWindow* window, bool update_fonts = true)
{
    if (window == nullptr || window->GetName() == "ai_content_color")
        return;
    window->SetBackgroundColour(wxGetApp().get_window_default_clr());
    window->SetForegroundColour(wxGetApp().get_label_clr_default());
    if (update_fonts)
        window->SetFont(window->GetFont().GetWeight() == wxFONTWEIGHT_BOLD ?
                        wxGetApp().bold_font() : wxGetApp().normal_font());
    for (auto* child : window->GetChildren())
        refresh_ai_appearance(child, update_fonts);
    window->Refresh(false);
}

} // namespace Slic3r::GUI
