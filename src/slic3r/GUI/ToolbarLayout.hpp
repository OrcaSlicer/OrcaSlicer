#pragma once

#include <nlohmann/json.hpp>

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

struct ToolbarLayout
{
    int                version = 1;
    ToolbarGroupLayout main;
    ToolbarGroupLayout gizmos;
    // Top-level fields this build does not know (written by a newer one), kept so a save does not drop them.
    nlohmann::json     extra = nlohmann::json::object();

    bool operator==(const ToolbarLayout& rhs) const
    {
        return version == rhs.version && main == rhs.main && gizmos == rhs.gizmos && extra == rhs.extra;
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

}} // namespace Slic3r::GUI
