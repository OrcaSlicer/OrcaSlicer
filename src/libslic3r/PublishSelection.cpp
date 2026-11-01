#include "PublishSelection.hpp"

#include "Config.hpp"
#include "FilamentMixer.hpp"
#include "PrintConfig.hpp"
#include "PublishSettings.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
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

void PublishSelection::set_owner_published(int owner, bool published)
{
    m_owner_states[owner].published = published;
}

bool PublishSelection::owner_published(int owner) const
{
    const auto it = m_owner_states.find(owner);
    return it == m_owner_states.end() ? false : it->second.published;
}

void PublishSelection::set_full_publish(int owner, bool full)
{
    m_owner_states[owner].full = full;
}

bool PublishSelection::full_publish(int owner) const
{
    const auto it = m_owner_states.find(owner);
    return it == m_owner_states.end() ? false : it->second.full;
}

void PublishSelection::set_checked(const PublishRowId &row, bool checked)
{
    m_row_states[row].checked = checked;
}

bool PublishSelection::checked(const PublishRowId &row) const
{
    const auto it = m_row_states.find(row);
    return it == m_row_states.end() ? false : it->second.checked;
}

void PublishSelection::set_enabled(const PublishRowId &row, bool enabled)
{
    m_row_states[row].enabled = enabled;
}

bool PublishSelection::enabled(const PublishRowId &row) const
{
    const auto it = m_row_states.find(row);
    return it == m_row_states.end() ? true : it->second.enabled;
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
        out.insert(out.end(), row.keys.begin(), row.keys.end());
    }
    return out;
}

// The values one owner's checked and enabled Material rows carry. Keys, colour and type are
// independent of each other; whether the owner has a value to put in them is the caller's rule.
struct PublishRowValues
{
    std::vector<std::string> keys; // every key row's keys, in row order
    bool                     colour{false};
    bool                     type{false};
};

// Walks the rows once for both rules: owner_publishes asks whether any value is present,
// material_entries takes the keys and the two flags.
static PublishRowValues publish_row_values(const PublishSelection &selection, int owner,
                                           const std::vector<PublishRowId> &rows)
{
    PublishRowValues out;
    for (const PublishRowId &row : rows) {
        if (row.section != PublishSection::Material || row.owner != owner)
            continue;
        if (!selection.checked(row) || !selection.enabled(row))
            continue;
        if (row.kind == PublishRowKind::Color) {
            out.colour = true;
        } else if (row.kind == PublishRowKind::Type) {
            out.type = true;
        } else {
            out.keys.insert(out.keys.end(), row.keys.begin(), row.keys.end());
        }
    }
    return out;
}

bool PublishSelection::owner_publishes(int owner, const std::vector<PublishRowId> &rows) const
{
    const auto owner_info = m_owners.find(owner);
    if (owner_info == m_owners.end())
        return false;
    // A mixed slot publishes its definition as one unit: its publish checkbox is the whole rule.
    if (owner_info->second.mixed)
        return owner_published(owner);
    // Full Publish embeds the whole preset.
    if (full_publish(owner))
        return true;
    // A physical slot publishes through a checked row that carries a value: a key row's keys, or a
    // Color/Type row whose owner value is non-empty.
    const PublishRowValues values = publish_row_values(*this, owner, rows);
    const bool             keys   = !values.keys.empty();
    return keys || (values.colour && !owner_info->second.colour.empty()) ||
           (values.type && !normalize_filament_type(owner_info->second.filament_type).empty());
}

bool PublishSelection::type_row_checked(int owner, const std::vector<PublishRowId> &rows) const
{
    return std::any_of(rows.begin(), rows.end(), [owner, this](const PublishRowId &row) {
        return row.section == PublishSection::Material && row.owner == owner && row.kind == PublishRowKind::Type &&
               checked(row) && enabled(row);
    });
}

std::vector<PublishedMaterialEntry> PublishSelection::material_entries(const std::vector<PublishRowId> &rows) const
{
    std::vector<PublishedMaterialEntry> out;
    for (const auto &owner_info : m_owners) {
        const int               owner = owner_info.first;
        const PublishOwnerInfo &info  = owner_info.second;
        if (!owner_publishes(owner, rows))
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
        // A physical slot's colour and type are their own flagged rows, published independently of
        // its keys, so a slot with key rows only ships no identity. owner_publishes has ensured at
        // least one of keys, colour and type is present.
        PublishRowValues values = publish_row_values(*this, owner, rows);
        // Keys are publish-ready: a bare base key is resolved to its column by
        // publish_normalize_material_entries.
        entry.keys = std::move(values.keys);
        if (values.colour && !info.colour.empty()) {
            entry.publish_color = true;
            entry.color         = info.colour;
        }
        const std::string type_value = normalize_filament_type(info.filament_type);
        if (values.type && !type_value.empty()) {
            entry.publish_type       = true;
            entry.publish_type_value = type_value;
        }
        out.push_back(std::move(entry));
    }
    return out;
}

// The key whose index names the write address: a variant-bearing key when the row has one, else
// the first key, which is then addressed whole.
static const std::string *publish_address_key(const std::vector<std::string> &keys)
{
    for (const std::string &key : keys)
        if (publish_key_index(key) >= 0)
            return &key;
    return keys.empty() ? nullptr : &keys.front();
}

