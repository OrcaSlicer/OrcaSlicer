#pragma once

#include <cstddef>
#include <utility>
#include <vector>

namespace Slic3r
{

// Display-index -> device-tool-id permutation for the printer's tool list.
//
// display_to_tool()[i] is the device tool id shown at display position i. A
// valid order is a permutation of 0..size()-1: every device tool id appears
// exactly once, so the forward and inverse lookups always agree.
//
// This is not physical_extruder_map. physical_extruder_map maps a logical
// extruder to a physical (firmware) extruder; this order only chooses which
// display position a device tool takes. Never apply one where the other is
// expected.
class ToolDisplayOrder
{
public:
    // No order supplied: invalid, so a forgotten assignment cannot pass for a
    // one-tool printer. Every lookup returns -1.
    ToolDisplayOrder() = default;

    // Takes the order as given. A negative, out-of-range, or duplicate id
    // makes the order invalid, and every lookup then returns -1.
    explicit ToolDisplayOrder(std::vector<int> display_to_tool);

    // Also checks that the order describes exactly tool_count tools. A length
    // mismatch makes the order invalid, as does any malformed id.
    ToolDisplayOrder(std::vector<int> display_to_tool, int tool_count);

    bool is_valid() const { return m_valid; }
    std::size_t size() const { return m_display_to_tool.size(); }

    // Device tool id shown at display position display_index, or -1 when the
    // order is invalid or display_index is not in [0, size()).
    int tool_at(int display_index) const;

    // Display position of device tool id tool_id, or -1 when the order is
    // invalid or tool_id is not in [0, size()).
    int display_index_of(int tool_id) const;

    // Raw order. Only meaningful when is_valid() is true.
    const std::vector<int>& display_to_tool() const { return m_display_to_tool; }

private:
    void rebuild_inverse();

    std::vector<int> m_display_to_tool;
    std::vector<int> m_tool_to_display;
    bool             m_valid = false;
};

inline ToolDisplayOrder::ToolDisplayOrder(std::vector<int> display_to_tool)
    : m_display_to_tool(std::move(display_to_tool))
{
    rebuild_inverse();
}

inline ToolDisplayOrder::ToolDisplayOrder(std::vector<int> display_to_tool, int tool_count)
    : m_display_to_tool(std::move(display_to_tool))
{
    if (tool_count < 0 || static_cast<std::size_t>(tool_count) != m_display_to_tool.size())
        return;
    rebuild_inverse();
}

inline void ToolDisplayOrder::rebuild_inverse()
{
    const std::size_t tool_count = m_display_to_tool.size();
    m_tool_to_display.assign(tool_count, -1);
    m_valid = true;
    for (std::size_t display_index = 0; display_index < tool_count; ++display_index) {
        const int tool_id = m_display_to_tool[display_index];
        // A duplicate id is already mapped; an id outside 0..tool_count-1 has
        // no position to map to. Either one means this is not a permutation.
        if (tool_id < 0 || static_cast<std::size_t>(tool_id) >= tool_count
            || m_tool_to_display[static_cast<std::size_t>(tool_id)] != -1) {
            m_valid = false;
            m_tool_to_display.clear();
            return;
        }
        m_tool_to_display[static_cast<std::size_t>(tool_id)] = static_cast<int>(display_index);
    }
}

inline int ToolDisplayOrder::tool_at(int display_index) const
{
    if (!m_valid || display_index < 0 || static_cast<std::size_t>(display_index) >= m_display_to_tool.size())
        return -1;
    return m_display_to_tool[static_cast<std::size_t>(display_index)];
}

inline int ToolDisplayOrder::display_index_of(int tool_id) const
{
    if (!m_valid || tool_id < 0 || static_cast<std::size_t>(tool_id) >= m_tool_to_display.size())
        return -1;
    return m_tool_to_display[static_cast<std::size_t>(tool_id)];
}

// Resolve a zero-based filament index to the device tool id the printer uses
// for it.
//
// filament_map is the saved 1-based filament -> logical extruder map.
// physical_extruder_map is the effective logical -> device map whose length
// bounds the device ids; pass effective_physical_extruder_map()'s output, not
// the raw option default. A result needs all of:
//   - filament_index in [0, filament_map.size())
//   - a 1-based filament_map entry naming a logical tool in [1, physical_extruder_map.size()]
//   - a device id in [0, physical_extruder_map.size())
// Anything else returns -1.
//
// The physical map need not be a permutation: several logical tools may route
// to the same device. The result is a device tool id, not a display position;
// use ToolDisplayOrder for the latter.
inline int resolve_filament_device_id(int filament_index,
                                      const std::vector<int>& filament_map,
                                      const std::vector<int>& physical_extruder_map)
{
    if (filament_index < 0 || static_cast<std::size_t>(filament_index) >= filament_map.size())
        return -1;

    const int logical_extruder = filament_map[static_cast<std::size_t>(filament_index)];
    if (logical_extruder <= 0 || static_cast<std::size_t>(logical_extruder) > physical_extruder_map.size())
        return -1;

    const int device_id = physical_extruder_map[static_cast<std::size_t>(logical_extruder - 1)];
    if (device_id < 0 || static_cast<std::size_t>(device_id) >= physical_extruder_map.size())
        return -1;

    return device_id;
}

} // namespace Slic3r
