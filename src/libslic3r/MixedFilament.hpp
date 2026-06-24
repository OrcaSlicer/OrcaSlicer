#ifndef slic3r_MixedFilament_hpp_
#define slic3r_MixedFilament_hpp_

#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <utility>

namespace Slic3r {

class PrintObject;

std::vector<int> fill_continuous_layer_range(const std::vector<int> &sorted_layers);

// Represents a virtual "mixed" filament created from physical filaments
// (layer cadence and/or same-layer interleaved stripe distribution). Display
// colour blending uses FilamentMixer  so pair previews better
//  match expected print mixing 
// (for example Blue+Yellow -> Green, Red+Yellow -> Orange, Red+Blue -> Purple). 
// Legacy RYB code is retained in source for reference only.
struct MixedFilament
{
    enum DistributionMode : uint8_t {
        LayerCycle = 0,
        SameLayerPointillisme = 1,
        Simple = 2
    };

    // 1-based physical filament IDs that are combined.
    unsigned int component_a = 1;
    unsigned int component_b = 2;

    // Persistent row identity used to keep painted virtual-tool assignments
    // stable even when the visible mixed-filament list is rebuilt.
    uint64_t stable_id = 0;

    // Layer-alternation ratio.  With ratio_a = 2, ratio_b = 1 the cycle is
    // A, A, B, A, A, B, ...
    int ratio_a = 1;
    int ratio_b = 1;

    // Blend percentage of component B in [0..100].
    int mix_b_percent = 50;

    // Optional manual pattern for this mixed filament.
    // Legacy format (no '/'): each '1'-'9' char is a token.
    //   '1'=>component_a, '2'=>component_b, '3'-'9'=>direct physical IDs.
    // Modern format (with '/'): '/' separates tokens, multi-digit IDs supported.
    //   e.g. "1/10/2/11/12". Comma separates per-perimeter groups.
    std::string manual_pattern;

    // Optional explicit gradient multi-color component list, encoded as
    // compact physical filament IDs (for example "123" -> filaments 1,2,3).
    // Interleaved stripe mode is active for gradient rows only when this list has 3+ IDs.
    std::string gradient_component_ids;
    // Optional explicit multi-color weights aligned with gradient_component_ids.
    // Compact integer list joined by '/': for example "50/25/25".
    std::string gradient_component_weights;

    // Legacy compatibility flag from earlier prototype serialization.
    bool pointillism_all_filaments = false;

    // How this mixed row is distributed:
    // - LayerCycle: one filament per layer based on cadence.
    // - SameLayerPointillisme: split painted masks in XY on each layer.
    int distribution_mode = int(Simple);

    // Optional Local-Z cap for this mixed row. 0 disables the cap.
    int local_z_max_sublayers = 0;

    static constexpr float k_default_gradient_dominant = 0.8f;  // Dominant component ratio
    static constexpr float k_default_gradient_minority = 0.2f;  // Minority component ratio
    static constexpr float k_min_gradient_difference   = 0.05f; // Minimum difference for valid gradient
    
    bool  gradient_enabled = false;
    float gradient_start = k_default_gradient_dominant;
    float gradient_end   = k_default_gradient_minority;

    // Additional XY surface offsets, in mm, applied when this mixed row
    // resolves to component A or B for an entire layer. Positive values
    // contract inward; negative values expand outward.
    float component_a_surface_offset = 0.f;
    float component_b_surface_offset = 0.f;

    // Whether this mixed filament is enabled (available for assignment).
    bool enabled = true;

    // True when this mixed filament row was deleted from UI and should stay hidden.
    bool deleted = false;

    // True when this row was user-created (custom) instead of auto-generated.
    bool custom = false;

    // True when this row originated from an auto-generated pair. This remains
    // true even after editing so delete logic can keep the base auto pair
    // tombstoned instead of letting regeneration resurrect it.
    bool origin_auto = false;

    // UI mode that created this row (-1=unknown/legacy, 0=RATIO, 1=CYCLE, 2=MATCH, 3=GRADIENT).
    int ui_mode = -1;

    // Computed display colour as "#RRGGBB".
    std::string display_color;

