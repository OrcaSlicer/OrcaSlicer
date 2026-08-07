#include "libslic3r/IMEXHelpers.hpp"

#include <algorithm>
#include <numeric>
#include <cassert>
#include <cctype>
#include <set>
#include <sstream>
#include <unordered_set>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/PresetBundle.hpp"

namespace Slic3r {

ConfigOptionInts effective_physical_extruder_map(const ConfigOptionInts* explicit_pem, int nozzle_count)
{
    // One entry per logical extruder, so a profile has authored a map only when its length
    // matches. The single-element PrintConfig default does not, and is replaced rather than
    // treated as a one-extruder machine.
    if (explicit_pem && nozzle_count > 0 && (int) explicit_pem->values.size() == nozzle_count)
        return *explicit_pem;

    ConfigOptionInts derived;
    derived.values.resize(std::max(nozzle_count, 1));
    std::iota(derived.values.begin(), derived.values.end(), 0);
    return derived;
}

ConfigOptionInts effective_physical_extruder_map(const PresetBundle& pb)
{
    const ConfigOptionInts* explicit_pem = pb.project_config.option<ConfigOptionInts>("physical_extruder_map");
    if (!explicit_pem || explicit_pem->values.size() < 2)
        explicit_pem = pb.printers.get_edited_preset().config.option<ConfigOptionInts>("physical_extruder_map");
    const auto* nozzles = pb.printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter");
    return effective_physical_extruder_map(explicit_pem, nozzles ? (int) nozzles->values.size() : 0);
}

int imex_pem_tool_for(int filament_id, const std::string& parallel_mode, const ConfigOptionInts& pem)
{
    const bool imex_parallel = !parallel_mode.empty() && parallel_mode != kImexPrimaryMode;
    if (!imex_parallel || pem.values.empty())
        return -1;
    return pem.get_at(filament_id);
}

bool imex_suppresses_bare_toolchange(const std::string& parallel_mode, unsigned int toolchange_count)
{
    return toolchange_count <= 1
        && !parallel_mode.empty()
        && parallel_mode != kImexPrimaryMode;
}

std::string imex_multicolor_block_reason(const std::string& parallel_mode,
                                         const std::string& active_tools_str,
                                         int tools_per_gantry,
                                         const std::vector<int>& used_filaments_0b,
                                         const ConfigOptionInts& pem)
{
    // Not an IMEX parallel print → nothing to validate here.
    if (parallel_mode.empty() || parallel_mode == kImexPrimaryMode) return {};
    // Single-color prints have no toolchanges to constrain.
    if (used_filaments_0b.size() <= 1) return {};

    // MMU lane sharing detection: two or more used filaments routed to the same
    // physical head means a mid-print MMU swap on that head, which IMEX parallel
    // modes can't slave to the secondary gantry.
    if (!pem.values.empty()) {
        std::unordered_set<int> seen_physicals;
        for (int filament : used_filaments_0b) {
            if (filament < 0 || filament >= (int)pem.values.size()) continue;
            const int phys = pem.get_at(filament);
            if (!seen_physicals.insert(phys).second) {
                return "Multi-color prints in IMEX parallel modes require each filament "
                       "to have its own dedicated physical extruder. Two or more of the active "
                       "filaments are routed to the same physical head via the printer's "
                       "physical extruder map (an MMU/AFC manifold), which the slaved gantry "
                       "cannot follow.";
            }
        }
    }

    // Determine which physical head is the primary in this mode.
    const int primary_physical = imex_primary_tool_for_mode(active_tools_str);
    if (primary_physical < 0) {
        return "The active IMEX mode does not define a primary tool, so multi-color "
               "printing cannot be scheduled. Open the printer settings IMEX Modes "
               "editor and assign a Primary role to one tool.";
    }

    // Walk the active tools once: collect the set of distinct gantries spanned by
    // the mode and check for an explicit Span marker on the primary's gantry.
    // Span declares "this tool is the multicolor partner of primary on the same
    // gantry" — without it, two tools on primary's gantry mean two independent
    // copies, not a within-gantry toolchange topology, which can't carry
    // multicolor in a parallel mode.
    const int tpg = std::max(1, tools_per_gantry);
    const int primary_gantry = primary_physical / tpg;
    bool span_on_primary_gantry = false;
    std::set<int> active_gantries;
    for (const auto& [phys, role] : parse_imex_active_tools(active_tools_str)) {
        active_gantries.insert(phys / tpg);
        if (phys / tpg == primary_gantry && role == ImexRole::Span)
            span_on_primary_gantry = true;
    }

    // Single-gantry mode: the active tools all sit on one gantry, so no second
    // gantry is being copied/mirrored to. The "Copy"/"Mirror" label is decorative
    // — there's no actual parallel printing happening, just a regular multi-tool
    // single-gantry print. Multi-color in this configuration is a misconfiguration
    // (use Primary mode for that), and the user's mode_gcode would still emit
    // parallel-print firmware setup that doesn't apply here.
    if (active_gantries.size() < 2) {
        return "Multi-color in this mode isn't a parallel-print scenario — all of the "
               "active tools sit on a single gantry, so there's no second gantry being "
               "copied or mirrored to. Switch the plate to Primary mode for multi-color "
               "printing on a single gantry, or define an IMEX mode that includes "
               "tools on a second gantry.";
    }

    // Dual-gantry mode but no Span tool on the primary's gantry — the slicer would
    // emit toolchanges between filaments but the mode doesn't declare a within-gantry
    // multicolor partner, so the swap can't carry. The user might intend independent
    // copies (each gantry tool prints its own object) which is incompatible with
    // mid-print multicolor in a parallel mode.
    if (!span_on_primary_gantry) {
        return "Multi-color prints in IMEX parallel modes require a Span tool on the "
               "primary's gantry — without one, the mode doesn't declare a within-gantry "
               "multicolor partner. Either reduce the print to a single filament, switch to "
               "Primary mode, or open the printer settings IMEX Modes editor and mark a "
               "tool on the primary's gantry as Span.";
    }
    return {};
}

int first_filament_for_physical_head(const ConfigOptionInts& pem, int physical)
{
    const auto& v = pem.values;
    if (v.empty())
        return (physical == 0) ? 0 : -1; // degenerate: treat as identity on head 0
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == physical)
            return static_cast<int>(i);
    }
    return -1;
}

