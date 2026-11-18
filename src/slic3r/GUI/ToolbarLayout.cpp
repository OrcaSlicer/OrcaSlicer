#include "ToolbarLayout.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI {

namespace {

constexpr const char* KEY_VERSION       = "version";
constexpr const char* KEY_MAIN          = "main";
constexpr const char* KEY_MAIN_HIDDEN   = "main_hidden";
constexpr const char* KEY_GIZMOS        = "gizmos";
constexpr const char* KEY_GIZMOS_HIDDEN = "gizmos_hidden";
constexpr const char* KEY_GROUPS        = "groups";
constexpr const char* KEY_GROUP_NAME    = "name";
constexpr const char* KEY_GROUP_MEMBERS = "members";

std::vector<std::string> read_string_array(const nlohmann::json& object, const char* key)
{
    std::vector<std::string> out;
    const auto it = object.find(key);
    if (it == object.end() || !it->is_array())
        return out;
    for (const nlohmann::json& entry : *it)
        if (entry.is_string())
            out.push_back(entry.get<std::string>());
    return out;
}

bool contains(const std::vector<std::string>& ids, const std::string& id) { return std::find(ids.begin(), ids.end(), id) != ids.end(); }

// The hidden ids in the group's display order, so the saved value does not depend on std::set ordering.
nlohmann::json hidden_in_order(const ToolbarGroupLayout& group)
{
    nlohmann::json out = nlohmann::json::array();
    for (const std::string& id : group.order)
        if (group.hidden.count(id) != 0)
            out.push_back(id);
    return out;
}

void write_group(nlohmann::json& out, const ToolbarGroupLayout& group, const std::vector<std::string>& stock, const char* order_key, const char* hidden_key)
{
    if (group.order != stock)
        out[order_key] = group.order;
    if (!group.hidden.empty())
        out[hidden_key] = hidden_in_order(group);
}

} // namespace

const std::vector<std::string>& stock_main_toolbar_ids()
{
    static const std::vector<std::string> ids{"add", "addplate", "orient", "arrange", TOOLBAR_SEPARATOR_ID,
                                              "more", "fewer", "splitobjects", "splitvolumes", "layersediting"};
    return ids;
}

const std::vector<std::string>& gizmo_ids_in_etype_order()
{
    static const std::vector<std::string> ids{"move", "rotate", "scale", "flatten", "cut", "mesh_boolean",
                                              "fdm_supports", "seam", "fuzzy_skin", "mmu_segmentation",
                                              "painted_modifier", "texture_displacement", "emboss", "svg",
                                              "measure", "assembly", "simplify", "brim_ears"};
    return ids;
}

const std::vector<std::string>& stock_gizmo_bar_ids()
{
    static const std::vector<std::string> ids = [] {
        std::vector<std::string> out;
        std::copy_if(gizmo_ids_in_etype_order().begin(), gizmo_ids_in_etype_order().end(), std::back_inserter(out),
                     [](const std::string& id) { return id != "svg" && id != "simplify"; });
        return out;
    }();
    return ids;
}

ToolbarGroupLayout merge_toolbar_group(const std::vector<std::string>& stock,
                                       const std::vector<std::string>& saved_order,
                                       const std::vector<std::string>& saved_hidden)
{
    ToolbarGroupLayout out;
    for (const std::string& id : saved_order)
        if (contains(stock, id) && !contains(out.order, id))
            out.order.push_back(id);

    // Stock ids the saved order lacks go right after their nearest stock predecessor that is already placed.
    for (size_t i = 0; i < stock.size(); ++i) {
        if (contains(out.order, stock[i]))
            continue;
        auto insert_at = out.order.begin();
        for (size_t j = i; j > 0; --j) {
            const auto prev = std::find(out.order.begin(), out.order.end(), stock[j - 1]);
            if (prev != out.order.end()) {
                insert_at = std::next(prev);
                break;
            }
        }
        out.order.insert(insert_at, stock[i]);
    }

    for (const std::string& id : saved_hidden)
        if (contains(stock, id))
            out.hidden.insert(id);
    return out;
}

ToolbarLayout stock_toolbar_layout()
{
    ToolbarLayout layout;
    layout.main.order   = stock_main_toolbar_ids();
    layout.gizmos.order = stock_gizmo_bar_ids();
    return layout;
}