    bool operator==(const MixedFilament &rhs) const
    {
        constexpr float k_surface_offset_epsilon = 1e-6f;
        constexpr float k_gradient_epsilon       = 1e-4f;
        return component_a == rhs.component_a &&
               component_b == rhs.component_b &&
               stable_id   == rhs.stable_id   &&
               ratio_a     == rhs.ratio_a     &&
               ratio_b     == rhs.ratio_b     &&
               mix_b_percent == rhs.mix_b_percent &&
               manual_pattern == rhs.manual_pattern &&
               gradient_component_ids == rhs.gradient_component_ids &&
               gradient_component_weights == rhs.gradient_component_weights &&
               pointillism_all_filaments == rhs.pointillism_all_filaments &&
               distribution_mode == rhs.distribution_mode &&
               local_z_max_sublayers == rhs.local_z_max_sublayers &&
               gradient_enabled == rhs.gradient_enabled &&
               std::abs(gradient_start - rhs.gradient_start) <= k_gradient_epsilon &&
               std::abs(gradient_end   - rhs.gradient_end)   <= k_gradient_epsilon &&
               std::abs(component_a_surface_offset - rhs.component_a_surface_offset) <= k_surface_offset_epsilon &&
               std::abs(component_b_surface_offset - rhs.component_b_surface_offset) <= k_surface_offset_epsilon &&
               enabled      == rhs.enabled &&
               deleted      == rhs.deleted &&
               custom       == rhs.custom &&
               origin_auto  == rhs.origin_auto &&
               ui_mode      == rhs.ui_mode;
    }
    bool operator!=(const MixedFilament &rhs) const { return !(*this == rhs); }
};

struct MixedFilamentPreviewSettings
{
    double nominal_layer_height { 0.2 };
    double mixed_lower_bound { 0.04 };
    double mixed_upper_bound { 0.16 };
    double preferred_a_height { 0.0 };
    double preferred_b_height { 0.0 };
    bool   local_z_mode { false };
    bool   local_z_direct_multicolor { false };
    size_t wall_loops { 1 };
};

struct MixedFilamentDisplayContext
{
    size_t                       num_physical { 0 };
    std::vector<std::string>     physical_colors;
    std::vector<double>          nozzle_diameters;
    MixedFilamentPreviewSettings preview_settings;
    bool                         component_bias_enabled { false };
};

int mixed_filament_effective_local_z_preview_mix_b_percent(const MixedFilament               &mf,
                                                           const MixedFilamentPreviewSettings &preview_settings);
bool mixed_filament_supports_bias_apparent_color(const MixedFilament               &mf,
                                                 const MixedFilamentPreviewSettings &preview_settings,
                                                 bool                                bias_mode_enabled);
std::pair<int, int> mixed_filament_apparent_pair_percentages(const MixedFilament               &mf,
                                                             const MixedFilamentPreviewSettings &preview_settings,
                                                             const std::vector<double>          &nozzle_diameters,
                                                             bool                                bias_mode_enabled);
std::string compute_mixed_filament_display_color(const MixedFilament &entry, const MixedFilamentDisplayContext &context);

// ---------------------------------------------------------------------------
// MixedFilamentManager
//
// Owns the list of mixed filaments and provides helpers used by the slicing
// pipeline to resolve virtual IDs back to physical extruders.
//
// Virtual filament IDs are numbered starting at (num_physical + 1).  For a
// 4-extruder printer the first mixed filament has ID 5, the second 6, etc.
// ---------------------------------------------------------------------------
class MixedFilamentManager
{
public:
    MixedFilamentManager() = default;

    static void set_auto_generate_enabled(bool enabled);
    static bool auto_generate_enabled();

    // ---- Auto-generation ------------------------------------------------

    // Rebuild the mixed-filament list from the current set of physical
    // filament colours.  Generates all C(N,2) pairwise combinations.
    // Previous ratio/enabled state is preserved when a combination still
    // exists.
    void auto_generate(const std::vector<std::string> &filament_colours);

    // Remove a physical filament (1-based ID) from the mixed list.
    // Any mixed filament that contains the removed component is deleted.
    // Remaining component IDs are shifted down to stay aligned with physical IDs.
    void remove_physical_filament(unsigned int deleted_filament_id);

    // Add a custom mixed filament.
    void add_custom_filament(unsigned int component_a, unsigned int component_b, int mix_b_percent, const std::vector<std::string> &filament_colours);

    // Remove all custom rows, keep auto-generated ones.
    void clear_custom_entries();

    // Clean up deleted mixed filaments from memory.
    // This should be called after serializing to remove deleted entries that are no longer needed.
    void cleanup_deleted_entries();

    // Recompute cadence ratios from gradient settings.
    // gradient_mode: 0 = Layer cycle weighted, 1 = Height weighted.
    void apply_gradient_settings(int   gradient_mode,
                                 float lower_bound,
                                 float upper_bound,
                                 bool  advanced_dithering = false);

    // Persist mixed rows, including auto/deleted state, into the compact
    // project-settings string.
    std::string serialize_custom_entries();
    void load_custom_entries(const std::string &serialized, const std::vector<std::string> &filament_colours);

