#pragma once

#include "PublishSettings.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace Slic3r {

enum class PublishSection { Printer, Material, Process };
enum class PublishRowKind { Key, Color, Type };

// One selectable row of the dialog. `keys` holds the row's base config keys (several for a
// multi-option row). `owner` is the filament slot for Material rows, -1 otherwise. `column` is
// the file column for variant-bearing rows, the extruder column for per-extruder rows, and -1
// for slot-level rows (which publish as a plain base key).
struct PublishRowId
{
    std::vector<std::string> keys;
    int                      owner{-1};
    int                      column{-1};
    PublishSection           section{PublishSection::Process};
    PublishRowKind           kind{PublishRowKind::Key};
    bool operator<(const PublishRowId &rhs) const; // ordered so it can key a std::map
};

// Owner-level state and identity the dialog knows when it builds the pages. For a material
// owner `owner` is the 0-based filament slot.
struct PublishOwnerInfo
{
    bool        mixed{false}; // mixed-color slot: publishes its definition as a whole
    std::string filament_type; // raw material family, e.g. "PLA High Speed"
    std::string filament_vendor;
    std::string filament_id;
    std::string setting_id;
    std::string preset_name;
    std::vector<std::string> full_keys; // non-structural filament keys, for Full Publish
    std::string              colour;    // slot colour as "#RRGGBB", may be empty
};

class PublishSelection
{
public:
    void set_owner_info(int owner, PublishOwnerInfo info);
    void set_owner_enabled(int owner, bool enabled);
    bool owner_enabled(int owner) const; // true when never set
    void set_full_publish(int owner, bool full);
    bool full_publish(int owner) const; // false when never set

    void set_checked(const PublishRowId &row, bool checked);
    bool checked(const PublishRowId &row) const; // false when never set
    void set_enabled(const PublishRowId &row, bool enabled);
    bool enabled(const PublishRowId &row) const; // true when never set

    void clear();

    // Registered owners in ascending slot order; unpublished_mixed_components walks it.
    const std::map<int, PublishOwnerInfo> &owners() const;

    // Print/printer rows (section != Material) that are checked and enabled, in the given order.
    std::vector<std::string> published_keys(const std::vector<PublishRowId> &rows) const;

    // One entry per enabled material owner, in ascending slot order.
    std::vector<PublishedMaterialEntry> material_entries(const std::vector<PublishRowId> &rows) const;

private:
    std::map<PublishRowId, bool> m_checked;
    std::map<PublishRowId, bool> m_enabled;
    std::map<int, PublishOwnerInfo> m_owners;
    std::map<int, bool> m_owner_enabled;
    std::map<int, bool> m_full;
};

// Component slots (1-based, as the config stores them) of a mixed filament; empty when the
// project config names none for that slot.
std::vector<unsigned int> publish_mixed_components(const DynamicPrintConfig &project, size_t slot);

std::vector<MixedDependencyIssue> unpublished_mixed_components(const PublishSelection &selection,
                                                              const std::vector<PublishRowId> &rows,
                                                              const DynamicPrintConfig &project);

} // namespace Slic3r