ToolbarLayout parse_toolbar_layout(const std::string& value)
{
    const nlohmann::json parsed = nlohmann::json::parse(value, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object())
        return stock_toolbar_layout();

    ToolbarLayout layout;
    const auto version = parsed.find(KEY_VERSION);
    if (version != parsed.end() && version->is_number_integer() && version->get<int>() >= 1)
        layout.version = version->get<int>();
    layout.main   = merge_toolbar_group(stock_main_toolbar_ids(), read_string_array(parsed, KEY_MAIN), read_string_array(parsed, KEY_MAIN_HIDDEN));
    layout.gizmos = merge_toolbar_group(stock_gizmo_bar_ids(), read_string_array(parsed, KEY_GIZMOS), read_string_array(parsed, KEY_GIZMOS_HIDDEN));

    const auto groups = parsed.find(KEY_GROUPS);
    const bool groups_known = groups != parsed.end() && groups->is_array();
    if (groups_known)
        for (const nlohmann::json& entry : *groups) {
            const auto name    = entry.is_object() ? entry.find(KEY_GROUP_NAME) : entry.end();
            const auto members = entry.is_object() ? entry.find(KEY_GROUP_MEMBERS) : entry.end();
            if (entry.is_object() && name != entry.end() && name->is_string() && members != entry.end() && members->is_array())
                layout.groups.push_back({name->get<std::string>(), read_string_array(entry, KEY_GROUP_MEMBERS)});
            else
                layout.other_groups.push_back(entry);
        }
    normalize_gizmo_groups(layout);

    for (const auto& [key, field] : parsed.items())
        if (key != KEY_VERSION && key != KEY_MAIN && key != KEY_MAIN_HIDDEN && key != KEY_GIZMOS && key != KEY_GIZMOS_HIDDEN &&
            !(key == KEY_GROUPS && groups_known))
            layout.extra[key] = field;
    return layout;
}

std::string serialize_toolbar_layout(const ToolbarLayout& layout)
{
    nlohmann::json out = nlohmann::json::object();
    write_group(out, layout.main, stock_main_toolbar_ids(), KEY_MAIN, KEY_MAIN_HIDDEN);
    write_group(out, layout.gizmos, stock_gizmo_bar_ids(), KEY_GIZMOS, KEY_GIZMOS_HIDDEN);
    if (!layout.groups.empty() || !layout.other_groups.empty()) {
        nlohmann::json groups = nlohmann::json::array();
        for (const ToolbarGizmoGroup& group : layout.groups)
            groups.push_back({{KEY_GROUP_NAME, group.name}, {KEY_GROUP_MEMBERS, group.members}});
        for (const nlohmann::json& entry : layout.other_groups)
            groups.push_back(entry);
        out[KEY_GROUPS] = std::move(groups);
    }
    if (out.empty() && layout.extra.empty())
        return {};

    out[KEY_VERSION] = std::max(layout.version, 1);
    for (const auto& [key, field] : layout.extra.items())
        // A "groups" value this build could not read gives way to the groups made here.
        if (key != KEY_GROUPS || out.find(KEY_GROUPS) == out.end())
            out[key] = field;
    return out.dump();
}

void normalize_gizmo_groups(ToolbarLayout& layout)
{
    std::vector<std::string>&      order = layout.gizmos.order;
    std::set<std::string>          taken;
    std::vector<ToolbarGizmoGroup> kept;
    for (const ToolbarGizmoGroup& group : layout.groups) {
        ToolbarGizmoGroup clean{group.name, {}};
        for (const std::string& id : order)
            if (contains(group.members, id) && contains(stock_gizmo_bar_ids(), id) && taken.count(id) == 0)
                clean.members.push_back(id);
        if (clean.members.size() < 2)
            continue;
        taken.insert(clean.members.begin(), clean.members.end());
        kept.push_back(std::move(clean));
    }

    // Each group's members follow its first member, in their existing relative order.
    for (const ToolbarGizmoGroup& group : kept) {
        const std::vector<std::string> rest(group.members.begin() + 1, group.members.end());
        order.erase(std::remove_if(order.begin(), order.end(), [&rest](const std::string& id) { return contains(rest, id); }), order.end());
        order.insert(std::next(std::find(order.begin(), order.end(), group.members.front())), rest.begin(), rest.end());
    }

    const auto position = [&order](const ToolbarGizmoGroup& group) { return std::find(order.begin(), order.end(), group.members.front()) - order.begin(); };
    std::stable_sort(kept.begin(), kept.end(), [&position](const ToolbarGizmoGroup& a, const ToolbarGizmoGroup& b) { return position(a) < position(b); });
    layout.groups = std::move(kept);
}

