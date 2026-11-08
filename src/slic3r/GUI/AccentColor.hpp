#ifndef slic3r_GUI_AccentColor_hpp_
#define slic3r_GUI_AccentColor_hpp_

#include <wx/colour.h>

// Unlayered Slicer accent palette.
//
// The accent is a neutral grey and is DELIBERATELY THEME-INDEPENDENT: none of these
// values appears as a key or as a value in gDarkColors (Widgets/StateColor.cpp), so
// StateColor::darkModeColorFor() / lightModeColorFor() pass them through unchanged and
// the accent renders identically in light and dark mode. Keep it that way -- adding a
// gDarkColors row for Base/Hover/Pressed is what would break it.
//
// Base #767676 is the one grey that clears both themes at once:
//   white text on Base ............ 4.54:1  (WCAG AA normal text, >= 4.5:1)
//   Base against dark panel #2D2D31  3.02:1  (WCAG AA non-text UI, >= 3:1)
// It sits exactly at both thresholds. Anything lighter fails white text; anything
// darker disappears on the dark panel. Hover is a knowing compromise (3.54:1 with
// white text) because WCAG is evaluated on the default state.
//
// Icon SVGs use a SEPARATE, theme-split ramp -- they sit beside #949494 line art on the
// 3D canvas rather than on a filled button, so a single mid-grey is unreadable there.
// That ramp is baked into the assets by tools/theme/recolor-accent.sh and inverted for
// dark mode in BitmapCache::load_svg(). See that file for the values.
//
// Float mirror of Base lives in libslic3r/Color.hpp :: ColorRGB/ColorRGBA::ORCA()
// (a GUI header cannot be included from libslic3r) -- keep the two in sync.

namespace Slic3r { namespace GUI { namespace Accent {

// Interaction ramp.
inline constexpr const char *Base    = "#767676";
inline constexpr const char *Hover   = "#888888";
inline constexpr const char *Pressed = "#5C5C5C";

// Hover is the ONE part of the ramp that is not theme-independent. Against the dark
// window background #2D2D31 the Base->Hover step (#767676 -> #888888) is only ~1.3:1 and
// reads as no change at all, so dark mode lifts it further. This has a gDarkColors row
// ({"#888888", "#A8A8A8"}); ImGui does not consult that map, so its dark branches must
// reference HoverDark explicitly.
inline constexpr const char *HoverDark = "#A8A8A8";

// Selection / focus bands. Formerly the teal tints #BFE1DE (25%) and #E5F0EE (10%).
// These two DO get a gDarkColors row, because a background band has to follow the
// theme; only the interaction ramp above is theme-independent.
inline constexpr const char *Tint25     = "#D8D8D8";
inline constexpr const char *Tint25Dark = "#4F4F53";
inline constexpr const char *Tint10     = "#ECECEC";
inline constexpr const char *Tint10Dark = "#3A3A3E";

// Integer forms, for the StateColor(unsigned long) / DropDown / ComboBox overloads.
// Note StateColor::append(unsigned long) swizzles R and B; for a neutral grey that
// swizzle is the identity, so the whole class of R/B-swap bugs is moot here.
inline constexpr unsigned long BaseRGB    = 0x767676;
inline constexpr unsigned long HoverRGB   = 0x888888;
inline constexpr unsigned long PressedRGB = 0x5C5C5C;
inline constexpr unsigned long Tint25RGB  = 0xD8D8D8;
inline constexpr unsigned long Tint10RGB  = 0xECECEC;

inline wxColour base()    { return wxColour(Base); }
inline wxColour hover()   { return wxColour(Hover); }
inline wxColour pressed() { return wxColour(Pressed); }
inline wxColour tint25()  { return wxColour(Tint25); }
inline wxColour tint10()  { return wxColour(Tint10); }

}}} // namespace Slic3r::GUI::Accent

#endif // slic3r_GUI_AccentColor_hpp_