bool has_mmu(const ConfigOptionInts& pem)
{
    if (pem.values.size() < 2)
        return false;
    std::unordered_set<int> seen;
    for (int pv : pem.values) {
        if (!seen.insert(pv).second)
            return true;
    }
    return false;
}

bool has_non_primary_mmu(const ConfigOptionInts& pem, int primary_physical)
{
    if (pem.values.size() < 2)
        return false;
    std::unordered_set<int> seen;
    for (int pv : pem.values) {
        if (pv == primary_physical)
            continue;
        if (!seen.insert(pv).second)
            return true;
    }
    return false;
}

std::vector<std::pair<int, ImexRole>> parse_imex_active_tools(const std::string& active_tools_for_mode)
{
    std::vector<std::pair<int, ImexRole>> out;
    if (active_tools_for_mode.empty()) return out;
    std::istringstream ss(active_tools_for_mode);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        tok.erase(std::remove_if(tok.begin(), tok.end(), ::isspace), tok.end());
        if (tok.empty()) continue;
        const auto colon = tok.find(':');
        const std::string idx_str = (colon == std::string::npos) ? tok : tok.substr(0, colon);
        int phys = -1;
        try { phys = std::stoi(idx_str); } catch (...) { continue; }
        if (phys < 0 || phys >= (int)MAXIMUM_EXTRUDER_NUMBER) continue;
        ImexRole role = ImexRole::Copy;
        if (colon != std::string::npos) {
            const std::string r = tok.substr(colon + 1);
            if      (r == "P") role = ImexRole::Primary;
            else if (r == "M") role = ImexRole::Mirror;
            else if (r == "S") role = ImexRole::Span;
            // "C" and anything else → Copy.
        }
        out.emplace_back(phys, role);
    }
    return out;
}

