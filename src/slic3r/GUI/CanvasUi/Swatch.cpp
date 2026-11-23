#include "Swatch.hpp"

#include "Context.hpp"
#include "Font.hpp"
#include "Metrics.hpp"
#include "Paint.hpp"
#include "Theme.hpp"
#include "Tokens.hpp"
#include "Types.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

namespace {

constexpr int CHECKER_CELLS = 3;

} // namespace

void draw_swatch(const Painter& paint, const Rect& rect, const Swatch& swatch, const Color& background)
{
    const Context& ui     = *Context::current();
    const Theme&   t      = ui.theme();
    const Metrics& m      = ui.metrics();
    const float    radius = m.swatch_radius;
    if (swatch.hollow) {
        const Color edge = contrast_ratio(swatch.color, background) < 3.f ? t.muted_foreground : swatch.color;
        paint.stroke_rect(rect, edge, m.px(Stroke::md), radius);
        return;
    }
    Color shown = swatch.color;
    if (swatch.translucent) {
        // The corner cells are light, so the rounded light base leaves no square corner showing.
        paint.fill_rect(rect, rgb(0xffffff), radius);
        const float cell_w = rect.width() / float(CHECKER_CELLS);
        const float cell_h = rect.height() / float(CHECKER_CELLS);
        for (int i = 0; i < CHECKER_CELLS; ++i)
            for (int j = 0; j < CHECKER_CELLS; ++j)
                if ((i + j) % 2 == 1)
                    paint.fill_rect(Rect(rect.left + float(i) * cell_w, rect.top + float(j) * cell_h, rect.left + float(i + 1) * cell_w,
                                         rect.top + float(j + 1) * cell_h),
                                    rgb(0xd9d9d9));
        shown = blend_over(swatch.color, rgb(0xffffff));
    } else
        shown = blend_over(swatch.color, background);
    paint.fill_rect(rect, swatch.color, radius);
    if (contrast_ratio(shown, background) < 3.f)
        paint.stroke_rect(rect, t.contrast_outline, m.outline, radius);
    if (!swatch.label.empty()) {
        const Font  font = ui.bold_font().resized(m.px(TextRole::caption.size));
        const Color ink  = contrast_ratio(shown, rgb(0xffffff)) >= contrast_ratio(shown, rgb(0x000000)) ? rgb(0xffffff) : rgb(0x000000);
        paint.text(font, Point(rect.left + .5f * (rect.width() - font.width(swatch.label)), rect.top + .5f * (rect.height() - font.size())), ink,
                   swatch.label);
    }
}

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
