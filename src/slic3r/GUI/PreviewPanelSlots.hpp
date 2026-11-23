#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>

namespace Slic3r {
class AppConfig;

namespace GUI {

enum class PreviewColumn : unsigned char { Left, Right };

enum class PreviewPanel : unsigned char { Legend, GCode, Summary, Count };

inline constexpr size_t PREVIEW_PANEL_COUNT = size_t(PreviewPanel::Count);

// Where panels of equal order stand in a column: the legend, then the print summary, then the G-code panel.
int preview_panel_rank(PreviewPanel panel);

struct PreviewPanelSlot
{
    PreviewColumn column{PreviewColumn::Right};
    int           order{0};
};

using PreviewPanelSlots   = std::array<PreviewPanelSlot, PREVIEW_PANEL_COUNT>;
using PreviewPanelHeights = std::array<float, PREVIEW_PANEL_COUNT>;
using PreviewPanelShown   = std::array<bool, PREVIEW_PANEL_COUNT>;

inline constexpr PreviewPanelSlots DEFAULT_PREVIEW_PANEL_SLOTS{{{PreviewColumn::Right, 0}, {PreviewColumn::Right, 1}, {PreviewColumn::Right, 1}}};

std::string                     preview_panel_slot_to_string(const PreviewPanelSlot &slot);
std::optional<PreviewPanelSlot> parse_preview_panel_slot(const std::string &text);

PreviewPanelSlots load_preview_panel_slots(const AppConfig &config);
void              save_preview_panel_slots(AppConfig &config, const PreviewPanelSlots &slots);

// A value missing or not understood reads as expanded.
bool load_preview_panel_collapsed(const AppConfig &config, PreviewPanel panel);
void save_preview_panel_collapsed(AppConfig &config, PreviewPanel panel, bool collapsed);

// The panel in the other column, where it keeps its order.
PreviewPanelSlots move_panel_across(PreviewPanelSlots slots, PreviewPanel panel);
// Where the legend and the G-code panel share a column, which of them stands above.
bool              gcode_panel_first(const PreviewPanelSlots &slots);
PreviewPanelSlots put_gcode_panel_first(PreviewPanelSlots slots, bool first);

struct PreviewPanelPlacement
{
    PreviewColumn column{PreviewColumn::Right};
    // The edge the panel is anchored to: its left edge in the left column, its right edge in the right one.
    float         x{0.f};
    float         top{0.f};
    float         bottom{0.f};

    bool  is_right() const { return column == PreviewColumn::Right; }
    float left(float width) const { return is_right() ? x - width : x; }
};

} // namespace GUI
} // namespace Slic3r