// A scalar option is addressed whole (index 0); a vector option only by an element it holds, so a
// short config yields no address rather than a clamped index.
static bool publish_addressable(const DynamicPrintConfig &config, const std::string &key, int index)
{
    const ConfigOption *option = config.option(publish_base_key(key));
    if (option == nullptr)
        return false;
    const auto *values = dynamic_cast<const ConfigOptionVectorBase *>(option);
    return values == nullptr ? index <= 0 : index >= 0 && size_t(index) < values->size();
}

PublishWriteLocation publish_write_location(const PublishRowId &row,
                                            const DynamicPrintConfig &uncompacted,
                                            const DynamicPrintConfig &resolved,
                                            bool full_owner)
{
    PublishWriteLocation location;
    // A Color/Type requirement row carries no config key: nothing to write.
    if (row.keys.empty() || row.kind != PublishRowKind::Key)
        return location;

    // Full Publish copies the whole preset; its rows are not written one by one, not even a
    // mixed definition key.
    if (row.section == PublishSection::Material && (row.owner < 0 || full_owner))
        return location;

    // A mixed definition is a project-level per-slot array, not a preset key; only a material row
    // names the slot that holds its element.
    const std::set<std::string> &mixed_keys = publish_mixed_keys();
    for (const std::string &key : row.keys)
        if (mixed_keys.count(publish_base_key(key)) != 0) {
            if (row.section != PublishSection::Material)
                return location;
            location.target       = PublishWriteTarget::Project;
            location.owner        = row.owner;
            location.option_index = size_t(row.owner); // the material guard above keeps it non-negative
            return location;
        }

    if (row.section == PublishSection::Material) {
        size_t local = 0;
        if (row.column >= 0) {
            // The 7.1.1 fallback column is the value the printer forces, not the author's choice.
            const PublishResolvedColumn resolved_column = publish_resolved_column(uncompacted, size_t(row.owner));
            if (!resolved_column.exact && resolved_column.file_column == std::optional<size_t>(size_t(row.column)))
                return location;
            // The preset stores one element per variant of its own, so a file column has to be
            // translated; a column the slot does not define has no local index.
            const std::vector<size_t> columns = publish_filament_columns(uncompacted, size_t(row.owner));
            const auto found = std::find(columns.begin(), columns.end(), size_t(row.column));
            if (found == columns.end())
                return location;
            local = size_t(found - columns.begin());
        }
        const std::string *key = publish_address_key(row.keys);
        if (key == nullptr || !publish_addressable(resolved, *key, int(local)))
            return location;
        location.target       = PublishWriteTarget::FilamentPreset;
        location.owner        = row.owner;
        location.option_index = local;
        return location;
    }

    const std::string *key = publish_address_key(row.keys);
    if (key == nullptr)
        return location;
    const int index = publish_key_index(*key);
    if (!publish_addressable(uncompacted, *key, index))
        return location;
    location.target = row.section == PublishSection::Printer ? PublishWriteTarget::PrinterPreset :
                                                              PublishWriteTarget::ProcessPreset;
    location.option_index = index < 0 ? 0 : size_t(index);
    return location;
}

std::vector<unsigned int> publish_mixed_components(const DynamicPrintConfig &project, size_t slot)
{
    const auto *components = project.option<ConfigOptionStrings>("filament_mixed_components");
    if (components == nullptr || slot >= components->size())
        return {};
    return parse_mixed_components(components->values[slot]);
}

std::vector<MixedDependencyIssue> unpublished_mixed_components(const PublishSelection &selection,
                                                              const DynamicPrintConfig &project,
                                                              const std::vector<PublishRowId> &rows)
{
    std::vector<MixedDependencyIssue> out;
    const std::map<int, PublishOwnerInfo> &owners = selection.owners();
    for (const auto &owner_info : owners) {
        const int               mixed_slot = owner_info.first;
        const PublishOwnerInfo &info       = owner_info.second;
        if (!info.mixed || !selection.owner_published(mixed_slot))
            continue;
        if (mixed_slot < 0)
            continue;
        // Components are 1-based physical filament indices; a repeated component is one issue.
        std::set<unsigned int> seen;
        for (const unsigned int component : publish_mixed_components(project, size_t(mixed_slot))) {
            if (component == 0 || !seen.insert(component).second)
                continue;
            const int component_slot = int(component) - 1;
            // Only a component the dialog shows (it registered an owner) can be missing.
            const auto component_owner = owners.find(component_slot);
            if (component_owner == owners.end())
                continue;
            // A mixed slot is not a physical filament: the dialog this replaced skipped it too.
            if (component_owner->second.mixed)
                continue;
            // A component publishes its material through a Type row that carries a value, or its
            // whole preset through Full Publish; checked key rows or an empty type ship no material.
            const bool publishes_type = selection.type_row_checked(component_slot, rows) &&
                                        !normalize_filament_type(component_owner->second.filament_type).empty();
            if (!publishes_type && !selection.full_publish(component_slot)) {
                out.push_back({ size_t(mixed_slot), size_t(component_slot) });
                continue;
            }
        }
    }
    // Deterministic order for the warning rows: by mixed slot, then component slot.
    std::sort(out.begin(), out.end(), [](const MixedDependencyIssue &a, const MixedDependencyIssue &b) {
        return std::tie(a.mixed_slot, a.component_slot) < std::tie(b.mixed_slot, b.component_slot);
    });
    return out;
}

} // namespace Slic3r
