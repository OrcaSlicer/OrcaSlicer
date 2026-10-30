#pragma once

#include "PublishSettings.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace Slic3r {

enum class PublishSection { Printer, Material, Process };
// A row's publish target. A Key row publishes its config keys; a physical owner's Color row
// publishes its colour, and its Type row publishes the normalised material type. Both flag rows
// carry no keys and are independent of each other.
enum class PublishRowKind { Key, Color, Type };

// One selectable row of the dialog. `keys` holds publish-ready keys: "base" for a slot-level row
// and "base#col" for a variant-bearing or per-extruder row, several for a multi-option row.
// `owner` is the filament slot for Material rows, -1 otherwise. `column` is the file column the
// row was built for; it is used for identity, seeding and the owner notice, never spliced into a
// key.
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
    // "Publish this owner's identity". A mixed slot's publish checkbox publishes the mix
    // definition as a whole; a physical slot's colour and type are row-driven and ignore this.
    // Row keys are published independently of it.
    void set_owner_published(int owner, bool published);
    bool owner_published(int owner) const; // false when never set
    void set_full_publish(int owner, bool full);
    bool full_publish(int owner) const; // false when never set

    void set_checked(const PublishRowId &row, bool checked);
    bool checked(const PublishRowId &row) const; // false when never set
    void set_enabled(const PublishRowId &row, bool enabled);
    bool enabled(const PublishRowId &row) const; // true when never set

    // Registered owners in ascending slot order; unpublished_mixed_components walks it.
    const std::map<int, PublishOwnerInfo> &owners() const;

    // Print/printer rows (section != Material) that are checked and enabled, in the given order.
    std::vector<std::string> published_keys(const std::vector<PublishRowId> &rows) const;

    // One entry per publishing owner, in ascending slot order. A mixed owner publishes when its
    // publish checkbox is on. A physical owner publishes when Full Publish is on or a checked row
    // carries a value: a key row's keys, or a Color/Type row whose owner value is non-empty. Its
    // Color and Type rows publish the corresponding flag, its key rows the keys.
    std::vector<PublishedMaterialEntry> material_entries(const std::vector<PublishRowId> &rows) const;

    // Whether `owner` contributes an entry: the rule material_entries applies. A mixed owner counts
    // when published, a Full owner always, and a physical owner only when a checked row carries a
    // key or a non-empty colour/type value.
    bool owner_publishes(int owner, const std::vector<PublishRowId> &rows) const;

    // Whether one of `owner`'s Type rows in `rows` is checked and enabled. Only the rows the dialog
    // shows count; the owner still needs a non-empty type value to publish one.
    bool type_row_checked(int owner, const std::vector<PublishRowId> &rows) const;

private:
    std::map<PublishRowId, bool> m_checked;
    std::map<PublishRowId, bool> m_enabled;
    std::map<int, PublishOwnerInfo> m_owners;
    std::map<int, bool> m_owner_published;
    std::map<int, bool> m_full;
};

// Where an edit to a publish row has to be written so the project's real presets carry it (spec
// 7.10). `owner` is the filament slot for FilamentPreset and Project, -1 otherwise.
// `option_index` is the index in the target preset's vector for the row's key: a material row's
// local variant index, the column of a "base#N" printer or process row, and 0 for a value that has
// no per-column dimension. For Project it is the element of the per-slot array the row addresses.
enum class PublishWriteTarget { None, FilamentPreset, PrinterPreset, ProcessPreset, Project };

struct PublishWriteLocation
{
    PublishWriteTarget target { PublishWriteTarget::None };
    int                owner { -1 };
    size_t             option_index { 0 };
};

// Where an edit to `row` must be written. `uncompacted` is the full config the dialog was built
// from (full_config(false)): it holds the file column layout, the per-key vector widths and the
// 7.1.1 fallback resolution. `resolved` is the owner's own resolved config (Preset::config), the
// preset the edit lands in, so it bounds the local variant index; pass the same inputs the dialog
// has. `full_owner` is true when the row's owner has Full Publish.
//
// None for a row with no writable owner: no keys, a non-Key row kind, a mixed definition key on a
// non-material row, a Full Publish material owner, a material row whose column is the 7.1.1
// fallback, and any key the target preset cannot address at that index. A mixed definition key
// resolves to Project, which names its slot in `owner` and its element in `option_index`. A
// material "base#column" row resolves to its slot's filament preset at the local variant index of
// that file column (not the file column itself); a material row without a column resolves to
// index 0.
PublishWriteLocation publish_write_location(const PublishRowId &row,
                                            const DynamicPrintConfig &uncompacted,
                                            const DynamicPrintConfig &resolved,
                                            bool full_owner);

// Component slots (1-based, as the config stores them) of a mixed filament; empty when the
// project config names none for that slot.
std::vector<unsigned int> publish_mixed_components(const DynamicPrintConfig &project, size_t slot);

// Publishing mixed owners whose component filaments publish no material. A component satisfies
// the mix when one of its Type rows in `rows` is checked and its type is non-empty, or it is
// Full-Published; only a component with neither, and that is not itself a mixed slot, is an issue.
// A component the dialog does not show (no registered owner) is never reported.
std::vector<MixedDependencyIssue> unpublished_mixed_components(const PublishSelection &selection,
                                                              const DynamicPrintConfig &project,
                                                              const std::vector<PublishRowId> &rows);

} // namespace Slic3r
