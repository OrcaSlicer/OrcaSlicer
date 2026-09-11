#ifndef SLIC3R_FILAMENT_MIXER_HPP
#define SLIC3R_FILAMENT_MIXER_HPP

#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

// Photoshop-style gradient curve control point in [0,1] x [0,1].
// (x, y) is the anchor position; (m_in, m_out) are optional cubic Hermite tangent
// overrides. NaN means "use the PCHIP-computed default", which is the case for plain
// anchors loaded from old 2-field 3MF projects or freshly added via a quick click.
// A press-and-drag on a curve segment populates m_out of its left anchor and m_in of
// its right anchor so the segment bends without inserting a new anchor.
struct GradientAnchor {
    double x     = 0.0;
    double y     = 0.0;
    double m_in  = std::numeric_limits<double>::quiet_NaN();
    double m_out = std::numeric_limits<double>::quiet_NaN();
};

// Sorted list of GradientAnchor; x in [0,1], y in [kGradientMinRatio, kGradientMaxRatio].
// Empty means "no custom curve" (callers should fall back to the linear range).
struct GradientCurve {
    std::vector<GradientAnchor> points;
    bool empty() const { return points.empty(); }
};

// Reserved blend ratio range. Anchor y values (= component 0's ratio) are constrained
// to this band so the mixed filament never reaches pure 0% / 100% of either physical
// component, which keeps both extruders flowing and avoids degenerate transitions.
// Both the editor and the sampler enforce this clamp.
constexpr double kGradientMinRatio = 0.1;
constexpr double kGradientMaxRatio = 0.9;

// Parse "x0,y0[,m_in0,m_out0]|x1,y1[,m_in1,m_out1]|..." into a GradientCurve.
// (Anchors are pipe-separated; the fields within an anchor are comma-separated.)
// Accepts both the legacy 2-field form (tangents -> NaN) and the new 4-field form
// (empty token or "nan" preserved as NaN). Returns an empty curve when the input is
// empty or unparsable. Points are clamped to [0,1] for (x, y) and re-sorted by x.
GradientCurve parse_gradient_curve(const std::string& s);

// Serialize a GradientCurve back to a string. Emits 4 fields per anchor when any
// tangent override is finite; emits 2 fields when both tangents are NaN so unchanged
// projects stay byte-identical with the legacy format. Returns "" when empty.
std::string serialize_gradient_curve(const GradientCurve& c);

// Sample the curve at t in [0,1] using cubic Hermite with Fritsch-Carlson PCHIP
// default tangents, optionally overridden per anchor via m_in / m_out. Returns the
// clamped end values when t is outside the control point range. Returns 0.5 when the
// curve has fewer than 2 points (a safety fallback; callers should check empty()).
double sample_gradient_curve(const GradientCurve& c, double t);

// Compute Fritsch-Carlson PCHIP default tangents for a sorted-by-x anchor list.
// Result size == pts.size(). Useful for callers that need to know what tangent the
// sampler would synthesize when m_in / m_out are NaN (e.g. the GUI's segment-bend
// interaction that inserts a virtual anchor and reads back the surrounding tangents).
std::vector<double> compute_pchip_default_tangents(const std::vector<GradientAnchor>& pts);

void filament_mixer_lerp(unsigned char r1, unsigned char g1, unsigned char b1,
                         unsigned char r2, unsigned char g2, unsigned char b2,
                         float t,
                         unsigned char* out_r, unsigned char* out_g, unsigned char* out_b);

void filament_mixer_lerp_float(float r1, float g1, float b1,
                               float r2, float g2, float b2,
                               float t,
                               float* out_r, float* out_g, float* out_b);

void filament_mixer_lerp_linear_float(float r1, float g1, float b1,
                                      float r2, float g2, float b2,
                                      float t,
                                      float* out_r, float* out_g, float* out_b);

// Blend two hex colors ("#RRGGBB") by ratio (0.0 ~ 1.0 for color_b).
// Returns "#RRGGBB" string.
std::string blend_color(const std::string& hex_a, const std::string& hex_b, float ratio_b);