ImexGantryGrouping group_imex_active_tools_by_gantry(const std::string& active_tools_for_mode,
                                                     int                tools_per_gantry)
{
    ImexGantryGrouping out;
    const int tpg = std::max(1, tools_per_gantry);

    const int primary = imex_primary_tool_for_mode(active_tools_for_mode);
    if (primary < 0)
        return out;
    out.primary_phys   = primary;
    out.primary_gantry = primary / tpg;

    const int primary_col = primary % tpg;

    // Bucket every active tool by its gantry index. Skip the primary entry itself
    // since it carries no role-driven semantics for grouping (it's just the source).
    std::map<int, std::vector<std::pair<int, ImexRole>>> by_gantry;
    bool span_seen = false;
    for (const auto& [phys, role] : parse_imex_active_tools(active_tools_for_mode)) {
        if (phys < 0) continue;
        const int g = phys / tpg;
        by_gantry[g].emplace_back(phys, role);
        if (g == out.primary_gantry && role == ImexRole::Span)
            span_seen = true;
    }
    out.span_on_primary = span_seen;

    for (auto& [g, tools] : by_gantry) {
        ImexGantryGroup grp;
        grp.gantry_index = g;
        grp.tools        = tools;

        // Pick representative: column-paired to primary if active, else lowest phys.
        const int paired_phys = g * tpg + primary_col;
        auto pick = std::find_if(tools.begin(), tools.end(),
                                 [&](const auto& pr) { return pr.first == paired_phys; });
        if (pick == tools.end())
            pick = std::min_element(tools.begin(), tools.end(),
                                    [](const auto& a, const auto& b) { return a.first < b.first; });
        grp.representative_phys = pick->first;
        grp.representative_role = pick->second;

        // Aggregate iff Span is on primary's gantry, this is a non-primary gantry,
        // it has ≥2 tools, and all those tools share the same role (so the merged
        // visualization is unambiguous). Mixed-role gantries fall back to per-tool.
        const bool same_role = std::all_of(tools.begin(), tools.end(),
            [&](const auto& pr) { return pr.second == grp.representative_role; });
        grp.aggregate = span_seen
                        && g != out.primary_gantry
                        && tools.size() >= 2
                        && same_role;

        out.groups.push_back(std::move(grp));
    }
    return out;
}

int imex_primary_tool_for_mode(const std::string& active_tools_for_mode)
{
    if (active_tools_for_mode.empty())
        return -1;
    std::istringstream ss(active_tools_for_mode);
    std::string token;
    int first_bare = -1;
    while (std::getline(ss, token, ',')) {
        token.erase(std::remove_if(token.begin(), token.end(), ::isspace), token.end());
        if (token.empty())
            continue;
        const auto colon = token.find(':');
        const std::string idx_str = (colon == std::string::npos) ? token : token.substr(0, colon);
        int idx = -1;
        try {
            idx = std::stoi(idx_str);
        } catch (...) {
            continue;
        }
        if (idx < 0)
            continue;
        if (colon == std::string::npos) {
            // Backwards-compat: a bare index is Primary.
            if (first_bare < 0)
                first_bare = idx;
            continue;
        }
        const std::string role = token.substr(colon + 1);
        if (role == "P")
            return idx;
    }
    return first_bare;
}

std::map<int,int> parse_imex_head_filament_map(const std::string& s)
{
    std::map<int,int> result;
    if (s.empty()) return result;
    std::istringstream ss(s);
    std::string token;
    while (std::getline(ss, token, ',')) {
        token.erase(std::remove_if(token.begin(), token.end(), ::isspace), token.end());
        if (token.empty()) continue;
        auto colon = token.find(':');
        if (colon == std::string::npos) continue;
        try {
            int phys = std::stoi(token.substr(0, colon));
            int slot = std::stoi(token.substr(colon + 1));
            if (phys >= 0 && slot >= 1)
                result[phys] = slot;
        } catch (...) {}
    }
    return result;
}

int imex_primary_logical_from_objects(const std::vector<int>&  used_slots_1b,
                                      const ConfigOptionInts&  pem,
                                      int                       primary_physical)
{
    if (pem.values.empty()) return -1;
    const int pem_size = (int)pem.values.size();
    for (int slot_1b : used_slots_1b) {
        const int slot_0b = slot_1b - 1;
        if (slot_0b >= 0 && slot_0b < pem_size && pem.values[slot_0b] == primary_physical)
            return slot_0b;
    }
    return -1;
}

