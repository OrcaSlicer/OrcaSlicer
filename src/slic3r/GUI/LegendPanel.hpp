#pragma once

#include <functional>

#include "LegendModel.hpp"
#include "LegendScaleView.hpp"
#include "LegendText.hpp"
#include "PreviewPanelSlots.hpp"
#include "CanvasUi/PanelCard.hpp"
#include "CanvasUi/Types.hpp"
#include "libvgcode/include/Types.hpp"

namespace Slic3r {
class AppConfig;

namespace GUI {

namespace CanvasUi {
class Card;
struct SettingsFormOptions;
} // namespace CanvasUi

// What the colours of the preview mean, the parts of the print hidden, and its totals, in a card of fixed
// width over the preview.
class LegendPanel
{
public:
    std::function<void(libvgcode::EViewType)> select_view_type;
    std::function<void(const LegendRow&)>     toggle_row;
    // Hides every other line type, or every other move.
    std::function<void(const LegendRow&)>     show_only_row;
    // Every line type, and the moves shown by default.
    std::function<void()>                     show_all;
    std::function<void()>                     show_all_line_types;
    std::function<void()>                     hide_all_moves;
    std::function<void(bool)>                 show_travel;
    // The row under the mouse this frame, to show its paths in the preview.
    std::function<void(const LegendRow&)>     hover_row;
    std::function<void(float, float)>         hover_value;
    std::function<void(int)>                  hover_filament;
    // Keeps a filament in colour and hides the others, or shows them all again when it is the one kept.
    std::function<void(int)>                  isolate_filament;
    std::function<void(const LegendSettings&)> save_settings;
    CanvasUi::PanelCollapse                    collapse;
    LegendScaleView                            scale_view;

    void load_settings(const AppConfig& config);
    const LegendSettings& settings() const { return m_settings; }
    void render(const PreviewPanelPlacement& place, float bottom, const LegendModel& model);
    void                  hide() { m_last_rect = CanvasUi::Rect(); }
    const CanvasUi::Rect& last_rect() const { return m_last_rect; }

private:
    struct Sizes;
    class Body;

    LegendSettings m_settings;
    bool           m_settings_open{false};
    float          m_body_content{0.f};
    LegendRow      m_menu_row;
    CanvasUi::Point m_menu_pos;
    bool           m_menu_pending{false};
    CanvasUi::Rect m_last_rect;

    void  render_header(const CanvasUi::PanelCard& frame, const Sizes& s, const LegendModel& model);
    void  render_row_menu(const LegendModel& model);
    CanvasUi::SettingsFormOptions settings_form(float width);
};

} // namespace GUI
} // namespace Slic3r
