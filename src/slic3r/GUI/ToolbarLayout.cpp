#include "ToolbarLayout.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/trim.hpp>

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
    normalize_toolbar_groups(layout);

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
        for (const ToolbarIconGroup& group : layout.groups)
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

bool is_main_toolbar_id(const std::string& id) { return contains(stock_main_toolbar_ids(), id); }

ToolbarGroupLayout& toolbar_list_of(ToolbarLayout& layout, const std::string& id) { return is_main_toolbar_id(id) ? layout.main : layout.gizmos; }
const ToolbarGroupLayout& toolbar_list_of(const ToolbarLayout& layout, const std::string& id) { return is_main_toolbar_id(id) ? layout.main : layout.gizmos; }

namespace {

// An id that can be in a group: a main toolbar item or a gizmo bar id, never the separator.
bool groupable(const std::string& id) { return id != TOOLBAR_SEPARATOR_ID && (is_main_toolbar_id(id) || contains(stock_gizmo_bar_ids(), id)); }

} // namespace

void normalize_toolbar_groups(ToolbarLayout& layout)
{
    std::set<std::string>         taken;
    std::vector<ToolbarIconGroup> kept;
    for (const ToolbarIconGroup& group : layout.groups) {
        if (group.members.empty())
            continue;
        // A group belongs to its first member's list; members from the other list are dropped.
        const std::vector<std::string>& order = toolbar_list_of(layout, group.members.front()).order;
        ToolbarIconGroup                clean{group.name, {}};
        for (const std::string& id : order)
            if (contains(group.members, id) && groupable(id) && taken.count(id) == 0)
                clean.members.push_back(id);
        if (clean.members.size() < 2)
            continue;
        taken.insert(clean.members.begin(), clean.members.end());
        kept.push_back(std::move(clean));
    }

    // Each group's members follow its first member, in their existing relative order.
    for (const ToolbarIconGroup& group : kept) {
        std::vector<std::string>&      order = toolbar_list_of(layout, group.members.front()).order;
        const std::vector<std::string> rest(group.members.begin() + 1, group.members.end());
        order.erase(std::remove_if(order.begin(), order.end(), [&rest](const std::string& id) { return contains(rest, id); }), order.end());
        order.insert(std::next(std::find(order.begin(), order.end(), group.members.front())), rest.begin(), rest.end());
    }

    // The main toolbar's groups first, then the gizmo bar's, each in bar order.
    const auto position = [&layout](const ToolbarIconGroup& group) {
        const std::vector<std::string>& order = toolbar_list_of(layout, group.members.front()).order;
        return std::make_pair(!is_main_toolbar_id(group.members.front()), std::find(order.begin(), order.end(), group.members.front()) - order.begin());
    };
    std::stable_sort(kept.begin(), kept.end(), [&position](const ToolbarIconGroup& a, const ToolbarIconGroup& b) { return position(a) < position(b); });
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

// Ids that can join a group: distinct groupable ids of one list, in its order, in no group.
bool ungrouped_bar_ids(const ToolbarLayout& layout, const std::vector<std::string>& ids)
{
    for (size_t i = 0; i < ids.size(); ++i)
        if (!groupable(ids[i]) || is_main_toolbar_id(ids[i]) != is_main_toolbar_id(ids.front()) ||
            !contains(toolbar_list_of(layout, ids[i]).order, ids[i]) || group_of(layout, ids[i]) >= 0 ||
            std::find(ids.begin(), ids.begin() + static_cast<std::ptrdiff_t>(i), ids[i]) != ids.begin() + static_cast<std::ptrdiff_t>(i))
            return false;
    return true;
}

} // namespace