    // ---- Pattern string functions -------------------------------------------
    // Normalize a manual mixed-pattern string into canonical form.
    // Format: digits 1-9 for IDs 1-9, [N] for IDs >= 10, comma for group separator.
    // Returns empty string if invalid.
    static std::string normalize_manual_pattern(const std::string &pattern);
    static int         mix_percent_from_manual_pattern(const std::string &pattern);

    // Tokenize a single pattern group (no commas) into token strings.
    // Handles single-digit and bracket ([N]) tokens.
    static std::vector<std::string> split_pattern_group_to_tokens(const std::string &group, size_t num_physical);

    // Map a string token to a physical extruder ID.
    // "1" => component_a, "2" => component_b, "3"+ => direct physical ID.
    static unsigned int physical_filament_from_token(const std::string &token, const MixedFilament &mf, size_t num_physical);

    // Split a normalized pattern string by comma into group strings.
    static std::vector<std::string> split_pattern_groups(const std::string &pattern);

    // ---- Gradient component ID encoding / decoding ------------------------

    // Maximum number of physical filaments supported by the encoding.
    static constexpr size_t kMaxPhysicalFilaments = 64;

    // Encode filament IDs (1-based) to a compact string.
    // Legacy format (all IDs ≤ 9):  concatenated single chars, e.g. "132".
    // Extended format (any ID > 9): '/' separated decimals, e.g. "1/12/3".
    // Single-ID extended uses leading '/' to disambiguate, e.g. "/12".
    static std::string encode_gradient_component_ids(const std::vector<unsigned int> &ids);

    // Decode a gradient_component_ids string to a vector of filament IDs (1-based).
    // Handles both legacy and extended formats.  When num_physical > 0 each ID is
    // validated to be ≤ num_physical.
    static std::vector<unsigned int> decode_gradient_component_ids(const std::string &components,
                                                                   size_t             num_physical = 0);

    // Expand virtual mixed-filament IDs in a sorted/deduplicated vector into
    // their physical component IDs (component_a, component_b, and gradient
    // component IDs).  IDs ≤ num_physical are left unchanged.  The caller is
    // responsible for re-sorting and re-deduplicating after the call.
    void expand_virtual_extruder_ids(std::vector<int> &ids, size_t num_physical) const;

    // Normalize a gradient_component_ids string to canonical form.
    // Canonical form uses legacy encoding when all IDs ≤ 9, extended otherwise.
    static std::string normalize_gradient_component_ids(const std::string &components);

    // ---- Queries --------------------------------------------------------

    // True when `filament_id` (1-based) refers to a mixed filament.
    bool is_mixed(unsigned int filament_id, size_t num_physical) const
    {
        return mixed_index_from_filament_id(filament_id, num_physical) >= 0;
    }

    // Resolve a mixed filament ID to a physical extruder (1-based) for the
    // given layer context. Returns `filament_id` unchanged when it is not a
    // mixed filament.
    unsigned int resolve(unsigned int filament_id,
                         size_t       num_physical,
                         int          layer_index,
                         float        layer_print_z = 0.f,
                         float        layer_height  = 0.f,
                         bool         force_height_weighted = false,
                         const PrintObject* current_object = nullptr) const;
    unsigned int resolve_perimeter(unsigned int filament_id,
                                   size_t       num_physical,
                                   int          layer_index,
                                   int          perimeter_index,
                                   float        layer_print_z = 0.f,
                                   float        layer_height  = 0.f,
                                   bool         force_height_weighted = false,
                                   const PrintObject* current_object = nullptr) const;
    // Resolve the filament ID that should own painted regions on this layer.
    // Modes that require virtual identity later in G-code generation keep the
    // original mixed ID; ordinary mixed rows collapse to the current physical
    // extruder so adjacent same-tool regions can merge.
    unsigned int effective_painted_region_filament_id(unsigned int filament_id,
                                                      size_t       num_physical,
                                                      int          layer_index,
                                                      float        layer_print_z = 0.f,
                                                      float        layer_height  = 0.f,
                                                      float        layer_height_a = 0.f,
                                                      float        layer_height_b = 0.f,
                                                      float        base_layer_height = 0.2f) const;
    float component_surface_offset(unsigned int filament_id,
                                   size_t       num_physical,
                                   int          layer_index,
                                   float        layer_print_z = 0.f,
                                   float        layer_height  = 0.f,
                                   bool         force_height_weighted = false) const;
    std::vector<unsigned int> ordered_perimeter_extruders(unsigned int filament_id,
                                                          size_t       num_physical,
                                                          int          layer_index,
                                                          float        layer_print_z = 0.f,
                                                          float        layer_height  = 0.f,
                                                          bool         force_height_weighted = false) const;

