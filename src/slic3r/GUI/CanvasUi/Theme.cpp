#include "Theme.hpp"

#include "Types.hpp"
#include "libslic3r/Color.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

static Color orca(float alpha = 1.f)
{
    const ColorRGBA c = ColorRGBA::ORCA();
    return {c.r(), c.g(), c.b(), alpha};
}

static Theme make_light()
{
    Theme t;
    t.dark                     = false;
    t.card                     = rgb(0xffffff);
    t.foreground               = rgb(0x323a3d);
    t.muted_foreground         = rgb(0x6b7175);
    t.inactive_foreground      = rgb(0x5f666a);
    t.disabled_foreground      = rgb(0x9aa0a4);
    t.tabs_list                = rgb(0xf7f8f9);
    t.tab_hover                = rgb(0x000000, .06f);
    t.shadow_below             = rgb(0x000000, .16f);
    t.current_line             = rgb(0x009688);
    t.position_indicator       = rgb(0x000000, .07f);
    t.position_indicator_thumb = rgb(0xa0a6aa);
    t.contrast_outline         = rgb(0x000000, .28f);
    t.scrollbar_thumb          = rgb(0x6b7175, .45f);
    t.scrollbar_thumb_hover    = rgb(0x6b7175, .65f);
    t.scrollbar_thumb_active   = rgb(0x6b7175, .85f);
    t.border                   = rgb(0xe3e5e7);
    t.secondary                = rgb(0xf1f2f3);
    t.input                    = rgb(0x878d90);
    t.accent                   = rgb(0x000000, .06f);
    t.card_border              = rgb(0x000000, .20f);
    t.primary                  = orca();
    t.primary_strong           = rgb(0x00675b);
    t.primary_foreground       = rgb(0xffffff);
    t.selection                = orca(.16f);
    t.tooltip                  = rgb(0x323a3d);
    t.tooltip_foreground       = rgb(0xffffff);
    t.tooltip_muted_foreground = rgb(0xffffff, .7f);
    t.toggle_pressed           = rgb(0xffffff);
    t.toggle_pressed_border    = rgb(0x878d90);
    t.shadow                   = rgb(0x000000, .09f);
    t.toggle_pressed_shadow    = rgb(0x000000, .12f);
    t.switch_thumb             = rgb(0xffffff);
    t.switch_thumb_shadow      = rgb(0x000000, .2f);
    t.warning                  = rgb(0xb8461f);
    t.warning_surface          = rgb(0xb8461f, .08f);
    t.info                     = rgb(0x1f6fa8);
    t.secondary_hover = blend_over(t.accent, blend_over(t.secondary, t.card));
    return t;
}

static Theme make_dark()
{
    Theme t;
    t.dark                     = true;
    t.card                     = rgb(0x2d2d31);
    t.foreground               = rgb(0xe6e6e7);
    t.muted_foreground         = rgb(0xa0a0a6);
    t.inactive_foreground      = rgb(0xa0a0a6);
    t.disabled_foreground      = rgb(0x6d727a);
    t.tabs_list                = rgb(0x29292d);
    t.tab_hover                = rgb(0xffffff, .04f);
    t.shadow_below             = rgb(0x000000, .45f);
    t.current_line             = rgb(0x26a69a);
    t.position_indicator       = rgb(0xffffff, .08f);
    t.position_indicator_thumb = rgb(0x8a8d94);
    t.contrast_outline         = rgb(0xffffff, .35f);
    t.scrollbar_thumb          = rgb(0xa0a0a6, .45f);
    t.scrollbar_thumb_hover    = rgb(0xa0a0a6, .65f);
    t.scrollbar_thumb_active   = rgb(0xa0a0a6, .85f);
    t.border                   = rgb(0x3d3d45);
    t.secondary                = rgb(0x000000, .25f);
    t.input                    = rgb(0x828288);
    t.accent                   = rgb(0xffffff, .08f);
    t.card_border              = rgb(0xffffff, .12f);
    t.primary                  = orca();
    t.primary_strong           = rgb(0x26a69a);
    t.primary_foreground       = t.card;
    t.selection                = orca(.28f);
    t.tooltip                  = rgb(0xffffff);
    t.tooltip_foreground       = rgb(0x2d2d31);
    t.tooltip_muted_foreground = rgb(0x2d2d31, .7f);
    t.toggle_pressed           = rgb(0x46464e);
    t.toggle_pressed_border    = rgb(0x828288);
    t.shadow                   = rgb(0x000000, .22f);
    t.toggle_pressed_shadow    = rgb(0x000000, .12f);
    t.switch_thumb             = rgb(0xffffff);
    t.switch_thumb_shadow      = rgb(0x000000, .2f);
    t.warning                  = rgb(0xf1754e);
    t.warning_surface          = rgb(0xf1754e, .12f);
    t.info                     = rgb(0x7fc4f0);
    t.secondary_hover = blend_over(t.accent, blend_over(t.secondary, t.card));
    return t;
}

const Theme& theme(bool dark)
{
    static const Theme light_theme = make_light();
    static const Theme dark_theme  = make_dark();
    return dark ? dark_theme : light_theme;
}

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
