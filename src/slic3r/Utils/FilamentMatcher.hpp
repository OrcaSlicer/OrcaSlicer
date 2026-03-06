#ifndef __FILAMENT_MATCHER_HPP__
#define __FILAMENT_MATCHER_HPP__

#include <string>

namespace Slic3r {

class Preset;
class PresetCollection;

// Shared filament-to-preset matching for all printer agents.
//
// Each agent populates a FilamentMatchInput with whatever data the printer
// provides, then calls FilamentMatcher::resolve().  The matcher works from three
// pieces of information -- and does not care whether a given printer reports a
// name or a numeric code or a base type for each:
//
//   V  vendor info    : vendor_name and/or vendor_type
//   F  filament info  : filament_name and/or filament_idx and/or tray_type
//   C  color          : color
//   P  prefix         : optional agent+model tag, tried as both "P_..." and "..."
//
// A printer that fills more than one form of V or F (QIDI reports a vendor name
// *and* a numeric id, a filament name *and* an index *and* a base type) produces
// a V[]/F[] list, and every id match tries each V x F combination.
//
// Cascade (most specific -> least specific; C-hat = closest color, exact wins
// at distance 0 so no separate exact-color level is needed). Each level has an
// id-string match and a config-field match; the "a" variants add the prefix:
//
//   Level 1  V + F + Chat   1a id  [P_]V_F_<hex>, nearest       e.g. QD_3_Acme_Inc_PLA_Plus_F0F0F0
//                           1b id  V_F_<hex>, nearest
//                           1c cfg filament_vendor=V & filament_type=T & name, nearest color
//                           1d cfg filament_vendor=V & filament_type=T & name (any color)
//   Level 2  V + F          2a id  [P_]V_F                       e.g. QD_3_Acme_Inc_PLA_Plus  /  QD_3_7_52
//                           2b id  V_F
//                           2c cfg filament_vendor=V & filament_type=T, nearest color
//                           2d cfg filament_vendor=V & filament_type=T (any color)
//   Level 3  F + Chat       3a/3b id [P_]F_<hex>, nearest
//                           3c cfg filament_type=T & name, nearest color
//                           3d cfg filament_type=T & name (any color)
//   Level 4  F              4a/4b id [P_]F
//                           4c cfg filament_type=T, nearest color
//                           4d cfg filament_type=T   (== filament_id_by_type)
//   Level 5  generic        map_type_to_generic_id(tray_type)   e.g. "PLA" -> "OGFL99"
//
// "& name" narrows to presets whose *preset name* carries every token of the
// reported filament name, compared as uppercase alphanumeric tokens with any
// " @printer" suffix dropped.  Tokenizing this way makes word order irrelevant
// ("PLA Matte" matches both "eSun PLA Matte" and "eSun Matte PLA") and lets a
// reported "PLA" line up with a preset named "PLA+".
//
// The id levels and the config levels serve different authors.  filament_id is
// not a PrintConfig option -- there is no GUI field for it, and a preset saved
// from the GUI inherits its parent's id -- so the id levels are reachable only
// by hand-editing profile JSON.  filament_vendor, filament_type and
// default_filament_colour are all GUI-editable, which makes the config levels
// the only matching mechanism available to a user who has not edited files by
// hand.  Both must work; neither may switch the other off.
//
// Rules that keep a coarse base type from masquerading as a specific filament:
//  - tray_type is excluded from the vendor-bearing levels (1 & 2) whenever a
//    filament_name is present, so "Acme PLA Plus" never lands on an "Acme PLA"
//    profile.  It still drives the vendor-less levels 3/4 and the generic 5.
//  - config-field matching only ever keys F on filament_type (profiles carry no
//    "name" field), uses only the string forms of V/F, and carries no prefix.
//    That makes it coarser than an id match, which is why it sits below every id
//    match in the cascade -- a reported filament name is honoured there first.
//
// Color is evidence, never a substitute for it.  Two rules follow from that:
//
//  - A preset that declares no default_filament_colour sits out the
//    closest-color rungs rather than being scored as some default color, and
//    stays eligible on the colorless rungs below.  Scoring colorless presets as
//    black made every preset in a color-less vendor profile set tie at the same
//    distance, so the winner fell out of collection order and the reported color
//    changed nothing at all.
//
//  - Identity outranks color everywhere.  Every rung that knows what a filament
//    *is* -- a reported product name, or an authored filament_id -- is tried
//    before any rung that only knows what color it is.  Color cannot separate
//    two profiles of the same vendor and type, and letting it try makes it pick
//    by the wrong criterion: an "eSun PLA+ Black" profile (#000000) would take
//    an "eSun PLA Matte" spool reported as 060606 away from the matte profile
//    at #202020, purely because black sits nearer.  That is why the color-only
//    config rungs (2c, 4c) wait until after the id rungs at levels 2 and 4.
//
// Examples: QIDI X-Max 4 -> prefix "QD_3", V=[Acme_Inc, 7], F=[PLA_Plus, 52];
//           Snapmaker    -> no prefix, V=[eSUN], F=[PLA-CF] (its config match at
//                           level 1c is the vendor+type closest-color path);
//           Moonraker    -> F=[PLA] only -> config 4c / generic 5.

struct FilamentMatchInput {
    std::string prefix;            // P: agent+model prefix, e.g. "QD_3" (optional)

