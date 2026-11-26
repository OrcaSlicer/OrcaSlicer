#include "PreviewPanelSlots.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <sstream>
#include <string>

#include "libslic3r/AppConfig.hpp"

namespace Slic3r {
namespace GUI {

std::string preview_panel_slot_to_string(const PreviewPanelSlot &slot)
{
    return std::string(slot.column == PreviewColumn::Left ? "left " : "right ") + std::to_string(slot.order);
}

std::optional<PreviewPanelSlot> parse_preview_panel_slot(const std::string &text)
{
    std::istringstream in(text);
    std::string        column;
    PreviewPanelSlot   slot;
    if (!(in >> column >> slot.order) || (column != "left" && column != "right"))
        return std::nullopt;
    slot.column = column == "left" ? PreviewColumn::Left : PreviewColumn::Right;
    return slot;
}

int preview_panel_rank(PreviewPanel panel)
{
    switch (panel) {
    case PreviewPanel::Legend: return 0;
    case PreviewPanel::Summary: return 1;
    case PreviewPanel::GCode:
    case PreviewPanel::Count: break;
    }
    return 2;
}

static const char *panel_slot_key(PreviewPanel panel)
{
    switch (panel) {
    case PreviewPanel::Legend: return "legend_slot";
    case PreviewPanel::Summary: return "print_summary_slot";
    case PreviewPanel::GCode:
    case PreviewPanel::Count: break;
    }
    return "gcode_panel_slot";
}

PreviewPanelSlots load_preview_panel_slots(const AppConfig &config)
{
    PreviewPanelSlots slots = DEFAULT_PREVIEW_PANEL_SLOTS;
    for (size_t panel = 0; panel < PREVIEW_PANEL_COUNT; ++panel) {
        if (const std::optional<PreviewPanelSlot> slot = parse_preview_panel_slot(config.get(panel_slot_key(PreviewPanel(panel)))))
            slots[panel] = *slot;
    }
    return slots;
}

void save_preview_panel_slots(AppConfig &config, const PreviewPanelSlots &slots)
{
    for (size_t panel = 0; panel < PREVIEW_PANEL_COUNT; ++panel)
        config.set(panel_slot_key(PreviewPanel(panel)), preview_panel_slot_to_string(slots[panel]));
}

static const char *panel_collapsed_key(PreviewPanel panel)
{
    switch (panel) {
    case PreviewPanel::Legend: return "legend_collapsed";
    case PreviewPanel::Summary: return "print_summary_collapsed";
    case PreviewPanel::GCode:
    case PreviewPanel::Count: break;
    }
    return "gcode_panel_collapsed";
}

bool load_preview_panel_collapsed(const AppConfig &config, PreviewPanel panel) { return config.get(panel_collapsed_key(panel)) == "1"; }

void save_preview_panel_collapsed(AppConfig &config, PreviewPanel panel, bool collapsed)
{
    config.set(panel_collapsed_key(panel), collapsed ? "1" : "0");
}

PreviewPanelSlots move_panel_across(PreviewPanelSlots slots, PreviewPanel panel)
{
    PreviewPanelSlot &slot = slots[size_t(panel)];
    slot.column            = slot.column == PreviewColumn::Left ? PreviewColumn::Right : PreviewColumn::Left;
    return slots;
}

bool gcode_panel_first(const PreviewPanelSlots &slots)
{
    return slots[size_t(PreviewPanel::GCode)].order < slots[size_t(PreviewPanel::Legend)].order;
}

PreviewPanelSlots put_gcode_panel_first(PreviewPanelSlots slots, bool first)
{
    slots[size_t(PreviewPanel::Legend)].order = first ? 1 : 0;
    slots[size_t(PreviewPanel::GCode)].order  = first ? 0 : 1;
    return slots;
}

} // namespace GUI
} // namespace Slic3r