std::vector<ToolbarRow> toolbar_rows(const ToolbarLayout& layout, const ToolbarGroupLayout& list)
{
    std::vector<ToolbarRow> rows;
    for (const std::string& id : list.order) {
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

bool group_icons(ToolbarLayout& layout, const std::vector<std::string>& ids, const std::string& name)
{
    const std::string clean = boost::trim_copy_if(name, boost::is_any_of(" \t\r\n"));
    if (ids.size() < 2 || clean.empty() || !ungrouped_bar_ids(layout, ids))
        return false;
    layout.groups.push_back({clean, ids});
    normalize_toolbar_groups(layout);
    return true;
}

bool add_to_group(ToolbarLayout& layout, int group, const std::vector<std::string>& ids)
{
    if (!valid_group(layout, group) || ids.empty() || !ungrouped_bar_ids(layout, ids) ||
        is_main_toolbar_id(ids.front()) != is_main_toolbar_id(layout.groups[static_cast<size_t>(group)].members.front()))
        return false;
    std::vector<std::string>& order = toolbar_list_of(layout, ids.front()).order;
    std::vector<std::string>  moved;
    for (const std::string& id : order)
        if (contains(ids, id))
            moved.push_back(id);
    order.erase(std::remove_if(order.begin(), order.end(), [&ids](const std::string& id) { return contains(ids, id); }), order.end());
    ToolbarIconGroup& target = layout.groups[static_cast<size_t>(group)];
    order.insert(std::next(std::find(order.begin(), order.end(), target.members.back())), moved.begin(), moved.end());
    target.members.insert(target.members.end(), moved.begin(), moved.end());
    normalize_toolbar_groups(layout);
    return true;
}

bool ungroup_icons(ToolbarLayout& layout, int group)
{
    if (!valid_group(layout, group))
        return false;
    layout.groups.erase(layout.groups.begin() + group);
    return true;
}

bool rename_group(ToolbarLayout& layout, int group, const std::string& name)
{
    const std::string clean = boost::trim_copy_if(name, boost::is_any_of(" \t\r\n"));
    if (!valid_group(layout, group) || clean.empty())
        return false;
    layout.groups[static_cast<size_t>(group)].name = clean;
    return true;
}

std::string default_group_name(const ToolbarLayout& layout, const std::function<std::string(int)>& name_for)
{
    for (int n = 1;; ++n) {
        std::string name = name_for(n);
        if (std::none_of(layout.groups.begin(), layout.groups.end(), [&name](const ToolbarIconGroup& g) { return g.name == name; }))
            return name;
    }
}

bool move_toolbar_row(ToolbarLayout& layout, const ToolbarRow& row, int step)
{
    if (step != -1 && step != 1)
        return false;
    // The row's list: a header's is its group's.
    const std::string anchor = row.kind == ToolbarRowKind::Header ? (valid_group(layout, row.group) ? layout.groups[static_cast<size_t>(row.group)].members.front() : std::string()) : row.id;
    if (anchor.empty())
        return false;
    std::vector<std::string>& order = toolbar_list_of(layout, anchor).order;

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
        normalize_toolbar_groups(layout);
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
    const auto from = std::find_if(blocks.begin(), blocks.end(), [&anchor](const std::vector<std::string>& block) { return block.front() == anchor; });
    if (from == blocks.end() || (row.kind == ToolbarRowKind::Plain && from->size() != 1))
        return false;
    const std::ptrdiff_t to = (from - blocks.begin()) + step;
    if (to < 0 || to >= static_cast<std::ptrdiff_t>(blocks.size()))
        return false;
    std::iter_swap(from, blocks.begin() + to);
    order.clear();
    for (const std::vector<std::string>& block : blocks)
        order.insert(order.end(), block.begin(), block.end());
    normalize_toolbar_groups(layout);
    return true;
}

std::vector<ToolbarBarSlot> compute_bar_slots(const std::vector<std::string>&       bar_ids,
                                                    const std::vector<ToolbarIconGroup>& groups,
                                                    const std::string&                    open_id,
                                                    const std::vector<std::string>&       recent_ids,
                                                    const std::set<std::string>&          cannot_open)
{
    // Each group's members on the bar; a group with fewer than two is no group here.
    std::vector<std::vector<std::string>> on_bar(groups.size());
    for (size_t g = 0; g < groups.size(); ++g) {
        for (const std::string& id : bar_ids)
            if (contains(groups[g].members, id))
                on_bar[g].push_back(id);
        if (on_bar[g].size() < 2)
            on_bar[g].clear();
    }
    const auto group_on_bar = [&on_bar](const std::string& id) {
        for (size_t g = 0; g < on_bar.size(); ++g)
            if (contains(on_bar[g], id))
                return static_cast<int>(g);
        return -1;
    };

    std::vector<ToolbarBarSlot> slots;
    for (const std::string& id : bar_ids) {
        const int g = group_on_bar(id);
        if (g < 0) {
            slots.push_back({id, {}, -1});
            continue;
        }
        const std::vector<std::string>& members = on_bar[static_cast<size_t>(g)];
        if (id != members.front())
            continue;
        const auto openable = [&cannot_open](const std::string& m) { return cannot_open.count(m) == 0; };
        const auto is_member = [&members](const std::string& m) { return contains(members, m); };
        // The open member, else the first that can be opened of those used recently then the rest, else the first of them.
        std::vector<std::string> candidates;
        std::copy_if(recent_ids.begin(), recent_ids.end(), std::back_inserter(candidates), is_member);
        candidates.insert(candidates.end(), members.begin(), members.end());
        const auto it = std::find_if(candidates.begin(), candidates.end(), openable);
        slots.push_back({is_member(open_id) ? open_id : it != candidates.end() ? *it : candidates.front(), members, g});
    }
    return slots;
}

std::string bar_slot_shown_for(const std::vector<ToolbarBarSlot>& slots, const std::string& id)
{
    for (const ToolbarBarSlot& slot : slots)
        if (slot.shown == id || contains(slot.members, id))
            return slot.shown;
    return {};
}

}} // namespace Slic3r::GUI
