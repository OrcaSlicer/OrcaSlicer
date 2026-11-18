#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI {

// The user's order and hidden set for the Prepare canvas's main toolbar and gizmo bar, saved as one JSON
// string in AppConfig ([toolbar] layout). Ids are the toolbar item names and the gizmo ids below; the
// layout is always merged with the stock order so ids from other builds are dropped and new ones appear.

inline constexpr const char* TOOLBAR_SEPARATOR_ID = "|";

struct ToolbarGroupLayout
{
    std::vector<std::string> order;
    std::set<std::string>    hidden;

    bool operator==(const ToolbarGroupLayout& rhs) const { return order == rhs.order && hidden == rhs.hidden; }
    bool operator!=(const ToolbarGroupLayout& rhs) const { return !(*this == rhs); }
};

// A user-made group of gizmo bar icons that share one bar button. Members are gizmo bar ids in their bar order.
struct ToolbarGizmoGroup
{
    std::string              name;
    std::vector<std::string> members;

    bool operator==(const ToolbarGizmoGroup& rhs) const { return name == rhs.name && members == rhs.members; }
    bool operator!=(const ToolbarGizmoGroup& rhs) const { return !(*this == rhs); }
};

struct ToolbarLayout
{
    int                version = 1;
    ToolbarGroupLayout main;
    ToolbarGroupLayout gizmos;
    // The user's gizmo groups, in bar order.
    std::vector<ToolbarGizmoGroup> groups;
    // Entries of the saved "groups" array that are not user groups, kept so a save does not drop them.
    nlohmann::json     other_groups = nlohmann::json::array();
    // Top-level fields this build does not know (written by a newer one), kept so a save does not drop them.
    nlohmann::json     extra = nlohmann::json::object();

    bool operator==(const ToolbarLayout& rhs) const
    {
        return version == rhs.version && main == rhs.main && gizmos == rhs.gizmos && groups == rhs.groups &&
               other_groups == rhs.other_groups && extra == rhs.extra;
    }
    bool operator!=(const ToolbarLayout& rhs) const { return !(*this == rhs); }
};

// Main toolbar item names in stock order, with TOOLBAR_SEPARATOR_ID where the separator goes.
const std::vector<std::string>& stock_main_toolbar_ids();
// One id per GLGizmosManager::EType value, in enum order.
const std::vector<std::string>& gizmo_ids_in_etype_order();
// The gizmos that can sit in the bar, in stock order (svg and simplify never do).
const std::vector<std::string>& stock_gizmo_bar_ids();

// Keeps the saved ids that are in stock, in saved order, and inserts missing stock ids after their nearest
// stock predecessor. Hidden ids not in stock are dropped.
ToolbarGroupLayout merge_toolbar_group(const std::vector<std::string>& stock,
                                       const std::vector<std::string>& saved_order,
                                       const std::vector<std::string>& saved_hidden);

ToolbarLayout stock_toolbar_layout();
// Never throws; a value that is not a JSON object gives the stock layout.
ToolbarLayout parse_toolbar_layout(const std::string& value);
// Returns "" when the layout is stock and carries no unknown fields: the caller then erases the key.
std::string serialize_toolbar_layout(const ToolbarLayout& layout);

// Drops group members that are not gizmo bar ids or that an earlier group already has, and groups left with fewer
// than two members; then moves each group's members next to its first one in gizmos.order and keeps the groups and
// their members in bar order.
void normalize_gizmo_groups(ToolbarLayout& layout);

// The gizmo list as the Customize Toolbar dialog shows it: a header row before each group's members.
enum class ToolbarRowKind { Plain, Header, Member };
struct ToolbarGizmoRow
{
    ToolbarRowKind kind;
    std::string    id;         // empty for a header
    int            group = -1; // the group of a header or member

    bool operator==(const ToolbarGizmoRow& rhs) const { return kind == rhs.kind && id == rhs.id && group == rhs.group; }
};
std::vector<ToolbarGizmoRow> gizmo_rows(const ToolbarLayout& layout);

// The dialog's edits. Each returns false and changes nothing when it does not apply (fewer than two ids, an id
// already grouped or not on the bar, a group index out of range, a blank name, a row that cannot move).
// Makes a group of ungrouped ids, gathered where the first of them is in the bar order.
bool group_gizmos(ToolbarLayout& layout, const std::vector<std::string>& ids, const std::string& name);
// Moves ungrouped ids into a group, right after its last member.
bool add_to_group(ToolbarLayout& layout, int group, const std::vector<std::string>& ids);
bool ungroup_gizmos(ToolbarLayout& layout, int group);
bool rename_group(ToolbarLayout& layout, int group, const std::string& name);
// name_for(n) for the lowest n >= 1 that no group is named.
std::string default_group_name(const ToolbarLayout& layout, const std::function<std::string(int)>& name_for);
// Moves a row one step (-1 up, +1 down). A header moves its whole group past the next row or group; a plain row
// jumps a whole group; a member moves within its group and leaves it at the group's edge, and a group left with one
// member is removed.
bool move_gizmo_row(ToolbarLayout& layout, const ToolbarGizmoRow& row, int step);

}} // namespace Slic3r::GUI