    // V: vendor info (either or both forms).
    std::string vendor_name;       // Sanitized vendor name, e.g. "Acme_Inc"
    int         vendor_type = -1;  // Numeric vendor id (>= 0 when provided)

    // F: filament info (any subset).
    std::string filament_name;     // Sanitized filament name, e.g. "PLA_Plus"
    int         filament_idx = -1; // Numeric filament index (> 0 when provided)
    std::string tray_type;         // Base material type, e.g. "PLA" / "PLA-CF"

    // C: reported color as RRGGBB or RRGGBBAA hex (optional leading '#').
    std::string color;             // e.g. "FAFAFA" or "1A2B3CFF"
};

namespace FilamentMatcher {

// What a resolve() call concluded.
//
// filament_id is always set; it is what the AMS payload carries as
// tray_info_idx and what every consumer understood before preset_name existed.
//
// preset_name is set only when the match identified one specific preset, and is
// how that preset survives the trip to PresetBundle::sync_ams_list().  A
// filament_id cannot carry the answer on its own: ids are shared in real profile
// data (in one shipped vendor set a single id covers dozens of products across
// several filament types), and every GUI-created preset inherits its parent's
// id, so resolving an id back to a preset can only pick the first of many.  When
// preset_name is empty -- generic fallbacks, or no preset bundle -- consumers
// fall back to looking up filament_id, which is the right behavior for an answer
// that was never about one particular preset.
struct FilamentMatchResult {
    std::string filament_id;
    std::string preset_name;
};

// Resolve the best matching filament preset for the given input, walking the
// cascade above from most to least specific and skipping any level whose inputs
// are missing.
//
// `filaments` is the collection to match against (the caller passes the GUI
// preset bundle's filament collection, or nullptr when none is loaded yet).
// Taking it as a parameter -- rather than reaching for the GUI singleton -- keeps
// the matcher free of GUI state and lets it be unit tested against a synthetic
// collection.
FilamentMatchResult resolve(const PresetCollection* filaments, const FilamentMatchInput& input);

// Sanitize a filament vendor or filament name for use in preset IDs.
// Non-alphanumeric characters become underscores; consecutive underscores
// are collapsed and trailing underscores are stripped.
//   "Acme Inc"     -> "Acme_Inc"      (filament vendor name)
//   "PLA Plus"     -> "PLA_Plus"      (filament name)
//   "PLA-AERO"     -> "PLA_AERO"      (filament name)
//   "TPU-AERO 64D" -> "TPU_AERO_64D"  (filament name)
std::string sanitize_for_id(const std::string& name);

// Find a visible, compatible preset with the given filament_id, or nullptr.
// Matches both system base presets and user presets with custom filament_id.
// Uses case-insensitive comparison so profile authors don't need to match
// exact casing from the printer's filament config.
const Preset* find_visible_preset(const PresetCollection& filaments, const std::string& filament_id);

// Map a filament type string (e.g. "PLA", "ABS") to an OrcaFilamentLibrary
// generic preset ID (e.g. "OGFL99").  Used as the last-resort fallback when
// no preset bundle is available.
std::string map_type_to_generic_id(const std::string& filament_type);

// Resolve a filament name the printer reported ("PLA-CF", "PLA Matte",
// "PAHT-CF") to the most specific filament_type that actually exists in
// `filaments`, or "" when nothing matches.
//
// A candidate type qualifies when every one of its tokens appears in the
// reported name, and the most specific qualifying type wins -- so "PLA-CF"
// resolves to PLA-CF rather than PLA, while "PLA Matte" still resolves to PLA
// because no "Matte" type exists.  Reading the vocabulary out of the collection
// rather than hardcoding it means a material added to the profiles works with no
// code change, and a composite degrades to its base ("PLA-CF" -> "PLA") for a
// user who has no CF profiles installed.
//
// This exists because a hardcoded substring ladder cannot express the
// vocabulary: it returned on the first hit, so "PLA-CF" became "PLA",
// "PAHT-CF" became "PA" and "PC-ABS-FR" became "ABS" -- and since the config
// rungs gate on filament_type, every one of those spools was excluded from its
// own profiles before matching began.
std::string resolve_filament_type(const PresetCollection& filaments, const std::string& reported);

} // namespace FilamentMatcher
} // namespace Slic3r

#endif
