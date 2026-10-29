#include "PublishSelection.hpp"

#include "Config.hpp"
#include "FilamentMixer.hpp"
#include "PrintConfig.hpp"
#include "PublishSettings.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace Slic3r {

bool PublishRowId::operator<(const PublishRowId &rhs) const
{
    return std::tie(keys, owner, column, section, kind) < std::tie(rhs.keys, rhs.owner, rhs.column, rhs.section, rhs.kind);
}

void PublishSelection::set_owner_info(int owner, PublishOwnerInfo info)
{
    m_owners[owner] = std::move(info);
}

void PublishSelection::set_owner_enabled(int owner, bool enabled)
{
    m_owner_enabled[owner] = enabled;
}

bool PublishSelection::owner_enabled(int owner) const
{
    const auto it = m_owner_enabled.find(owner);
    return it == m_owner_enabled.end() ? true : it->second;
}

void PublishSelection::set_full_publish(int owner, bool full)
{
    m_full[owner] = full;
}

bool PublishSelection::full_publish(int owner) const
{
    const auto it = m_full.find(owner);
    return it == m_full.end() ? false : it->second;
}

void PublishSelection::set_checked(const PublishRowId &row, bool checked)
{
    m_checked[row] = checked;
}

bool PublishSelection::checked(const PublishRowId &row) const
{
    const auto it = m_checked.find(row);
    return it == m_checked.end() ? false : it->second;
}

void PublishSelection::set_enabled(const PublishRowId &row, bool enabled)
{
    m_enabled[row] = enabled;
}

bool PublishSelection::enabled(const PublishRowId &row) const
{
    const auto it = m_enabled.find(row);
    return it == m_enabled.end() ? true : it->second;
}

void PublishSelection::clear()
{
    m_checked.clear();
    m_enabled.clear();
    m_owners.clear();
    m_owner_enabled.clear();
    m_full.clear();
}

const std::map<int, PublishOwnerInfo> &PublishSelection::owners() const
{
    return m_owners;
}

std::vector<std::string> PublishSelection::published_keys(const std::vector<PublishRowId> &rows) const
{
    std::vector<std::string> out;
    for (const PublishRowId &row : rows) {
        if (row.section == PublishSection::Material || !checked(row) || !enabled(row))
            continue;
        for (const std::string &key : row.keys)
            out.push_back(row.column >= 0 ? key + "#" + std::to_string(row.column) : key);
    }
    return out;
}

std::vector<PublishedMaterialEntry> PublishSelection::material_entries(const std::vector<PublishRowId> &rows) const
{
    std::vector<PublishedMaterialEntry> out;
    for (const auto &owner_info : m_owners) {
        const int               owner = owner_info.first;
        const PublishOwnerInfo &info  = owner_info.second;
        if (!owner_enabled(owner))
            continue;

        PublishedMaterialEntry entry;
        entry.filament_type   = info.filament_type;
        entry.filament_vendor = info.filament_vendor;
        entry.filament_id     = info.filament_id;
        entry.setting_id      = info.setting_id;
        entry.preset_name     = info.preset_name;
        entry.slot            = owner;

        // A mixed slot publishes its definition as a whole: components, ratios and gradient.
        if (info.mixed) {
            for (const std::string &key : publish_mixed_keys())
                entry.keys.emplace_back(key);
            if (!info.colour.empty()) {
                entry.publish_color = true;
                entry.color         = info.colour;
            }
            out.push_back(std::move(entry));
            continue;
        }
        // Full Publish embeds the whole preset; type and colour are implicitly published and the
        // owner's per-key rows are ignored.
        if (full_publish(owner)) {
            entry.full               = true;
            entry.full_keys          = info.full_keys;
            entry.publish_type       = true;
            entry.publish_type_value = normalize_filament_type(info.filament_type);
            if (!info.colour.empty()) {
                entry.publish_color = true;
                entry.color         = info.colour;
            }
            out.push_back(std::move(entry));
            continue;
        }
        for (const PublishRowId &row : rows) {
            if (row.section != PublishSection::Material || row.owner != owner)
                continue;
            if (!checked(row) || !enabled(row))
                continue;
            if (row.kind == PublishRowKind::Color) {
                entry.publish_color = true;
                entry.color         = info.colour;
            } else if (row.kind == PublishRowKind::Type) {
                entry.publish_type       = true;
                entry.publish_type_value = normalize_filament_type(info.filament_type);
            } else {
                // Variant-bearing rows carry their file column; a slot-level row stays plain and
                // publish_normalize_material_entries fills in the resolved column for the rest.
                for (const std::string &key : row.keys)
                    entry.keys.emplace_back(row.column >= 0 ? key + "#" + std::to_string(row.column) : key);
            }
        }
        // Nothing checked at all -> nothing to write.
        if (!entry.keys.empty() || entry.publish_type || entry.publish_color)
            out.push_back(std::move(entry));
    }
    return out;
}

std::vector<unsigned int> publish_mixed_components(const DynamicPrintConfig &project, size_t slot)
{
    const auto *components = project.option<ConfigOptionStrings>("filament_mixed_components");
    if (components == nullptr || slot >= components->size())
        return {};
    return parse_mixed_components(components->values[slot]);
}

std::vector<MixedDependencyIssue> unpublished_mixed_components(const PublishSelection &selection,
                                                              const std::vector<PublishRowId> &rows,
                                                              const DynamicPrintConfig &project)
{
    std::vector<MixedDependencyIssue> out;
    const std::map<int, PublishOwnerInfo> &owners = selection.owners();
    for (const auto &owner_info : owners) {
        const int               mixed_slot = owner_info.first;
        const PublishOwnerInfo &info       = owner_info.second;
        if (!info.mixed || !selection.owner_enabled(mixed_slot))
            continue;
        // Components are 1-based physical filament indices.
        for (const unsigned int component : publish_mixed_components(project, size_t(mixed_slot))) {
            if (component == 0)
                continue;
            const int component_slot = int(component) - 1;
            // Only a component the dialog shows (it registered an owner) can be missing.
            if (owners.count(component_slot) == 0)
                continue;
            if (!selection.owner_enabled(component_slot)) {
                out.push_back({ size_t(mixed_slot), size_t(component_slot), MixedDependencyIssue::Reason::Disabled });
                continue;
            }
            if (selection.full_publish(component_slot))
                continue;
            // Colour never counts: the receiver renders the mix from its own components' colours.
            bool type_checked = false;
            for (const PublishRowId &row : rows) {
                if (row.section != PublishSection::Material || row.owner != component_slot ||
                    row.kind != PublishRowKind::Type)
                    continue;
                if (selection.checked(row) && selection.enabled(row)) {
                    type_checked = true;
                    break;
                }
            }
            if (!type_checked)
                out.push_back({ size_t(mixed_slot), size_t(component_slot),
                                MixedDependencyIssue::Reason::MaterialNotPublished });
        }
    }
    // Deterministic order for the warning rows: by mixed slot, then component slot.
    std::sort(out.begin(), out.end(), [](const MixedDependencyIssue &a, const MixedDependencyIssue &b) {
        return std::tie(a.mixed_slot, a.component_slot) < std::tie(b.mixed_slot, b.component_slot);
    });
    return out;
}

} // namespace Slic3r
