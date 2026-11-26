#pragma once

#include <functional>

#include "PreviewPanelSlots.hpp"
#include "PrintSummaryModel.hpp"
#include "CanvasUi/PanelCard.hpp"
#include "CanvasUi/Types.hpp"

namespace Slic3r {
class AppConfig;

namespace GUI {

namespace CanvasUi {
class Card;
class Painter;
} // namespace CanvasUi

// What the print takes and what happens in it, for the plate shown: totals, the filament each part of the print uses, changes,
// events, the grouping of filaments over two nozzles and the profiles a G-code file was made with.
class PrintSummaryPanel
{
public:
    // From 1.
    std::function<void(int)>  go_to_layer;
    std::function<void(bool)> set_stealth;
    std::function<void()>     set_optimal_grouping;
    std::function<void()>     regroup_filaments;
    std::function<void()>     open_grouping_wiki;
    std::function<void(int)>  hover_filament;
    std::function<void(int)>  isolate_filament;
    CanvasUi::PanelCollapse   collapse;

    void load_settings(const AppConfig& config);
    void render(const PreviewPanelPlacement& place, float bottom, const PrintSummaryModel& model);
    void                  hide() { m_last_rect = CanvasUi::Rect(); }
    const CanvasUi::Rect& last_rect() const { return m_last_rect; }

private:
    struct Sizes;
    class Body;

    float          m_body_content{0.f};
    CanvasUi::Rect m_last_rect;

    void render_header(const CanvasUi::PanelCard& frame, const Sizes& s, const PrintSummaryModel& model);

    friend void draw_totals_line(const CanvasUi::Painter& paint, const PrintSummaryModel& model, float left, float top, float right, const Sizes& s);
};

} // namespace GUI
} // namespace Slic3r