// Blend N hex colors by integer weights using polynomial pigment mixing.
// Pairwise accumulation via filament_mixer_lerp. Returns "#RRGGBB".
std::string blend_color_multi(const std::vector<std::string> &hex_colors,
                              const std::vector<int> &weights);

// Parse comma-separated 1-based component IDs, e.g. "1,3" → {1, 3}.
std::vector<unsigned int> parse_mixed_components(const std::string &str);

// Parse comma-separated ratio values, e.g. "0.7,0.3" → {0.7, 0.3}.
// Returns equal ratios (1/n each) when str is empty or invalid.
// Normalizes so the sum equals 1.0.
std::vector<double> parse_mixed_ratios(const std::string &str, size_t n_components);

// Returns true if any element in is_mixed is true.
// ConfigOptionBools stores values as std::vector<unsigned char>.
bool has_any_mixed_filament(const std::vector<unsigned char> &is_mixed);

// Check which mixed filament slots have broken component references.
// Returns 0-based indices of mixed slots whose components reference
// filaments beyond num_physical (i.e., deleted filaments).
std::vector<size_t> check_mixed_filament_integrity(
    const std::vector<unsigned char> &is_mixed,
    const std::vector<std::string>   &comp_strs,
    size_t num_physical);

// Expand mixed filament slots in an extruder list to their physical components.
// Input/output are 0-based indices. Non-mixed slots pass through unchanged.
// Result is sorted and deduplicated.
std::vector<unsigned int> expand_mixed_filaments(
    const std::vector<unsigned int> &extruders_0based,
    const std::vector<unsigned char> &is_mixed,
    const std::vector<std::string>  &comp_strs);

// Remap mixed filament component references after a physical filament is deleted.
// del_1based: the 1-based index of the deleted physical filament.
// For each mixed slot:
//   - if component == del_1based -> replace with 0 (sentinel for deleted/unselected)
//   - if component > del_1based -> decrement by 1
void remap_mixed_components_on_delete(
    const std::vector<unsigned char> &is_mixed,
    std::vector<std::string>         &comp_strs,
    unsigned int                      del_1based);

// Check which mixed filament slots have type-mismatched components.
// filament_types: type strings for physical filaments (0-based, size == num_physical).
// Component IDs in comp_strs are 1-based; the function converts to 0-based to look up types.
// Returns 0-based config indices of mixed slots with mismatched component types.
std::vector<size_t> check_mixed_filament_type_consistency(
    const std::vector<unsigned char> &is_mixed,
    const std::vector<std::string>   &comp_strs,
    const std::vector<std::string>   &filament_types);

// Expand mixed-slot IDs in geometric unprintable sets to their physical component IDs.
// Each set entry that corresponds to a mixed slot is replaced by the slot's component
// IDs (0-based). Non-mixed entries pass through unchanged.
void expand_mixed_slots_in_unprintables(
    std::vector<std::set<int>>        &unprintables,
    const std::vector<unsigned char>  &is_mixed,
    const std::vector<std::string>    &comp_strs);

// Clear any non-empty gradient-curve slot that parses to fewer than 2 control points.
// Heals per-slot arrays corrupted by the legacy "|" separator collision between
// PresetBundle::export_selections / load_selections (which used "|" as the inter-slot
// delimiter) and serialize_gradient_curve / parse_gradient_curve (which use "|" as the
// intra-slot control-point delimiter). Such a round-trip splits a multi-point curve
// across adjacent slots, leaving single-point entries that fail MakerWorld's strict
// "curve needs >= 2 points" check. Clearing them falls back to the linear range.
void sanitize_mixed_gradient_curve_array(std::vector<std::string>& vals);

// Validate mixed-color (混色) parameters. Returns error messages keyed by option name.
// Slot details are included in the message text (1-based slot index).
std::map<std::string, std::string> validate_mixed_filament_params(
    const std::vector<unsigned char> &is_mixed,
    const std::vector<std::string>   &comp_strs,
    const std::vector<std::string>   &ratio_strs,
    const std::vector<unsigned char> &gradient_flags,
    const std::vector<std::string>   &gradient_range_strs,
    const std::vector<std::string>   &gradient_curve_strs);

} // namespace Slic3r

#endif // SLIC3R_FILAMENT_MIXER_HPP
