#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

// The part of a label ImGui shows, as text: "Move##gizmo" -> "Move", "###id" -> "", and Orca's
// icon glyphs and color markers left out.
std::string clean_imgui_label(const std::string& label);
// The id part of a label, which names a control whose shown label is empty or says little:
// "##cursor_radius" -> "cursor_radius", "Select##select_source_volume" -> "select_source_volume",
// "N###extruder_color_N" -> "extruder_color_N", "Move" -> "".
std::string imgui_label_name(const std::string& label);
// An ImGui window as locators name it: the shown title of its top window, so a panel's child
// windows ("Move##gizmo/##body_1A2B3C4D") count as the panel.
std::string imgui_window_label(const std::string& window);

// One line of a ui_snapshot.
struct UiNode
{
    std::string ref;     // "w12", "m3", "t5", "z2", "g7"
    std::string kind;    // button, checkbox, text, menu_item, gltoolbar, gizmo, imgui, canvas, dialog, ...
    std::string cls;     // C++ class without namespaces, for wx windows
    std::string label;
    std::string value;
    std::string name;    // wx name, when it is not a default one
    std::string option;  // config option key of a setting field
    std::string window;  // the ImGui window an ImGui item or text is in
    std::string note;    // anything else worth saying, e.g. "clipped"
    bool        enabled = true;
    bool        focused = false;
    int         checked = -1; // -1 not checkable, 0/1 state
    int         depth   = 0;
    int         x = 0, y = 0, w = 0, h = 0; // client coordinates in the parent window / canvas
};

// Compact indented text, one node per line, e.g. `  [w12] button "Slice plate"`.
std::string format_snapshot(const std::vector<UiNode>& nodes, std::size_t omitted);

// One item of a popup menu, from a walk that lists a submenu before its items.
struct MenuEntry
{
    std::string label;          // as shown, without mnemonic and accelerator
    int         parent  = -1;   // the entry of the submenu holding it; -1 at the top
    int         id      = 0;
    bool        enabled = true; // false under a disabled submenu too
    bool        checkable = false, checked = false, submenu = false;
    std::string path;           // set by name_menu_entries()
};

// Gives each entry its path: the labels from the top, joined with '>'. A label repeated among its
// siblings is numbered from its second time on ("Change Filament>Generic PLA #2"), so each path
// names one item. `current` is the marker a menu appends to the item in use (" (current)"); the
// numbering leaves it out, so the numbers stay when another item is in use.
void name_menu_entries(std::vector<MenuEntry>& entries, const std::string& current);
// {items, disabled?, checked?}: the paths of the items that run something (under the submenu entry
// `under`, or all of them for -1), then those of them that are disabled and those checked.
nlohmann::json menu_listing(const std::vector<MenuEntry>& entries, int under = -1);
// The entry `path` names. Labels match case-insensitively, `current` and a trailing "..." may be
// left out, and "#2" picks a label's second time among its siblings. A submenu or a disabled item
// can't run: null then, as with no match, with `err` saying why and `details` what there is to pick.
const MenuEntry* find_menu_entry(const std::vector<MenuEntry>& entries, const std::string& path, const std::string& current,
                                 std::string& err, nlohmann::json& details);

// The labels a row locator names, from the top: {row: "Cube>Cylinder"}, or an array of labels, which
// the journal writes when a label holds '>'. Throws ToolFailure for anything else.
std::vector<std::string> row_segments(const nlohmann::json& row);
// What a locator records for a row's labels from the top: joined with '>', or the array when one
// holds '>'.
nlohmann::json row_locator(const std::vector<std::string>& path);
// Whether `wanted` names a row whose labels from the top are `path`: it matches the path's last
// segments, so the plate an object is on may be left out. A label matches as shown or without
// `current`, the marker a list appends to the item in use.
bool row_matches(const std::vector<std::string>& path, const std::vector<std::string>& wanted, const std::string& current);

// A plate's icons in the Prepare view, by the picking action PartPlate gives each: "plate" (0, the
// plate itself), "delete", "orient", "arrange", "lock", "settings", "name", "move_to_front",
// "filament_map", "imex_mode". The bed picking id of plate p's icon a is p * kPlateIconCount + a.
constexpr int kPlateIconCount = 10;
// The action an icon name stands for, or -1.
int plate_icon_action(const std::string& name);
// The name of an action, or "" outside 0..kPlateIconCount-1.
std::string plate_icon_name(int action);
// {plate, action} of a bed picking id.
std::pair<int, int> plate_icon_of(int picking_id);

// The refs ui_snapshot hands out. Ids are never reused, so two clients can't confuse each other's
// refs; each stores what it points at and how to tell whether that still exists, plus the UI
// generation it was taken in (a rebuilt main window invalidates every ref).
template<class Payload> class RefTable
{
public:
    static constexpr std::size_t kMaxRefs = 20000;

    std::string add(char kind, uint64_t generation, Payload payload, std::function<bool()> alive)
    {
        std::string ref = std::string(1, kind) + std::to_string(++m_next);
        m_entries.emplace(ref, Entry{generation, std::move(payload), std::move(alive)});
        m_order.push_back(ref);
        while (m_order.size() > kMaxRefs) {
            m_entries.erase(m_order.front());
            m_order.pop_front();
        }
        return ref;
    }

    // The payload of a live ref, or nullptr with `err` saying why not.
    const Payload* resolve(const std::string& ref, uint64_t generation, std::string& err) const
    {
        auto it = m_entries.find(ref);
        if (it == m_entries.end()) {
            err = "Unknown ref " + ref + "; take a new ui_snapshot";
            return nullptr;
        }
        if (it->second.generation != generation || (it->second.alive && !it->second.alive())) {
            err = "Stale ref " + ref + ": the element is gone or the window was rebuilt; take a new ui_snapshot";
            return nullptr;
        }
        return &it->second.payload;
    }

private:
    struct Entry
    {
        uint64_t              generation;
        Payload               payload;
        std::function<bool()> alive;
    };
    std::map<std::string, Entry> m_entries;
    std::deque<std::string>      m_order;
    uint64_t                     m_next = 0;
};

}}} // namespace Slic3r::GUI::Mcp