namespace {

int group_of(const ToolbarLayout& layout, const std::string& id)
{
    for (size_t i = 0; i < layout.groups.size(); ++i)
        if (contains(layout.groups[i].members, id))
            return static_cast<int>(i);
    return -1;
}

bool valid_group(const ToolbarLayout& layout, int group) { return group >= 0 && static_cast<size_t>(group) < layout.groups.size(); }

std::string trimmed(const std::string& text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

// Ids that can join a group: distinct gizmo bar ids in the order, in no group.
bool ungrouped_bar_ids(const ToolbarLayout& layout, const std::vector<std::string>& ids)
{
    for (size_t i = 0; i < ids.size(); ++i)
        if (!contains(stock_gizmo_bar_ids(), ids[i]) || !contains(layout.gizmos.order, ids[i]) || group_of(layout, ids[i]) >= 0 ||
            std::find(ids.begin(), ids.begin() + static_cast<std::ptrdiff_t>(i), ids[i]) != ids.begin() + static_cast<std::ptrdiff_t>(i))
            return false;
    return true;
}

} // namespace

std::vector<ToolbarGizmoRow> gizmo_rows(const ToolbarLayout& layout)
{
    std::vector<ToolbarGizmoRow> rows;
    for (const std::string& id : layout.gizmos.order) {
        const int group = group_of(layout, id);
        if (group < 0) {
            rows.push_back({ToolbarRowKind::Plain, id, -1});
            continue;
        }
        if (layout.groups[static_cast<size_t>(group)].members.front() == id)
            rows.push_back({ToolbarRowKind::Header, {}, group});
        rows.push_back({ToolbarRowKind::Member, id, group});
    }
    return rows;
}

bool group_gizmos(ToolbarLayout& layout, const std::vector<std::string>& ids, const std::string& name)
{
    const std::string clean = trimmed(name);
    if (ids.size() < 2 || clean.empty() || !ungrouped_bar_ids(layout, ids))
        return false;
    layout.groups.push_back({clean, ids});
    normalize_gizmo_groups(layout);
    return true;
}

bool add_to_group(ToolbarLayout& layout, int group, const std::vector<std::string>& ids)
{
    if (!valid_group(layout, group) || ids.empty() || !ungrouped_bar_ids(layout, ids))
        return false;
    std::vector<std::string>& order = layout.gizmos.order;
    std::vector<std::string>  moved;
    for (const std::string& id : order)
        if (contains(ids, id))
            moved.push_back(id);
    order.erase(std::remove_if(order.begin(), order.end(), [&ids](const std::string& id) { return contains(ids, id); }), order.end());
    ToolbarGizmoGroup& target = layout.groups[static_cast<size_t>(group)];
    order.insert(std::next(std::find(order.begin(), order.end(), target.members.back())), moved.begin(), moved.end());
    target.members.insert(target.members.end(), moved.begin(), moved.end());
    normalize_gizmo_groups(layout);
    return true;
}

bool ungroup_gizmos(ToolbarLayout& layout, int group)
{
    if (!valid_group(layout, group))
        return false;
    layout.groups.erase(layout.groups.begin() + group);
    return true;
}

bool rename_group(ToolbarLayout& layout, int group, const std::string& name)
{
    const std::string clean = trimmed(name);
    if (!valid_group(layout, group) || clean.empty())
        return false;
    layout.groups[static_cast<size_t>(group)].name = clean;
    return true;
}

std::string default_group_name(const ToolbarLayout& layout, const std::function<std::string(int)>& name_for)
{
    for (int n = 1;; ++n) {
        std::string name = name_for(n);
        if (std::none_of(layout.groups.begin(), layout.groups.end(), [&name](const ToolbarGizmoGroup& g) { return g.name == name; }))
            return name;
    }
}

bool move_gizmo_row(ToolbarLayout& layout, const ToolbarGizmoRow& row, int step)
{
    if (step != -1 && step != 1)
        return false;
    std::vector<std::string>& order = layout.gizmos.order;

    if (row.kind == ToolbarRowKind::Member) {
        if (!valid_group(layout, row.group))
            return false;
        std::vector<std::string>& members = layout.groups[static_cast<size_t>(row.group)].members;
        const auto at = std::find(members.begin(), members.end(), row.id);
        if (at == members.end())
            return false;
        const std::ptrdiff_t to = (at - members.begin()) + step;
        if (to < 0 || to >= static_cast<std::ptrdiff_t>(members.size())) {
            // At the group's edge the member leaves; it already sits just outside on that side.
            members.erase(at);
        } else {
            const auto a = std::find(order.begin(), order.end(), row.id);
            const auto b = std::find(order.begin(), order.end(), members[static_cast<size_t>(to)]);
            std::iter_swap(a, b);
        }
        normalize_gizmo_groups(layout);
        return true;
    }

    // Plain rows and headers move as blocks: a group is one block, any other id is a block of one.
    std::vector<std::vector<std::string>> blocks;
    for (const std::string& id : order) {
        const int group = group_of(layout, id);
        if (group >= 0 && layout.groups[static_cast<size_t>(group)].members.front() != id)
            continue;
        blocks.push_back(group >= 0 ? layout.groups[static_cast<size_t>(group)].members : std::vector<std::string>{id});
    }
    const std::string key  = row.kind == ToolbarRowKind::Header ? (valid_group(layout, row.group) ? layout.groups[static_cast<size_t>(row.group)].members.front() : std::string()) : row.id;
    const auto        from = std::find_if(blocks.begin(), blocks.end(), [&key](const std::vector<std::string>& block) { return !key.empty() && block.front() == key; });
    if (from == blocks.end() || (row.kind == ToolbarRowKind::Plain && from->size() != 1))
        return false;
    const std::ptrdiff_t to = (from - blocks.begin()) + step;
    if (to < 0 || to >= static_cast<std::ptrdiff_t>(blocks.size()))
        return false;
    std::iter_swap(from, blocks.begin() + to);
    order.clear();
    for (const std::vector<std::string>& block : blocks)
        order.insert(order.end(), block.begin(), block.end());
    normalize_gizmo_groups(layout);
    return true;
}

}} // namespace Slic3r::GUI
