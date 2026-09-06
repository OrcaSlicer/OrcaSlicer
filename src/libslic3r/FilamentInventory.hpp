#ifndef slic3r_FilamentInventory_hpp_
#define slic3r_FilamentInventory_hpp_

#include <map>
#include <string>
#include <vector>

namespace Slic3r {

// A single physical filament known to a printer: either the filament currently loaded into a
// tool (slot 0 of that tool's list, see FilamentInventory::tools) or a swappable filament
// recorded for later use on that tool.
struct PhysicalFilament
{
    int         id   = 0;        // stable per-machine id, > 0; 0 = invalid/unassigned
    std::string color;           // "#RRGGBB" (may be empty = unknown)
    std::string type;            // e.g. "PLA" (may be empty = unknown)
    std::string preset;          // exact filament preset name (may be empty = profile unknown)
    enum class Kind { Manual, Mmu } kind = Kind::Manual; // Mmu reserved for future AMS-style slots
    bool empty() const { return color.empty() && type.empty() && preset.empty(); }
};

// Alias retained for source compatibility with call sites not yet reworked to the multi-slot
// model (see the physical-filaments-merge plan); refers to the same type.
using LoadedFilament = PhysicalFilament;

// The set of physical filaments known to a printer, grouped by tool. Element 0 of each tool's
// list is always present and is the currently loaded filament for that tool (possibly empty());
// further elements are swappable filaments recorded for that tool.
struct FilamentInventory
{
    std::vector<std::vector<PhysicalFilament>> tools; // index = physical tool
    int next_id = 1;                                   // id allocator for new entries, persisted

    std::string serialize() const;
    // Tolerant: malformed or short input yields an inventory of one empty loaded slot per tool,
    // sized to tool_count.
    static FilamentInventory deserialize(const std::string& s, size_t tool_count);

    // Lookup helpers used by dialog/engine plumbing.
    const PhysicalFilament* find(int id) const; // nullptr if absent
    int tool_of(int id) const;                  // 0-based tool index, -1 if absent

    // Mint an id for every non-empty slot that lacks one (id <= 0), flooring next_id above every
    // id already in use first. GUI write paths MUST call this before save_filament_inventories:
    // a non-empty slot persisted with id 0 (e.g. a loaded row first filled on a fresh inventory)
    // would be renumbered by deserialize's reconcile pass on the NEXT load, silently invalidating
    // every plate filament_physical_map entry that pointed at it. Also zeroes the id of any EMPTY
    // slot that still carries one (e.g. a row the editor just cleared, which keeps the id it was
    // loaded with -- see FilamentInventoryEditor::on_ok): find()/tool_of() key on id alone and
    // don't check empty(), so a lingering id would let a stale stored reference (an old plate's
    // filament_physical_map entry, or a durable SlotAssignment) keep resolving to a slot that no
    // longer represents any physical filament. compute_physical_map_proposal's own !pf->empty()
    // guard already covers this for the row's proposal, but a cleared slot should round-trip as
    // canonically empty (id 0) for every consumer, not rely on each one re-deriving that guard.
    void ensure_ids();

    // Pads tools with single-empty-slot entries up to tool_count; existing tools (and any extra
    // beyond tool_count) are never dropped.
    void ensure_tool_count(size_t tool_count);

    // Orca: write-through seam for a single tool's loaded slot (always favor what's reported
    // from the printer -- FilamentInventoryEditor::
    // do_sync_from_printer calls this immediately for every tool a sync actually covers, instead
    // of waiting for the dialog's OK, so the mapping dialog/auto-mapper/summary always mirror the
    // LAST sync rather than whatever was last saved). Overwrites tools[tool_idx][0] with `slot`
    // (pass a default-constructed PhysicalFilament for a printer-reported empty tray); every
    // other tool, and this tool's own swappable rows (index 1+), are left untouched. Grows tools
    // first via ensure_tool_count if tool_idx is out of range. The caller must still call
    // ensure_ids() afterward (same as any other inventory mutation) before saving.
    void apply_synced_loaded_slot(size_t tool_idx, const PhysicalFilament& slot);
};

// All known physical filament inventories, one per printer preset (an inventory follows the same
// thing Orca's own connection settings follow, the printer preset -- print_host/printhost_apikey
// are preset options, and one preset per physical machine is already Orca's own convention).
// Persisted as a single JSON blob (see serialize/deserialize): serialize() always writes the
// versioned envelope {"version": 1, "presets": {<preset name> -> FilamentInventory::serialize()
// output, ...}}. deserialize() also accepts the pre-versioning shape (a bare object keyed
// directly by preset name, no "version"/"presets" wrapper) and treats it as this same content --
// an object is only read as the envelope when it carries an integer "version" AND an object
// "presets"; anything else, including a legacy store whose preset names happen to collide with
// those two words, is read as the bare legacy shape (see deserialize's own comment).
struct FilamentInventories
{
    std::map<std::string, FilamentInventory> by_preset; // printer preset name -> inventory
    // Set by deserialize() when `s` could not be read as either shape above (invalid JSON, or a
    // non-object top level). by_preset is empty in that case. Callers that persist store back to
    // the same key MUST check this first and preserve the original `s` (e.g. to a backup key)
    // before doing so, or a transient/corrupt read silently destroys the saved data on next save.
    bool parse_error = false;

