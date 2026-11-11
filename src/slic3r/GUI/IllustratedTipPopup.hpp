#pragma once

#include <string>
#include <vector>

#include <wx/gdicmn.h>
#include <wx/string.h>

class wxWindow;

namespace Slic3r::GUI {

// One picture under a tooltip's text: a file under resources/images (a PNG still or a GIF that loops
// while the tooltip is open), and a translated caption under it, or none. Several in one tooltip are
// laid out side by side at equal widths, which is how a setting is shown as a comparison.
struct TipMedia
{
    std::string file;
    wxString    caption;
};

// Tooltips for wx windows that can carry pictures, with the behaviour of the illustrated tooltips of the
// 3D view (IllustratedTooltip): they appear once the pointer has rested on a control, sit beside the
// control rather than under the mouse, and play their animations from the first frame each time they
// open. A tooltip without pictures is drawn the same way, so the controls of one page all look alike.
// See docs/HLSD/tooltip-illustrations.md.

// Gives `window` a tooltip, replacing any wxToolTip it had. Calling it again replaces the text and the
// pictures, also while the tooltip is on screen. Empty text and no pictures remove it.
void set_illustrated_tooltip(wxWindow *window, const wxString &text, const std::vector<TipMedia> &media = {});

// For a window that draws several controls of its own (the option labels of OG_CustomCtrl, the cells
// of a list, the tools of a toolbar): call it on every mouse move over `window` with the control under
// the pointer, `item` being its rectangle in `window`'s client coordinates. An empty `item`, or empty
// text and no pictures, means the pointer is on nothing that has a tooltip.
void hover_illustrated_tooltip(wxWindow *window, const wxRect &item, const wxString &text,
                               const std::vector<TipMedia> &media = {});

// Hides the tooltip on screen, if any, and cancels one waiting to appear: for when a control starts
// being used, or its window is about to be hidden or destroyed.
void hide_illustrated_tooltip();

} // namespace Slic3r::GUI
