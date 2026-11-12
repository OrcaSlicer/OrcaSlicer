#pragma once

#include <functional>
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

// Pictures worked out when the tooltip opens rather than when it is set, for pictures that are costly to
// look up or depend on state that changes (the value a setting has now). Called only while the window it
// was given for exists.
using TipMediaSource = std::function<std::vector<TipMedia>()>;

// set_illustrated_tooltip() with pictures from `media`, asked for each time the tooltip opens. A null
// `media` means no pictures.
void set_illustrated_tooltip_deferred(wxWindow *window, const wxString &text, TipMediaSource media);

// hover_illustrated_tooltip() with pictures from `media`, asked for when the tooltip opens. `item` and
// `text` tell the controls apart; a tooltip whose text is empty and whose `media` gives nothing does not
// open.
void hover_illustrated_tooltip_deferred(wxWindow *window, const wxRect &item, const wxString &text, TipMediaSource media);

// If the tooltip of `window` is on screen, asks its TipMediaSource again and redraws it: for a window
// whose pictures follow its value, when the value changes.
void refresh_illustrated_tooltip(wxWindow *window);

// The text of the tooltip given to `window` with set_illustrated_tooltip(), or an empty string. The
// popup replaces the native tooltip, so code that named a control by its tooltip (an icon-only button
// read by a screen reader) asks here instead.
wxString illustrated_tooltip_text(const wxWindow *window);

// Whether a tooltip is on screen for `window` or for a window inside it.
bool is_illustrated_tooltip_shown_for(const wxWindow *window);

} // namespace Slic3r::GUI