    std::string                serialize() const;
    static FilamentInventories deserialize(const std::string& s); // malformed -> empty store, parse_error = true

    // Resolves the inventory for `printer_preset_name`, creating an empty one on first use. Pads
    // the returned inventory to at least tool_count tools (never truncates). This is the pure
    // core of Slic3r::GUI::current_inventory_for_preset (src/slic3r/GUI/FilamentInventoryStore.hpp),
    // which resolves the preset name from a Preset and delegates here -- kept in libslic3r so it
    // is unit-testable without a Preset/GUI dependency.
    FilamentInventory& for_preset(const std::string& printer_preset_name, size_t tool_count);
};

// Strip the vendor and material-type tokens (case-insensitive, whole tokens) from a filament
// preset display name, returning the product-line remainder -- the inverse of the
// "<Vendor> <Type> <SubType>" naming convention. "Snapmaker PLA SnapSpeed" (vendor Snapmaker,
// type PLA) -> "SnapSpeed"; "Generic PLA" -> "".
std::string derive_filament_subtype(const std::string& display_name, const std::string& vendor, const std::string& type);

// True when nothing is recorded anywhere in the inventory (every slot, loaded and swappable, on
// every tool is empty()) -- a fresh machine profile that was never populated. Used both to decide
// whether the mapping dialog can target real physical filaments or must fall back to
// picking a bare tool (FilamentMapRowsPanel's bootstrap mode), and to gate FilamentMapDialog's
// "record these as loaded" offer: checking every slot (not just slot 0/loaded) matters because a
// tool can have an empty loaded slot but a real swappable one (cleared via the Physical Filaments
// editor's per-tool Clear button, which only touches slot 0) -- treating that as "all unset"
// would let the record write silently discard the swappable entry.
bool inventory_all_unset(const FilamentInventory& inv);

// A single durable slot assignment for one physical device in one project: project filament
// filament_idx (0-based) is routed to physical tool `tool` (0-based), slot `slot` within that
// tool's PhysicalFilament list (0 = the tool's loaded slot). Persisted on the Model, keyed by
// printer preset name (the same key FilamentInventories uses), so a toolchanger's tray mapping
// survives closing and reopening the project even before the device reconnects.
struct SlotAssignment
{
    int filament_idx = 0;
    int tool         = 0;
    int slot         = 0;
};

// Serializes a device-key -> assignment-list map to JSON:
// {"<device_key>": [{"filament":0,"tool":1,"slot":0}, ...], ...}
std::string dump_slot_assignments(const std::map<std::string, std::vector<SlotAssignment>>& by_device);
// Tolerant inverse of dump_slot_assignments: malformed JSON, a non-object top level, or an empty
// string all yield an empty map, never throw. An entry missing a required field, or with a
// negative filament/tool/slot, is skipped rather than defaulted -- a fabricated (0,0,0) would look
// like a plausible real assignment instead of surfacing the corruption.
std::map<std::string, std::vector<SlotAssignment>> load_slot_assignments(const std::string& json);

// Builds the mapping dialog's per-row proposal: one entry per plate_filaments entry (same
// order), used to seed FilamentMapRowsPanel. Meaning of an entry depends on whether the
// inventory has anything recorded (see inventory_all_unset):
//  - normal (inventory has real targets): the proposed physical filament id, or 0 for no
//    target. stored_physical_map (full-length, 0-based by filament id) is only consulted when
//    stored_map_confirmed is true (the plate has actually been through a confirmed manual
//    mapping before); a stored entry is only kept as-is when it still resolves to a real,
//    non-empty physical filament -- inventory.find(id) returning a slot whose empty() is true
//    (e.g. a loaded/swap slot the user cleared in the Physical Filaments editor after this
//    plate's map was saved, which keeps its id but drops its content) must fall through to
//    auto-match exactly like a missing id, not be treated as a valid stored target.
//  - bootstrap (inventory has nothing recorded anywhere): there is no physical filament to
//    target, so a confirmed row instead reproposes its previously-stored TOOL
//    (stored_filament_map, full-length 1-based project filament -> tool map), encoded as
//    -(tool) so it can never collide with a real (positive) physical id; 0 still means no
//    proposal. FilamentMapRowsPanel decodes this the same way it independently derives
//    bootstrap mode -- from this same inventory.
// Matching itself (the "normal" branch above) is delegated to auto_map_filaments, the same core
// the slice-time auto-mapper uses -- there is exactly one matching implementation, so a
// filament_presets[id] that exactly equals a slot's PhysicalFilament::preset is trusted the same
// way here as it is at slice time, and PLA/PLA+ (etc, see type_compatible's family table) are
// treated as compatible here too.
// True when a project filament of `project_type` may be routed onto a physical slot of
// `slot_type`: exact match (case/whitespace-insensitive) or membership in the same small,
// explicit material-family table (PLA/PLA+, PETG/PETG+). Empty on either side is NOT
// compatible here -- callers gate their own unknown-type cases. Shared by the auto-matcher
// and the mapping picker UI so "family" means one thing everywhere.
bool type_compatible(const std::string& project_type, const std::string& slot_type);

std::vector<int> compute_physical_map_proposal(
    const std::vector<std::string>& filament_colors,
    const std::vector<std::string>& filament_types,
    const std::vector<std::string>& filament_vendors,
    const std::vector<std::string>& filament_presets,
    const std::vector<int>&         plate_filaments,
    const std::vector<int>&         stored_physical_map,
    const std::vector<int>&         stored_filament_map,
    const FilamentInventory&        inventory,
    bool                             stored_map_confirmed);

// One project filament's identity, as needed by auto_map_filaments -- deliberately narrower than
// the full project filament_colour/filament_type/filament_vendor config lists so the matcher
// itself never touches config machinery (spec: pure, fully unit-testable).
struct ProjectFilamentInfo
{
    std::string type;   // e.g. "PLA", "PLA-CF" (may be empty = unknown)
    std::string color;  // "#RRGGBB" or "#RRGGBBAA" (may be empty = unknown)
    std::string vendor;
    std::string preset; // exact filament preset name (may be empty = unknown); see auto_map_filaments
};

// One ProjectFilamentInfo per project filament (0-based, filament_colors' length is
// authoritative for count), assembled from the raw parallel per-filament lists. The single place
// this assembly happens: the GUI's build_project_filament_info (FilamentInventoryStore) extracts
// these lists from a DynamicPrintConfig and delegates here, and compute_physical_map_proposal
// below builds its ProjectFilamentInfo list the same way -- so the slice-time auto-mapper and the
// dialog's proposal can never disagree over what a project filament looks like.
std::vector<ProjectFilamentInfo> build_project_filament_info(
    const std::vector<std::string>& filament_colors,
    const std::vector<std::string>& filament_types,
    const std::vector<std::string>& filament_vendors,
    const std::vector<std::string>& filament_presets);

// Slice-time auto-mapping result: one entry per project filament (same order/length as the
// `project` argument to auto_map_filaments).
struct AutoMapResult
{
    std::vector<int> filament_map;  // 1-based tool per filament (matches the plate filament_map
                                     // convention); a fallback of 1 for an unmatched entry -- the
                                     // caller decides what to do with it, see `unmatched` below.
    std::vector<int> physical_map;  // physical filament id per filament (from the inventory slot
                                     // that resolved the entry), or -1 when nothing resolved.
    std::vector<int> unmatched;     // indices (into `project`) that resolved to neither a valid
                                     // stored assignment nor a compatible inventory slot.
};

// Slice-time auto-mapper -- also the sole matching core behind
// compute_physical_map_proposal's dialog-facing proposal, so the dialog and slice-time mapping
// always agree. Resolves each project filament to a physical tool by priority: (1) a stored
// SlotAssignment whose tool/slot still exist in `inventory` and are non-empty; (2) the
// color-nearest slot with an exactly-matching, non-empty preset, only if within
// kMaxFamilyColorDistance (or color unknown on either side); (3) the color-nearest
// family-compatible slot (type_compatible) within kMaxFamilyColorDistance, tie-broken by vendor
// then by preferring a loaded slot (index 0) over swappable, then lowest tool index; (4)
// otherwise unmatched -- filament_map keeps its fallback of 1 and the index is recorded in
// `unmatched`. A preset match beyond the color cutoff at tier 2 is treated as absent and falls
// through to tier 3, rather than ever auto-proposing a color it can't stand behind.
// Multiple project filaments may legitimately resolve to the same tool/physical filament -- a
// merge, not an error. Pure: no GUI or config-singleton reads.
AutoMapResult auto_map_filaments(
    const std::vector<ProjectFilamentInfo>& project,
    const FilamentInventory&                inventory,
    const std::vector<SlotAssignment>&      assignments);

// Kinds of problem a MANUAL (already-confirmed) mapping can have against the CURRENT inventory --
// see validate_manual_map below. EmptyTarget is the "impossible" case (the target no longer holds
// any filament at all); FamilyMismatch and GrossColorMismatch are both "confirm before proceeding"
// cases, same as auto_map_filaments would refuse to propose them unasked.
enum class ManualMapViolationKind { EmptyTarget, FamilyMismatch, GrossColorMismatch };

struct ManualMapViolation
{
    int                    filament_index = 0; // index into `project` (0-based)
    ManualMapViolationKind kind           = ManualMapViolationKind::EmptyTarget;
};

// Slice-time re-validation of a MANUAL (fmmManual) mapping against the CURRENT inventory. A
// manual map is a frozen pick from a prior dialog OK; nothing re-checks it
// against the inventory afterward, so a printer sync/edit that empties or repurposes its target
// tool between confirmation and slice previously flowed straight to gcode unnoticed. This shares
// the exact predicates auto_map_filaments uses to decide compatibility (type_compatible's family
// table, the empty() rule, kMaxFamilyColorDistance) -- it does not restate them, so a target this
// validator accepts is one auto_map_filaments itself would have been willing to propose.
//
// For each project filament i, the target physical filament is resolved as:
//   - physical_map[i] > 0: inventory.find(physical_map[i]) (the confirmed physical pick).
//   - otherwise (bootstrap-style manual pick, no physical filament existed to target at
//     confirmation time): filament_map[i]'s tool's loaded slot (tools[filament_map[i]-1][0]).
// A target that fails to resolve, or resolves to an empty() slot, raises EmptyTarget and skips the
// type/color checks below (an empty slot has neither to compare). Otherwise: a known, non-empty
// project type incompatible with the target's (type_compatible false) raises FamilyMismatch; a
// known, non-empty project color further than kMaxFamilyColorDistance from the target's raises
// GrossColorMismatch. A single filament may raise more than one kind. Does not consult
// SlotAssignment -- those inform the auto/bootstrap PATHS to a target, not the validity of a
// target a manual map already names explicitly. Pure: no GUI or config-singleton reads.
std::vector<ManualMapViolation> validate_manual_map(
    const std::vector<ProjectFilamentInfo>& project,
    const std::vector<int>&                 filament_map,  // 1-based tool per filament, same length/order as project
    const std::vector<int>&                 physical_map,  // physical filament id per filament (0/absent = no explicit pick, see resolution above)
    const FilamentInventory&                inventory);

// Stable fingerprint of the parts of `inventory` a user actually confirmed when OKing a manual
// map: the (type, color, preset) tuple of every slot, in tool/slot order, plus the tool/slot
// counts. Deliberately excludes `id`/`next_id` -- an id is a storage detail (e.g. renumbered by
// ensure_ids on save), not something the user looked at when confirming a pairing. Two inventories
// with identical tuples in the same order fingerprint identically; any tuple, tool-count, or
// slot-count difference changes it. Used to gate slice-time re-validation: a plate whose
// stored fingerprint still matches the current inventory needn't be re-validated or re-prompted,
// since the standing confirmation still describes what is physically loaded.
std::string inventory_confirmation_fingerprint(const FilamentInventory& inventory);

// Persistence for inventory_confirmation_fingerprint, following dump_slot_assignments/
// load_slot_assignments' pattern one level deeper: {"<device_key>": {"<plate_index>": "<fingerprint>"}}
// (Model metadata_items key "filament_map_confirmed_inventory", set on the mapping dialog's OK for
// a manual map -- see FilamentMapDialog.cpp). Tolerant: malformed JSON, a non-object top level, or
// a malformed inner value all yield/skip cleanly rather than throw or fabricate a fingerprint.
std::string dump_manual_map_confirmations(const std::map<std::string, std::map<int, std::string>>& by_device_plate);
std::map<std::string, std::map<int, std::string>> load_manual_map_confirmations(const std::string& s);

} // namespace Slic3r

#endif // slic3r_FilamentInventory_hpp_