std::vector<int> imex_secondary_logical_slots(const std::vector<int>&   active_physicals,
                                              int                        primary_physical,
                                              const std::map<int,int>&  plate_head_filament_map,
                                              const ConfigOptionInts&   pem)
{
    std::vector<int> out;
    std::unordered_set<int> seen;
    for (int phys : active_physicals) {
        if (phys == primary_physical) continue;
        const int logical = resolve_filament_for_head(plate_head_filament_map, pem, phys);
        if (logical < 0) continue;
        if (seen.insert(logical).second)
            out.push_back(logical);
    }
    return out;
}

int resolve_filament_for_head(const std::map<int,int>& plate_map,
                              const ConfigOptionInts&  pem,
                              int                       physical)
{
    auto it = plate_map.find(physical);
    if (it != plate_map.end()) {
        const int zero_based = it->second - 1;
        if (zero_based >= 0)
            return zero_based;
    }
    return first_filament_for_physical_head(pem, physical);
}

ImexMirrorAxis imex_mirror_axis_for(int primary_phys, int target_phys, int tools_per_gantry)
{
    const int tpg = std::max(1, tools_per_gantry);
    return (target_phys / tpg) == (primary_phys / tpg) ? ImexMirrorAxis::X : ImexMirrorAxis::Y;
}

Transform3d imex_head_transform(int /*primary*/, int /*target*/, ImexRole role,
                                const Vec2d& gantry_offset,
                                const Vec2d& primary_zone_center,
                                ImexMirrorAxis mirror_axis)
{
    switch (role) {
    case ImexRole::Primary:
        return Transform3d::Identity();
    case ImexRole::Copy:
        return Eigen::Translation3d(gantry_offset.x(), gantry_offset.y(), 0.0)
               * Transform3d::Identity();
    case ImexRole::Mirror: {
        const double len2 = gantry_offset.squaredNorm();
        if (len2 < 1e-12)
            return Transform3d::Identity();
        // True reflection about the zone-boundary plane between the two zones: perpendicular
        // to `mirror_axis`, passing through the midpoint of the zone centers along it. The
        // ghost lands at the mirrored position within the target zone — matching where the
        // mirror tool actually prints — and drag reflects across that plane, so the mirrored
        // axis inverts while the other translates 1:1.
        //   X: linear = diag(-1, 1, 1), translation = (2*center.x + off.x, off.y, 0)
        //   Y: linear = diag( 1,-1, 1), translation = (off.x, 2*center.y + off.y, 0)
        // Both have det = -1: a real mirror image, not a 180° rotation (which would be
        // diag(-1,-1,1), det = +1, and would print the primary's part merely turned around).
        Transform3d out = Transform3d::Identity();
        if (mirror_axis == ImexMirrorAxis::Y) {
            out.linear()      = Eigen::DiagonalMatrix<double, 3>(1.0, -1.0, 1.0);
            out.translation() = Vec3d(gantry_offset.x(),
                                      2.0 * primary_zone_center.y() + gantry_offset.y(),
                                      0.0);
        } else {
            out.linear()      = Eigen::DiagonalMatrix<double, 3>(-1.0, 1.0, 1.0);
            out.translation() = Vec3d(2.0 * primary_zone_center.x() + gantry_offset.x(),
                                      gantry_offset.y(),
                                      0.0);
        }
        return out;
    }
    }
    return Transform3d::Identity();
}

Vec2d compute_imex_slice_offset(bool firmware_managed,
                                const std::string& parallel_mode,
                                const std::optional<BoundingBoxf>& primary_zone_box)
{
    if (!firmware_managed)                    return Vec2d::Zero();
    if (parallel_mode.empty())                return Vec2d::Zero();
    if (parallel_mode == kImexPrimaryMode)    return Vec2d::Zero();
    if (!primary_zone_box.has_value())        return Vec2d::Zero();
    return primary_zone_box->center();
}

bool imex_hull_violates_zones(const std::vector<BoundingBoxf3>& zones, const Polygon& hull)
{
    if (hull.points.empty())
        return false;
    for (const auto& box : zones) {
        if (!intersection({box.polygon(true)}, {hull}).empty())
            return true;
    }
    return false;
}

} // namespace Slic3r
