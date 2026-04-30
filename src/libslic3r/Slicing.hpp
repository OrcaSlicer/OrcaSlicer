// Based on implementation by @platsch

#ifndef slic3r_Slicing_hpp_
#define slic3r_Slicing_hpp_

#include <cstring>
#include <map>
#include <set>
#include <type_traits>
#include <vector>

#include "libslic3r.h"
#include "Utils.hpp"
#include "Point.hpp"

namespace Slic3r
{

class PrintConfig;
class PrintObjectConfig;
class ModelConfig;
class ModelObject;
class DynamicPrintConfig;

// Parameters to guide object slicing and support generation.
// The slicing parameters account for a raft and whether the 1st object layer is printed with a normal or a bridging flow
// (using a normal flow over a soluble support, using a bridging flow over a non-soluble support).
struct SlicingParameters
{
	SlicingParameters() = default;

    // Orca: XYZ filament compensation introduced object_shrinkage_compensation
    static SlicingParameters create_from_config(
         const PrintConfig               &print_config,
         const PrintObjectConfig         &object_config,
         coordf_t                         object_height,
         const std::vector<unsigned int> &object_extruders,
         const Vec3d                     &object_shrinkage_compensation);

    // Has any raft layers?
    bool        has_raft() const { return raft_layers() > 0; }
    size_t      raft_layers() const { return base_raft_layers + interface_raft_layers; }

    // Is the 1st object layer height fixed, or could it be varied?
    bool        first_object_layer_height_fixed()  const { return ! has_raft() || first_object_layer_bridging; }

    // Height of the object to be printed. This value does not contain the raft height.
    coordf_t    object_print_z_height() const { return object_print_z_max - object_print_z_min; }
    
    // Height of the object to be printed. This value does not contain the raft height.
     // This value isn't scaled by shrinkage compensation in the Z-axis.
     coordf_t    object_print_z_uncompensated_height() const { return object_print_z_uncompensated_max - object_print_z_min; }

    bool        valid { false };

    // Number of raft layers.
    size_t      base_raft_layers { 0 };
    // Number of interface layers including the contact layer.
    size_t      interface_raft_layers { 0 };

    // Layer heights of the raft (base, interface and a contact layer).
    coordf_t    base_raft_layer_height { 0 };
    coordf_t    interface_raft_layer_height { 0 };
    coordf_t    contact_raft_layer_height { 0 };

	// The regular layer height, applied for all but the first layer, if not overridden by layer ranges
	// or by the variable layer thickness table.
    coordf_t    layer_height { 0 };
    // Minimum / maximum layer height, to be used for the automatic adaptive layer height algorithm,
    // or by an interactive layer height editor.
    coordf_t    min_layer_height { 0 };
    coordf_t    max_layer_height { 0 };
    coordf_t    max_suport_layer_height { 0 };

    // First layer height of the print, this may be used for the first layer of the raft
    // or for the first layer of the print.
    coordf_t    first_print_layer_height { 0 };

    // Thickness of the first layer. This is either the first print layer thickness if printed without a raft,
    // or a bridging flow thickness if printed over a non-soluble raft,
    // or a normal layer height if printed over a soluble raft.
    coordf_t    first_object_layer_height { 0 };

    // If the object is printed over a non-soluble raft, the first layer may be printed with a briding flow.
    bool 		first_object_layer_bridging { false };

    // Soluble interface? (PLA soluble in water, HIPS soluble in lemonen)
    // otherwise the interface must be broken off.
    bool        soluble_interface { false };
    // Gap when placing object over raft.
    coordf_t    gap_raft_object { 0 };
    // Gap when placing support over object.
    coordf_t    gap_object_support { 0 };
    // Gap when placing object over support.
    coordf_t    gap_support_object { 0 };

    // Bottom and top of the printed object.
    // If printed without a raft, object_print_z_min = 0 and object_print_z_max = object height.
    // Otherwise object_print_z_min is equal to the raft height.
    coordf_t    raft_base_top_z { 0 };
    coordf_t    raft_interface_top_z { 0 };
    coordf_t    raft_contact_top_z { 0 };
    // In case of a soluble interface, object_print_z_min == raft_contact_top_z, otherwise there is a gap between the raft and the 1st object layer.
    coordf_t 	object_print_z_min { 0 };
    // This value of maximum print Z is scaled by shrinkage compensation in the Z-axis.
    coordf_t 	object_print_z_max { 0 };
    
