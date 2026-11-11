#include "IllustratedTipPopup.hpp"

#include <wx/window.h>

namespace Slic3r::GUI {

// Placeholder until the popup lands: a plain wxToolTip with the text, no pictures.
void set_illustrated_tooltip(wxWindow *window, const wxString &text, const std::vector<TipMedia> &media)
{
    (void) media;
    if (window != nullptr)
        window->SetToolTip(text);
}

void hover_illustrated_tooltip(wxWindow *window, const wxRect &item, const wxString &text, const std::vector<TipMedia> &media)
{
    (void) media;
    if (window != nullptr)
        window->SetToolTip(item.IsEmpty() ? wxString() : text);
}

void hide_illustrated_tooltip() {}

} // namespace Slic3r::GUI