    // Map virtual filament ID (1-based, after physical IDs) to index into
    // m_mixed. Virtual IDs enumerate enabled mixed rows only.
    int mixed_index_from_filament_id(unsigned int filament_id, size_t num_physical) const;

    // Blend N colours using weighted FilamentMixer blending.
    // color_percents: vector of (hex_color, percent) where percents sum to 100.
    static std::string blend_color_multi(
        const std::vector<std::pair<std::string, int>> &color_percents);

    const MixedFilament *mixed_filament_from_id(unsigned int filament_id, size_t num_physical) const;

    // Get all mixed filament indices that depend on a specific physical filament (1-based ID).
    // Returns a vector of indices into m_mixed for mixed filaments that use the physical filament
    // as a component (either component_a, component_b, or in gradient_component_ids).
    std::vector<size_t> mixed_filaments_using_physical(unsigned int physical_filament_1based) const;

    // Compute a display colour by blending two colours with FilamentMixer.
    static std::string blend_color(const std::string &color_a,
                                   const std::string &color_b,
                                   int ratio_a, int ratio_b);
    static float max_component_surface_offset_mm(float reference_width_mm = 0.4f);
    static float max_pair_bias_mm(float reference_width_mm = 0.4f);
    static std::pair<float, float> surface_offset_pair_from_signed_bias(float bias_mm,
                                                                        float reference_width_mm = 0.4f);
    static float bias_ui_value_from_surface_offsets(float component_a_surface_offset,
                                                    float component_b_surface_offset,
                                                    float reference_width_mm = 0.4f);
    static int apparent_mix_b_percent(int   mix_b_percent,
                                      float component_a_surface_offset,
                                      float component_b_surface_offset,
                                      float reference_width_mm = 0.4f);

    // Exposed for unit testing — pure logic helpers.
    static int         safe_mod(int x, int m);
    static void        normalize_ratio_pair(int &a, int &b);
    static float       canonical_signed_bias_value(float component_a_surface_offset, float component_b_surface_offset);
    static std::string format_surface_offset_token(float value);
    static double      mixed_filament_reference_nozzle_mm(unsigned int component_a, unsigned int component_b, const std::vector<double> &nozzle_diameters);

    // ---- Accessors ------------------------------------------------------

    const std::vector<MixedFilament> &mixed_filaments() const { return m_mixed; }
    std::vector<MixedFilament>       &mixed_filaments()       { return m_mixed; }

    size_t enabled_count() const;

    // Total filament count = num_physical + number of *enabled* mixed filaments.
    size_t total_filaments(size_t num_physical) const { return num_physical + enabled_count(); }

    // Return the display colours of all enabled mixed filaments (in order).
    std::vector<std::string> display_colors() const;
    void set_display_context(const MixedFilamentDisplayContext &context);

private:
    // Convert a 1-based virtual ID to a 0-based index into m_mixed.
    size_t index_of(unsigned int filament_id, size_t num_physical) const
    {
        return static_cast<size_t>(filament_id - num_physical - 1);
    }

    void refresh_display_colors(const std::vector<std::string> &filament_colours);
    uint64_t allocate_stable_id();
    uint64_t normalize_stable_id(uint64_t stable_id);

    std::vector<MixedFilament> m_mixed;
    int                        m_gradient_mode       = 0;
    float                      m_height_lower_bound  = 0.04f;
    float                      m_height_upper_bound  = 0.16f;
    bool                       m_advanced_dithering  = false;
    uint64_t                   m_next_stable_id      = 1;
    MixedFilamentDisplayContext m_display_context;
};

// Returns true when the mixed filament represents a simple two-color gradient
// that can be rendered as a vertical color ramp (no manual pattern, exactly 2 components).
inline bool is_simple_gradient(const MixedFilament& mf)
{
    // Lightweight ID count without heap allocation.
    // Canonical form: legacy "12" = two IDs, extended "1/12/3" = three IDs.
    auto count_ids = [](const std::string& s) -> size_t {
        if (s.empty()) return 0;
        if (s.find('/') != std::string::npos) {
            size_t n = 0;
            bool in_token = false;
            for (char c : s) {
                if (c == '/') {
                    if (in_token) ++n;
                    in_token = false;
                } else {
                    in_token = true;
                }
            }
            if (in_token) ++n;
            return n;
        }
        return s.size();
    };
    return mf.gradient_enabled
        && mf.component_a != mf.component_b
        && MixedFilamentManager::normalize_manual_pattern(mf.manual_pattern).empty()
        && count_ids(mf.gradient_component_ids) < 3;
}

} // namespace Slic3r

#endif /* slic3r_MixedFilament_hpp_ */
