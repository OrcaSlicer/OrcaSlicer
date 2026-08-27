#pragma once

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <Eigen/Geometry>
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"

namespace Slic3r {

class PresetBundle;

// Reserved sentinel name for the always-present, non-deletable Primary mode in
// `imex_mode_names`. Comparison with this constant indicates "no parallel printing
// in effect" — IMEX zone visualization, ghost rendering, secondary-tool PA / temp
// emission, and the 3MF metadata serialization all short-circuit when the active
// plate mode equals this. Treat any value NOT equal to this as a parallel mode.
inline constexpr const char* kImexPrimaryMode = "primary";

// =============================================================================
// PHYSICAL vs LOGICAL extruder indices — read this before adding a new IMEX call site
// =============================================================================
// IMEX has two distinct index spaces and they are NOT interchangeable on MMU/AFC
// printers (where multiple logical filament slots share one physical extruder):
//
//   * PHYSICAL extruder index — identifies a hardware carriage / hotend.
//     Source: `imex_mode_active_tools` ("0:P,1:C,2:M") and `printer_extruder_id`.
//     Use for: user-facing labels (shown as "T0", "T1"...), per-firmware tool
//     qualifiers (Klipper EXTRUDER=extruderN, RRF M572 D<N>, Marlin T<N>), and
//     anything that addresses hardware.
//
//   * LOGICAL filament slot index — identifies one filament in the project.
//     Source: indexing into per-filament arrays — `filament_presets`,
//     `nozzle_temperature_initial_layer`, bed_temp arrays per plate type, etc.
//     Use for: looking up filament data by slot.
//
// On a single-extruder or pure-IDEX printer the two indices coincide. On any
// printer with `physical_extruder_map` size > 1 they diverge. Confusing them
// produces silent wrong-filament behavior — the slicer reads the wrong filament
// preset / temperature for a given carriage with no error or log line.
//
// Translate physical → logical via:
//     resolve_filament_for_head(plate_head_filament_map, pem, physical_idx)
// (or the simpler `first_filament_for_physical_head` if no per-plate override).
//
// The reverse translation (logical → physical) is `pem.get_at(logical_idx)`, already
// encapsulated in `imex_pem_tool_for` for the per-tool-qualifier case.
//
// Past bugs in this class:
//   - GCode PA emission used the inline `pem.get_at(filament_id)` form at two
//     sites (consolidated into `imex_pem_tool_for` in c2492ccc47).
//   - Pre-slice warnings indexed `filament_presets` directly by physical idx,
//     causing wrong-filament names on AFC/MMU layouts (fixed in fbc58d2a1d).
//   - Ghost color resolution had its own inline pem lookup with a stale default
//     (centralized in `effective_physical_extruder_map` in 6a3de6a28f).
//
// New call sites: if you index a per-filament array, you need a logical index.
// If you label a hardware carriage, use the physical index. When in doubt,
// route through one of the helpers below.
// =============================================================================

// physical_extruder_map has one entry per LOGICAL extruder -- the index space of
// nozzle_diameter -- with each value a physical extruder index. It is NOT derivable from
// printer_extruder_id, which is indexed by variant slot (one entry per extruder+variant pair):
// an X1 Carbon has one nozzle and printer_extruder_id {1,1}, an H2D two nozzles and {1,1,2,2,2}.
//
// A profile has authored a map only when its length matches nozzle_count; otherwise the identity
// is returned, which is the same default upstream applies (see Plater.cpp's extruder_map).
ConfigOptionInts effective_physical_extruder_map(const ConfigOptionInts* explicit_pem, int nozzle_count);

// GUI overload: resolves the effective pem from a live PresetBundle using the project_config
// → printer preset fallback. Use this instead of open-coding the lookup at ghost-color,
// tooltip, click-gate and cache-key call sites so they all agree with what the slicer sees.
ConfigOptionInts effective_physical_extruder_map(const PresetBundle& pb);

// Returns the physical extruder index to emit as a per-tool qualifier (PA / temperature)
// for the given filament slot, or -1 when the current IMEX state does not warrant a
// per-tool qualification: non-IMEX / primary mode, or the pem is empty / unpopulated.
// Used at tool-change and second-layer transition call sites where IMEX parallel modes
// route emission through the physical extruder, while ordinary tool changes emit bare
// firmware commands like any non-IMEX printer.
int imex_pem_tool_for(int filament_id, const std::string& parallel_mode, const ConfigOptionInts& pem);

// Translate a logical filament id into the physical heater that an M104/M109 `T` must
// name. Applies in every IMEX mode including Primary -- a heater command names hardware,
// so it does not depend on a parallel mode being active the way imex_pem_tool_for does.
//
// Gated on is_imex because physical_extruder_map carries two readings in this tree: the
// BBL paths index it by extruder id (GCode.cpp:3333, WipeTower.cpp:1353), the IMEX paths
// by filament id. Those coincide only when the filament and nozzle counts match, so an
// ungated mapping would impose the IMEX reading on profiles that mean the other one --
// fdm_bbl_3dp_002_common ships a non-identity [1,0] and is spared today only because
// single_extruder_multi_material suppresses the T qualifier entirely.
//
// Out-of-range ids pass through unchanged, the same rule GCodeProcessor's preheat rewrite
// uses (GCode/GCodeProcessor.cpp:1407), so a printer whose map is the registered
// single-entry default is unaffected.
int imex_physical_heater_for(bool is_imex, const ConfigOptionInts& pem, int logical_id);

// True when GCode::set_extruder should suppress its bare T<n> at the print-start
// initial-tool selection because the active IMEX parallel mode's setup macro
// (imex_mode_gcode) and machine_start_gcode already activate the primary tool
// (Klipper SET_PRINT_MODE / RRF M567 — both reference an active tool by definition),
// so any slicer-emitted T<n> at print-start is a duplicate.
//
// Mid-print toolchanges in parallel mode are intentionally NOT suppressed:
//   - Single-color prints have no mid-print toolchanges anyway
//   - Multi-color prints are blocked at slice-time when the active IMEX configuration
//     can't physically support multi-color (see imex_multicolor_block_reason). The only
//     not-blocked case is IQEX with 2+ tools active on the primary gantry, where mid-
//     print T<n> is legitimately needed and the firmware handles the slaved gantry.
//
// Returns false for: non-IMEX printers (empty parallel_mode), Primary mode, and any
// toolchange after the first (toolchange_count > 1). Custom change_filament_gcode that
// contains its own T<n> is handled separately by GCode.cpp's custom_gcode_changes_tool().
bool imex_suppresses_bare_toolchange(const std::string& parallel_mode, unsigned int toolchange_count);

// Returns a user-facing block reason when multi-color printing is incompatible with
// the active IMEX parallel mode, or an empty string when the configuration is OK.
//
// Multi-color in a parallel mode requires the firmware to swap tools mid-print on the
// primary gantry while the slaved gantry follows automatically. That works only when
// every used filament has its own dedicated physical head (no MMU lane sharing) AND
// the active mode definition explicitly marks a Span tool on the primary's gantry
// (Span declares "this tool is the within-gantry multicolor partner of Primary"; it
// disambiguates the multicolor topology from independent same-gantry copies).
//
// Catches:
//   - IDEX (1 tool per gantry): primary's gantry can never carry a Span partner → blocked
//   - IQEX 2-tool-active (e.g. T0 primary + T2 copy on different gantries): no Span on
//     primary's gantry → blocked
//   - MMU/AFC sharing among used filaments: multiple used filaments routed to same
//     physical head → blocked (the slaved gantry can't follow MMU lane swaps)
//   - IQEX 4-tool independent copies (T0:P,T1:C,T2:M,T3:M): no Span declared, T1 is an
//     independent copy → blocked
//   - IQEX paired-gantry multicolor (T0:P,T1:S,T2:M,T3:M): Span on T1 declares the
//     multicolor partner, no MMU sharing → ALLOWED
//
// Returns empty string for: non-IMEX (empty parallel_mode), Primary mode, single-color
// prints, or any configuration where multi-color is physically supportable.
//
// Inputs:
//   parallel_mode      — m_imex_parallel_mode at slice time
//   active_tools_str   — the imex_mode_active_tools entry for the active mode, e.g.
//                        "0:P,1:C,2:M,3:M"
//   tools_per_gantry   — imex_tools_per_gantry from printer config (>= 1)
//   used_filaments_0b  — 0-based logical filament indices used by the print
//   pem                — physical_extruder_map (logical idx → physical head)
std::string imex_multicolor_block_reason(const std::string& parallel_mode,
                                         const std::string& active_tools_str,
                                         int tools_per_gantry,
                                         const std::vector<int>& used_filaments_0b,
                                         const ConfigOptionInts& pem);

// Returns the lowest 0-based logical filament index L such that pem[L] == physical.
// Returns -1 if no filament routes to `physical`.
// Degenerate case: an empty pem returns 0 when `physical == 0` (identity-on-head-0
// fallback for printers that never configured a pem) and -1 otherwise.
// With identity pem, the caller sees logical == physical behavior because
// position L holds value L.
int first_filament_for_physical_head(const ConfigOptionInts& pem, int physical);

// Returns true if the printer has at least one physical head fed by >= 2 logical
// filaments (i.e., an MMU/AFC is present on some head). Drives the plater's
// click-handler branch: true -> open popover; false -> retain cycle-through.
bool has_mmu(const ConfigOptionInts& pem);

// Returns true if any physical head OTHER than `primary_physical` is fed by
// >= 2 logical filaments. Primary is the head whose filament is already
// controlled by the left sidebar's object->filament assignment, so an MMU
// there needs no popover row; only non-primary MMU heads do.
// Returns false when pem is empty or size < 2.
bool has_non_primary_mmu(const ConfigOptionInts& pem, int primary_physical);

// Parses one mode's entry from `imex_mode_active_tools` and returns the
// 0-based physical T-index carrying the `:P` (Primary) role marker.
// Accepts two forms:
//   "0:P,1:C,2:C"   (explicit role suffix)
//   "0"             (backwards-compat: a bare index == Primary)
// Returns -1 on empty/malformed input or if no primary is found.
// The caller is responsible for indexing into imex_mode_active_tools by mode.
int imex_primary_tool_for_mode(const std::string& active_tools_for_mode);

// Parses the "phys:slot,phys:slot" serialization of imex_head_filament_map.
// Keys are physical T-indices (0-based); values are 1-based filament slots.
// Returns empty map on empty/malformed input.
std::map<int,int> parse_imex_head_filament_map(const std::string& s);

// Resolves which 0-based logical filament to use for a physical head.
// 1. If the plate map overrides `physical`, returns (slot - 1) — 1-based → 0-based.
// 2. Else falls back to first_filament_for_physical_head(pem, physical).
// Returns -1 if neither source yields a valid filament.
int resolve_filament_for_head(const std::map<int,int>& plate_map,
                              const ConfigOptionInts&  pem,
                              int                       physical);

// Returns the 0-based logical filament slot the IMEX *primary* tool prints with,
// based on what the plate's objects are actually assigned to. Walks `used_slots_1b`
// (1-based filament indices, e.g. from PartPlate::get_extruders) and returns the
// first one whose pem entry maps to `primary_physical`. Returns -1 if no object on
// the plate routes to the primary's physical extruder, or if pem is empty.
//
// On non-MMU/non-AFC printers (one logical per physical) this is unambiguous; on
// AFC layouts where multiple logicals route to one physical, the first match wins
// — sufficient for warning labels and is_extruder_used marking. Multi-color
// primaries on the AFC manifold may want all matches; that's a separate iteration.
int imex_primary_logical_from_objects(const std::vector<int>&  used_slots_1b,
                                      const ConfigOptionInts&  pem,
                                      int                       primary_physical);

// Returns the 0-based logical filament slots that IMEX *secondary* carriages
// will load during the print. Iterates `active_physicals` (the set returned by
// imex_mode_active_tools parsing — physical extruder indices), skips entries
// equal to `primary_physical` (the primary is owned by tool_ordering /
// per-object filament assignment, not enumerated here), and resolves each
// remaining physical via `resolve_filament_for_head` (per-plate override
// first, then first_filament_for_physical_head as fallback).
//
// Returned slots are deduplicated and -1 entries (no routing found) are
// dropped. Use this for is_extruder_used marking and pre-slice warnings'
// secondary lookup; the caller still owns whatever it does with the slots
// (mark a bool array, compare temps, etc.).
std::vector<int> imex_secondary_logical_slots(const std::vector<int>&   active_physicals,
                                              int                        primary_physical,
                                              const std::map<int,int>&  plate_head_filament_map,
                                              const ConfigOptionInts&   pem);

enum class ImexRole { Primary, Copy, Mirror, Span };

// Parses `imex_mode_active_tools[mode]` into a list of (physical_head, role) pairs.
// Accepted token forms (comma-separated, whitespace-tolerant):
//   "phys"      — bare index, role defaults to Copy
//   "phys:P"    — Primary
//   "phys:C"    — Copy
//   "phys:M"    — Mirror
//   "phys:S"    — Span (multicolor partner of Primary on the same gantry; only
//                meaningful on tools sharing primary's gantry, and only on a
//                multi-gantry printer. Drives the multicolor-allow path and
//                paired-gantry ghost/zone aggregation.)
//   "phys:???"  — unknown role suffix, treated as Copy
// Malformed tokens (unparseable int, negative phys) are skipped.
// NOTE: the bare-token → Copy default differs from `imex_primary_tool_for_mode`,
// which separately scans for a Primary; callers needing the primary index should
// use that helper. This parser is for call sites that already have the primary
// in hand and need the full head/role list (e.g. ghost factory/updater).
std::vector<std::pair<int, ImexRole>> parse_imex_active_tools(const std::string& active_tools_for_mode);

// Per-gantry grouping derived from the active_tools string. The single source of
// truth for paired-gantry visualization aggregation: when the primary's gantry
// has at least one Span tool, every non-primary gantry's representative tool
// stands in for the whole gantry (one ghost, one zone strip). Gantries without
// the aggregation trigger keep per-tool semantics.
//
// `representative_phys` is the column-paired tool to primary on that gantry —
// i.e. `gantry_index * tools_per_gantry + (primary_phys % tools_per_gantry)` if
// active, else the lowest active phys on that gantry. Aggregation falls back
// when tools on the same non-primary gantry carry mixed roles (e.g. one Copy
// + one Mirror), since the user explicitly authored two distinct topologies.
struct ImexGantryGroup {
    int                                     gantry_index = -1;
    int                                     representative_phys = -1;
    ImexRole                                representative_role = ImexRole::Copy;
    bool                                    aggregate = false;  // collapse this gantry to one ghost/zone
    std::vector<std::pair<int, ImexRole>>   tools;              // every active tool on this gantry
};

struct ImexGantryGrouping {
    int                              primary_phys     = -1;
    int                              primary_gantry   = -1;
    bool                             span_on_primary  = false;  // any Span tool on primary's gantry
    std::vector<ImexGantryGroup>     groups;                    // every gantry with ≥1 active tool, sorted by index
};

// Groups parsed active tools by gantry (`phys / tools_per_gantry`). Aggregation
// triggers iff `span_on_primary` is true AND a non-primary gantry's tools all
// carry the same role; mixed-role non-primary gantries fall back to per-tool.
// Returns an empty grouping when active_tools_str is empty / has no primary.
ImexGantryGrouping group_imex_active_tools_by_gantry(const std::string& active_tools_for_mode,
                                                     int                tools_per_gantry);

// World-space transform composed as `head_xf * primary_instance_world` to place a ghost
// copy of the primary into `target`'s frame under `role`.
// Which boundary a Mirror reflects across. Tools on the primary's own gantry sit beside
// it along X, so they mirror across the vertical boundary between their zones. Tools on a
// different gantry sit in front of / behind it along Y, so they mirror across the
// horizontal boundary between the gantry row strips — the part comes off that gantry as a
// Y-reflection of the tool directly behind it, not an X-reflection. Single-gantry printers
// only ever use X.
enum class ImexMirrorAxis { X, Y };

// The single source of truth for that choice. Gantry row is `phys / tools_per_gantry`, the
// same derivation PartPlate and GCodeViewer use to build their zone grids. Keep every caller
// on this helper: if the three of them ever disagree about the axis, the plate ghosts and the
// preview markers place the same tool in different spots for the same mode.
// `tools_per_gantry` is clamped to >= 1, so a zero/negative config divides safely. Note the
// clamp lands on "one tool per gantry", meaning every secondary is then cross-gantry and
// mirrors on Y — not "everything on one gantry". That is the honest reading of tpg = 1.
ImexMirrorAxis imex_mirror_axis_for(int primary_phys, int target_phys, int tools_per_gantry);

// `gantry_offset` = center_for(target) - center_for(primary) (XY, in mm).
// `primary_zone_center` = XY center of the primary head's zone in world coords. Only
//   consulted for Mirror; Copy/Primary ignore it.
// `mirror_axis` = axis the Mirror reflection negates; get it from imex_mirror_axis_for().
//   Deliberately NOT defaulted: a default would silently hand a forgetful caller the X
//   reflection, which is wrong for every cross-gantry tool and would fail silently — the
//   ghosts would just quietly go back to mirroring on the wrong axis. Ignored by Copy/Primary.
// Copy:    pure translation by gantry_offset. Ghost tracks primary 1:1 during drag.
// Mirror:  true reflection (det = -1, so chirality flips — a real mirror image) about the
//          zone-boundary plane between primary and target, perpendicular to `mirror_axis`
//          and passing through the midpoint of the two zone centers along that axis. The
//          ghost origin lands at the mirrored position within the target zone, and primary
//          drag reflects across that plane, so the mirrored axis moves opposite the primary
//          while the other axis tracks 1:1.
//          Zero-length gantry_offset degenerates to identity.
// Primary: identity.
Transform3d imex_head_transform(int primary, int target, ImexRole role,
                                const Vec2d& gantry_offset,
                                const Vec2d& primary_zone_center,
                                ImexMirrorAxis mirror_axis);

// Slice-time XY shift for printers that delegate copy/mirror placement to firmware.
// When `imex_firmware_managed_zones` is on AND the active mode is non-primary, returns
// the primary zone's plate-local center so gcode emission can subtract it (yielding a
// centered slice the firmware can fan out). Returns Vec2d::Zero() in every other case:
// flag off, primary/empty mode, or no zone box (IMEX disabled, primary-only, etc.).
Vec2d compute_imex_slice_offset(bool firmware_managed,
                                const std::string& parallel_mode,
                                const std::optional<BoundingBoxf>& primary_zone_box);

// True if `hull` (scaled Clipper coords) overlaps any of `zones` (unscaled mm) —
// the one definition of "overlap" shared by the object and prime-tower placement
// checks. Area-based: Clipper yields no result for shapes sharing only an edge, so
// a hull flush against a zone boundary is NOT reported, it has to cross. That
// matches the long-standing object behaviour. An empty hull never violates.
bool imex_hull_violates_zones(const std::vector<BoundingBoxf3>& zones, const Polygon& hull);

} // namespace Slic3r
