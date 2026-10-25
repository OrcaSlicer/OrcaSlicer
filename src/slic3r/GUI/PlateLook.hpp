#pragma once

#include <string>
#include <wx/bitmap.h>
#include <wx/gdicmn.h>
#include <wx/string.h>

#include "libslic3r/PrintConfig.hpp"

class wxWindow;

namespace Slic3r { namespace GUI {

// How a plate type appears to the user. Cosmetic only: the plate type itself, profiles and
// projects are untouched. Empty fields mean "use the built-in one".
struct PlateLook
{
    std::string label;
    std::string icon_path;
    std::string skin_path;
};

// Printers of the same vendor, model and bed size share their plate looks. A printer with no
// known model is keyed by its preset name.
std::string plate_look_printer_key();

PlateLook get_plate_look(BedType bed_type);
void      set_plate_look(BedType bed_type, const PlateLook &look);

// The icon image size users should upload.
constexpr int ICON_PIXELS = 240;

// The skin image size users should upload: the current printer's bed proportions, longest side
// at the largest texture the 3D view keeps.
wxSize skin_pixel_size();

wxString builtin_plate_label(BedType bed_type);

// The user's label, or the built-in one.
wxString plate_label(BedType bed_type);

// The icon at `icon_path`, or the built-in one when empty or unreadable, scaled to `px` pixels high.
wxBitmap icon_bitmap(BedType bed_type, const std::string &icon_path, wxWindow *window, int px, bool big = false);

// The user's icon, or the built-in one, scaled to `px` pixels high.
wxBitmap plate_icon_bitmap(BedType bed_type, wxWindow *window, int px, bool big = false);

// The skin at `skin_path` shrunk to fit `box`; invalid when there is none.
wxBitmap skin_preview_bitmap(const std::string &skin_path, const wxSize &box);

}} // namespace Slic3r::GUI