    // Orca: XYZ shrinkage compensation
    // This value of maximum print Z isn't scaled by shrinkage compensation.
     coordf_t     object_print_z_uncompensated_max { 0 };
     // Scaling factor for compensating shrinkage in Z-axis.
     coordf_t    object_shrinkage_compensation_z { 0 };
};
static_assert(IsTriviallyCopyable<SlicingParameters>::value, "SlicingParameters class is not POD (and it should be - see constructor).");

// The two slicing parameters lead to the same layering as long as the variable layer thickness is not in action.
inline bool equal_layering(const SlicingParameters &sp1, const SlicingParameters &sp2)
{
    assert(sp1.valid);
    assert(sp2.valid);
    return  sp1.base_raft_layers                    == sp2.base_raft_layers                     &&
            sp1.interface_raft_layers               == sp2.interface_raft_layers                &&
            sp1.base_raft_layer_height              == sp2.base_raft_layer_height               &&
            sp1.interface_raft_layer_height         == sp2.interface_raft_layer_height          &&
            sp1.contact_raft_layer_height           == sp2.contact_raft_layer_height            &&
            sp1.layer_height                        == sp2.layer_height                         &&
            sp1.min_layer_height                    == sp2.min_layer_height                     &&
            sp1.max_layer_height                    == sp2.max_layer_height                     &&
//            sp1.max_suport_layer_height             == sp2.max_suport_layer_height              &&
            sp1.first_print_layer_height            == sp2.first_print_layer_height             &&
            sp1.first_object_layer_height           == sp2.first_object_layer_height            &&
            sp1.first_object_layer_bridging         == sp2.first_object_layer_bridging          &&
            // BBS: following  are not required for equal layer height.
            // Since the z-gap diff may be multiple of layer height.
#if 0
            sp1.soluble_interface                   == sp2.soluble_interface                    &&
            sp1.gap_raft_object                     == sp2.gap_raft_object                      &&
            sp1.gap_object_support                  == sp2.gap_object_support                   &&
            sp1.gap_support_object                  == sp2.gap_support_object                   &&
#endif
            sp1.raft_base_top_z                     == sp2.raft_base_top_z                      &&
            sp1.raft_interface_top_z                == sp2.raft_interface_top_z                 &&
            sp1.raft_contact_top_z                  == sp2.raft_contact_top_z                   &&
            sp1.object_print_z_min                  == sp2.object_print_z_min;
}

typedef std::pair<coordf_t,coordf_t> t_layer_height_range;
typedef std::map<t_layer_height_range, ModelConfig> t_layer_config_ranges;

std::vector<coordf_t> layer_height_profile_from_ranges(
    const SlicingParameters     &slicing_params,
    const t_layer_config_ranges &layer_config_ranges);

std::vector<double> layer_height_profile_adaptive(
    const SlicingParameters& slicing_params,
    const ModelObject& object, float quality_factor);

std::vector<double> layer_height_overhang(
    const SlicingParameters& slicing_params,
    const ModelObject&       object,
    float                    height,
    std::vector<double>      input_height,
    Transform3d              trafo = {});

struct HeightProfileSmoothingParams
{
    unsigned int radius;
    bool keep_min;

    HeightProfileSmoothingParams() : radius(5), keep_min(false) {}
    HeightProfileSmoothingParams(unsigned int radius, bool keep_min) : radius(radius), keep_min(keep_min) {}
};

std::vector<double> smooth_height_profile(
    const std::vector<double>& profile, const SlicingParameters& slicing_params,
    const HeightProfileSmoothingParams& smoothing_params);

enum LayerHeightEditActionType : unsigned int {
    LAYER_HEIGHT_EDIT_ACTION_INCREASE = 0,
    LAYER_HEIGHT_EDIT_ACTION_DECREASE = 1,
    LAYER_HEIGHT_EDIT_ACTION_REDUCE   = 2,
    LAYER_HEIGHT_EDIT_ACTION_SMOOTH   = 3
};

void adjust_layer_height_profile(
    const ModelObject           &model_object,
    const SlicingParameters     &slicing_params,
    std::vector<coordf_t>       &layer_height_profile,
    coordf_t                     z,
    coordf_t                     layer_thickness_delta, 
    coordf_t                     band_width,
    LayerHeightEditActionType    action);
    
std::vector<double> layer_width_profile_adaptive(
    const SlicingParameters& slicing_params,
    const ModelObject&       object,
    std::vector<coordf_t>    layer_height_profile,
    const double             ow_width,
    Transform3d              trafo = {});

// Produce object layers as pairs of low / high layer boundaries, stored into a linear vector.
// The object layers are based at z=0, ignoring the raft layers.
std::vector<coordf_t> generate_object_layers(
    const SlicingParameters     &slicing_params,
    const std::vector<coordf_t> &layer_height_profile,
    bool is_precise_z_height);

// Check whether the layer height profile describes a fixed layer height profile.
bool check_object_layers_fixed(
    const SlicingParameters     &slicing_params,
    const std::vector<coordf_t> &layer_height_profile);

// Produce a 1D texture packed into a 2D texture describing in the RGBA format
// the planned object layers.
// Returns number of cells used by the texture of the 0th LOD level.
int generate_layer_height_texture(
    const SlicingParameters     &slicing_params,
    const std::vector<coordf_t> &layers,
    void *data, int rows, int cols, bool level_of_detail_2nd_level);

namespace Slicing {
	// Minimum layer height for the variable layer height algorithm. Nozzle index is 1 based.
	coordf_t min_layer_height_from_nozzle(const DynamicPrintConfig &print_config, int idx_nozzle);

	// Maximum layer height for the variable layer height algorithm, 3/4 of a nozzle dimaeter by default,
	// it should not be smaller than the minimum layer height.
	// Nozzle index is 1 based.
	coordf_t max_layer_height_from_nozzle(const DynamicPrintConfig &print_config, int idx_nozzle);
} // namespace Slicing

} // namespace Slic3r

namespace cereal
{
	template<class Archive> void serialize(Archive& archive, Slic3r::t_layer_height_range &lhr) { archive(lhr.first, lhr.second); }
}

#endif /* slic3r_Slicing_hpp_ */
