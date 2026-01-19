#include <limits>

#include "libslic3r.h"
#include "Slicing.hpp"
#include "SlicingAdaptive.hpp"
#include "PrintConfig.hpp"
#include "Model.hpp"

// #define SLIC3R_DEBUG

// Make assert active if SLIC3R_DEBUG
#ifdef SLIC3R_DEBUG
    #undef NDEBUG
    #define DEBUG
    #define _DEBUG
    #include "SVG.hpp"
    #undef assert 
    #include <cassert>
#endif

namespace Slic3r
{

static const coordf_t MIN_LAYER_HEIGHT = 0.01;
static const coordf_t MIN_LAYER_HEIGHT_DEFAULT = 0.07;
static const double LAYER_HEIGHT_CHANGE_STEP = 0.04;

// Minimum layer height for the variable layer height algorithm.
inline coordf_t min_layer_height_from_nozzle(const PrintConfig &print_config, int idx_nozzle)
{
    coordf_t min_layer_height = print_config.min_layer_height.get_at(idx_nozzle - 1);
    return (min_layer_height == 0.) ? MIN_LAYER_HEIGHT_DEFAULT : std::max(MIN_LAYER_HEIGHT, min_layer_height);
}

// Maximum layer height for the variable layer height algorithm, 3/4 of a nozzle dimaeter by default,
// it should not be smaller than the minimum layer height.
inline coordf_t max_layer_height_from_nozzle(const PrintConfig &print_config, int idx_nozzle)
{
    coordf_t min_layer_height = min_layer_height_from_nozzle(print_config, idx_nozzle);
    coordf_t max_layer_height = print_config.max_layer_height.get_at(idx_nozzle - 1);
    coordf_t nozzle_dmr       = print_config.nozzle_diameter.get_at(idx_nozzle - 1);
    return std::max(min_layer_height, (max_layer_height == 0.) ? (0.75 * nozzle_dmr) : max_layer_height);
}

// Minimum layer height for the variable layer height algorithm.
coordf_t Slicing::min_layer_height_from_nozzle(const DynamicPrintConfig &print_config, int idx_nozzle)
{
    coordf_t min_layer_height = print_config.opt_float("min_layer_height", idx_nozzle - 1);
    return (min_layer_height == 0.) ? MIN_LAYER_HEIGHT_DEFAULT : std::max(MIN_LAYER_HEIGHT, min_layer_height);
}

// Maximum layer height for the variable layer height algorithm, 3/4 of a nozzle dimaeter by default,
// it should not be smaller than the minimum layer height.
coordf_t Slicing::max_layer_height_from_nozzle(const DynamicPrintConfig &print_config, int idx_nozzle)
{
    coordf_t min_layer_height = min_layer_height_from_nozzle(print_config, idx_nozzle);
    coordf_t max_layer_height = print_config.opt_float("max_layer_height", idx_nozzle - 1);
    coordf_t nozzle_dmr       = print_config.opt_float("nozzle_diameter", idx_nozzle - 1);
    return std::max(min_layer_height, (max_layer_height == 0.) ? (0.75 * nozzle_dmr) : max_layer_height);
}

SlicingParameters SlicingParameters::create_from_config(
     const PrintConfig                 &print_config,
     const PrintObjectConfig         &object_config,
     coordf_t                         object_height,
     const std::vector<unsigned int> &object_extruders,
     const Vec3d                     &object_shrinkage_compensation)
{
    coordf_t initial_layer_print_height                      = (print_config.initial_layer_print_height.value <= 0) ? 
        object_config.layer_height.value : print_config.initial_layer_print_height.value;
    // If object_config.support_filament == 0 resp. object_config.support_interface_filament == 0,
    // print_config.nozzle_diameter.get_at(size_t(-1)) returns the 0th nozzle diameter,
    // which is consistent with the requirement that if support_filament == 0 resp. support_interface_filament == 0,
    // support will not trigger tool change, but it will use the current nozzle instead.
    // In that case all the nozzles have to be of the same diameter.
    coordf_t support_material_extruder_dmr           = print_config.nozzle_diameter.get_at(object_config.support_filament.value - 1);
    coordf_t support_material_interface_extruder_dmr = print_config.nozzle_diameter.get_at(object_config.support_interface_filament.value - 1);
    bool     soluble_interface                       = object_config.support_top_z_distance.value == 0.;

    SlicingParameters params;
    params.layer_height = object_config.layer_height.value;
    params.first_print_layer_height = initial_layer_print_height;
    params.first_object_layer_height = initial_layer_print_height;
    params.object_print_z_min = 0.;
    // Orca: XYZ filament compensation
    params.object_print_z_max = object_height * object_shrinkage_compensation.z();
    params.object_print_z_uncompensated_max = object_height;
    params.object_shrinkage_compensation_z = object_shrinkage_compensation.z();
    params.base_raft_layers = object_config.raft_layers.value;
    params.soluble_interface = soluble_interface;

    // Miniumum/maximum of the minimum layer height over all extruders.
    params.min_layer_height = MIN_LAYER_HEIGHT;
    params.max_layer_height = std::numeric_limits<double>::max();
    if (object_config.enable_support.value || params.base_raft_layers > 0 || object_config.enforce_support_layers > 0) {
        // Has some form of support. Add the support layers to the minimum / maximum layer height limits.
        params.min_layer_height = std::max(
            min_layer_height_from_nozzle(print_config, object_config.support_filament), 
            min_layer_height_from_nozzle(print_config, object_config.support_interface_filament));
        params.max_layer_height = std::min(
            max_layer_height_from_nozzle(print_config, object_config.support_filament), 
            max_layer_height_from_nozzle(print_config, object_config.support_interface_filament));
        params.max_suport_layer_height = params.max_layer_height;
    }
    if (object_extruders.empty()) {
        params.min_layer_height = std::max(params.min_layer_height, min_layer_height_from_nozzle(print_config, 0));
        params.max_layer_height = std::min(params.max_layer_height, max_layer_height_from_nozzle(print_config, 0));
    } else {
        for (unsigned int extruder_id : object_extruders) {
            params.min_layer_height = std::max(params.min_layer_height, min_layer_height_from_nozzle(print_config, extruder_id));
            params.max_layer_height = std::min(params.max_layer_height, max_layer_height_from_nozzle(print_config, extruder_id));
        }
    }
    params.min_layer_height = std::min(params.min_layer_height, params.layer_height);
    params.max_layer_height = std::max(params.max_layer_height, params.layer_height);

    if (! soluble_interface) {
        params.gap_raft_object    = object_config.raft_contact_distance.value;
        //BBS
        params.gap_object_support = object_config.support_bottom_z_distance.value; 
        params.gap_support_object = object_config.support_top_z_distance.value;

        if (!print_config.independent_support_layer_height) {
            params.gap_raft_object = std::round(params.gap_raft_object / object_config.layer_height + EPSILON) * object_config.layer_height;
            params.gap_object_support = std::round(params.gap_object_support / object_config.layer_height + EPSILON) * object_config.layer_height;
            params.gap_support_object = std::round(params.gap_support_object / object_config.layer_height + EPSILON) * object_config.layer_height;
        }
    }

    if (params.base_raft_layers > 0) {
		params.interface_raft_layers = (params.base_raft_layers + 1) / 2;
        params.base_raft_layers -= params.interface_raft_layers;
        // Use as large as possible layer height for the intermediate raft layers.
        params.base_raft_layer_height       = std::max(params.layer_height, 0.75 * support_material_extruder_dmr);
        params.interface_raft_layer_height  = std::max(params.layer_height, 0.75 * support_material_interface_extruder_dmr);
        params.first_object_layer_bridging  = false;
        params.contact_raft_layer_height    = std::max(params.layer_height, 0.75 * support_material_interface_extruder_dmr);
        params.first_object_layer_height    = params.layer_height;
    }

    if (params.has_raft()) {
        // Raise first object layer Z by the thickness of the raft itself plus the extra distance required by the support material logic.
        //FIXME The last raft layer is the contact layer, which shall be printed with a bridging flow for ease of separation. Currently it is not the case.
		if (params.raft_layers() == 1) {
            // There is only the contact layer.
            params.contact_raft_layer_height = initial_layer_print_height;
            params.raft_contact_top_z        = initial_layer_print_height;
		} else {
            assert(params.base_raft_layers > 0);
            assert(params.interface_raft_layers > 0);
            // Number of the base raft layers is decreased by the first layer.
            params.raft_base_top_z       = initial_layer_print_height + coordf_t(params.base_raft_layers - 1) * params.base_raft_layer_height;
            // Number of the interface raft layers is decreased by the contact layer.
            params.raft_interface_top_z  = params.raft_base_top_z + coordf_t(params.interface_raft_layers - 1) * params.interface_raft_layer_height;
			params.raft_contact_top_z    = params.raft_interface_top_z + params.contact_raft_layer_height;
		}
        coordf_t print_z = params.raft_contact_top_z + params.gap_raft_object;
        params.object_print_z_min  = print_z;
        params.object_print_z_max += print_z;
        params.object_print_z_uncompensated_max += print_z;
    }

    params.valid = true;
    return params;
}

// Convert layer_config_ranges to layer_height_profile. Both are referenced to z=0, meaning the raft layers are not accounted for
// in the height profile and the printed object may be lifted by the raft thickness at the time of the G-code generation.
std::vector<coordf_t> layer_height_profile_from_ranges(
	const SlicingParameters 	&slicing_params,
	const t_layer_config_ranges &layer_config_ranges)
{
    // 1) If there are any height ranges, trim one by the other to make them non-overlapping. Insert the 1st layer if fixed.
    std::vector<std::pair<t_layer_height_range,coordf_t>> ranges_non_overlapping;
    ranges_non_overlapping.reserve(layer_config_ranges.size() * 4);
    if (slicing_params.first_object_layer_height_fixed())
        ranges_non_overlapping.push_back(std::pair<t_layer_height_range,coordf_t>(
            t_layer_height_range(0., slicing_params.first_object_layer_height), 
            slicing_params.first_object_layer_height));
    // The height ranges are sorted lexicographically by low / high layer boundaries.
    for (t_layer_config_ranges::const_iterator it_range = layer_config_ranges.begin(); it_range != layer_config_ranges.end(); ++ it_range) {
        coordf_t lo = it_range->first.first;
        coordf_t hi = std::min(it_range->first.second, slicing_params.object_print_z_height());
        coordf_t height = it_range->second.option("layer_height")->getFloat();
        if (! ranges_non_overlapping.empty())
            // Trim current low with the last high.
            lo = std::max(lo, ranges_non_overlapping.back().first.second);
        if (lo + EPSILON < hi)
            // Ignore too narrow ranges.
            ranges_non_overlapping.push_back(std::pair<t_layer_height_range,coordf_t>(t_layer_height_range(lo, hi), height));
    }

    // 2) Convert the trimmed ranges to a height profile, fill in the undefined intervals between z=0 and z=slicing_params.object_print_z_max()
    // with slicing_params.layer_height
    std::vector<coordf_t> layer_height_profile;
    auto last_z = [&layer_height_profile]() {
        return layer_height_profile.empty() ? 0. : *(layer_height_profile.end() - 2);
    };
    auto lh_append = [&layer_height_profile](coordf_t z, coordf_t layer_height) {
        if (!layer_height_profile.empty()) {
            bool last_z_matches = is_approx(*(layer_height_profile.end() - 2), z);
            bool last_h_matches = is_approx(layer_height_profile.back(), layer_height);
            if (last_h_matches) {
                if (last_z_matches) {
                    // Drop a duplicate.
                    return;
                }
                if (layer_height_profile.size() >= 4 && is_approx(*(layer_height_profile.end() - 3), layer_height)) {
                    // Third repetition of the same layer_height. Update z of the last entry.
                    *(layer_height_profile.end() - 2) = z;
                    return;
                }
            }
        }
        layer_height_profile.push_back(z);
        layer_height_profile.push_back(layer_height);
    };

    for (const std::pair<t_layer_height_range, coordf_t>& non_overlapping_range : ranges_non_overlapping) {
        coordf_t lo = non_overlapping_range.first.first;
        coordf_t hi = non_overlapping_range.first.second;
        coordf_t height = non_overlapping_range.second;
        if (coordf_t z = last_z(); lo > z + EPSILON) {
            // Insert a step of normal layer height.
            lh_append(z, slicing_params.layer_height);
            lh_append(lo, slicing_params.layer_height);
        }
        // Insert a step of the overriden layer height.
        lh_append(lo, height);
        lh_append(hi, height);
    }

    if (coordf_t z = last_z(); z < slicing_params.object_print_z_uncompensated_height()) {
        // Insert a step of normal layer height up to the object top.
        lh_append(z, slicing_params.layer_height);
        lh_append(slicing_params.object_print_z_uncompensated_height(), slicing_params.layer_height);
    }

   	return layer_height_profile;
}

// Based on the work of @platsch
// Fill layer_height_profile by heights ensuring a prescribed maximum cusp height.
std::vector<double> layer_height_profile_adaptive(const SlicingParameters& slicing_params, const ModelObject& object, float quality_factor)
{
    // 1) Initialize the SlicingAdaptive class with the object meshes.
    SlicingAdaptive as;
    as.set_slicing_parameters(slicing_params);
    as.prepare(object);

    // 2) Generate layers using the algorithm of @platsch 
    std::vector<double> layer_height_profile;
    layer_height_profile.push_back(0.0);
    layer_height_profile.push_back(slicing_params.first_object_layer_height);
    if (slicing_params.first_object_layer_height_fixed()) {
        layer_height_profile.push_back(slicing_params.first_object_layer_height);
        layer_height_profile.push_back(slicing_params.first_object_layer_height);
    }
    double print_z = slicing_params.first_object_layer_height;
    // last facet visited by the as.next_layer_height() function, where the facets are sorted by their increasing Z span.
    size_t current_facet = 0;
    // loop until we have at least one layer and the max slice_z reaches the object height
    while (print_z + EPSILON < slicing_params.object_print_z_height()) {
        float height = slicing_params.max_layer_height;
        // Slic3r::debugf "\n Slice layer: %d\n", $id;
        // determine next layer height
        float cusp_height = as.next_layer_height(float(print_z), quality_factor, current_facet);

#if 0
        // check for horizontal features and object size
        if (this->config.match_horizontal_surfaces.value) {
            coordf_t horizontal_dist = as.horizontal_facet_distance(print_z + height, min_layer_height);
            if ((horizontal_dist < min_layer_height) && (horizontal_dist > 0)) {
                #ifdef SLIC3R_DEBUG
                std::cout << "Horizontal feature ahead, distance: " << horizontal_dist << std::endl;
                #endif
                // can we shrink the current layer a bit?
                if (height-(min_layer_height - horizontal_dist) > min_layer_height) {
                    // yes we can
                    height -= (min_layer_height - horizontal_dist);
                    #ifdef SLIC3R_DEBUG
                    std::cout << "Shrink layer height to " << height << std::endl;
                    #endif
                } else {
                    // no, current layer would become too thin
                    height += horizontal_dist;
                    #ifdef SLIC3R_DEBUG
                    std::cout << "Widen layer height to " << height << std::endl;
                    #endif
                }
            }
        }
#endif
        height = std::min(cusp_height, height);

        // apply z-gradation
        /*
        my $gradation = $self->config->get_value('adaptive_slicing_z_gradation');
        if($gradation > 0) {
            $height = $height - unscale((scale($height)) % (scale($gradation)));
        }
        */
    
        // look for an applicable custom range
        /*
        if (my $range = first { $_->[0] <= $print_z && $_->[1] > $print_z } @{$self->layer_height_ranges}) {
            $height = $range->[2];
    
            # if user set custom height to zero we should just skip the range and resume slicing over it
            if ($height == 0) {
                $print_z += $range->[1] - $range->[0];
                next;
            }
        }
        */
        //BBS: avoid the layer height change to be too steep
        if (layer_height_profile.back() < height && height - layer_height_profile.back() > LAYER_HEIGHT_CHANGE_STEP)
            height = layer_height_profile.back() + LAYER_HEIGHT_CHANGE_STEP;
        else if (layer_height_profile.back() > height && layer_height_profile.back() - height > LAYER_HEIGHT_CHANGE_STEP)
            height = layer_height_profile.back() - LAYER_HEIGHT_CHANGE_STEP;

        for (auto const& [range,options] : object.layer_config_ranges) {
            if ( print_z >= range.first && print_z <= range.second) {
                    height = options.opt_float("layer_height");
                    break;
            };
        };

        layer_height_profile.push_back(print_z);
        layer_height_profile.push_back(height);
        print_z += height;
    }

    double z_gap = slicing_params.object_print_z_height() - *(layer_height_profile.end() - 2);
    if (z_gap > 0.0)
    {
        layer_height_profile.push_back(slicing_params.object_print_z_height());
        layer_height_profile.push_back(std::clamp(z_gap, slicing_params.min_layer_height, slicing_params.max_layer_height));
    }

    return layer_height_profile;
}

std::vector<double> smooth_height_profile(const std::vector<double>& profile, const SlicingParameters& slicing_params, const HeightProfileSmoothingParams& smoothing_params)
{
    auto gauss_blur = [&slicing_params](const std::vector<double>& profile, const HeightProfileSmoothingParams& smoothing_params) -> std::vector<double> {
        auto gauss_kernel = [] (unsigned int radius) -> std::vector<double> {
            unsigned int size = 2 * radius + 1;
            std::vector<double> ret;
            ret.reserve(size);

            // Reworked from static inline int getGaussianKernelSize(float sigma) taken from opencv-4.1.2\modules\features2d\src\kaze\AKAZEFeatures.cpp
            double sigma = 0.3 * (double)(radius - 1) + 0.8;
            double two_sq_sigma = 2.0 * sigma * sigma;
            double inv_root_two_pi_sq_sigma = 1.0 / ::sqrt(M_PI * two_sq_sigma);

            for (unsigned int i = 0; i < size; ++i)
            {
                double x = (double)i - (double)radius;
                ret.push_back(inv_root_two_pi_sq_sigma * ::exp(-x * x / two_sq_sigma));
            }

            return ret;
        };

        // skip first layer ?
        size_t skip_count = slicing_params.first_object_layer_height_fixed() ? 4 : 0;

        // not enough data to smmoth
        if ((int)profile.size() - (int)skip_count < 6)
            return profile;
        
        unsigned int radius = std::max(smoothing_params.radius, (unsigned int)1);
        std::vector<double> kernel = gauss_kernel(radius);
        int two_radius = 2 * (int)radius;

        std::vector<double> ret;
        size_t size = profile.size();
        ret.reserve(size);

        // leave first layer untouched
        for (size_t i = 0; i < skip_count; ++i)
        {
            ret.push_back(profile[i]);
        }

        // smooth the rest of the profile by biasing a gaussian blur
        // the bias moves the smoothed profile closer to the min_layer_height
        double delta_h = slicing_params.max_layer_height - slicing_params.min_layer_height;
        double inv_delta_h = (delta_h != 0.0) ? 1.0 / delta_h : 1.0;

        double max_dz_band = (double)radius * slicing_params.layer_height;
        for (size_t i = skip_count; i < size; i += 2)
        {
            double zi = profile[i];
            double hi = profile[i + 1];
            ret.push_back(zi);
            ret.push_back(0.0);
            double& height = ret.back();
            int begin = std::max((int)i - two_radius, (int)skip_count);
            int end = std::min((int)i + two_radius, (int)size - 2);
            double weight_total = 0.0;
            for (int j = begin; j <= end; j += 2)
            {
                int kernel_id = radius + (j - (int)i) / 2;
                double dz = std::abs(zi - profile[j]);
                if (dz * slicing_params.layer_height <= max_dz_band)
                {
                    double dh = std::abs(slicing_params.max_layer_height - profile[j + 1]);
                    double weight = kernel[kernel_id] * sqrt(dh * inv_delta_h);
                    height += weight * profile[j + 1];
                    weight_total += weight;
                }
            }

            height = std::clamp(weight_total == 0 ? hi : height / weight_total, slicing_params.min_layer_height, slicing_params.max_layer_height);
            if (smoothing_params.keep_min)
                height = std::min(height, hi);
        }

        return ret;
    };

    //BBS: avoid the layer height change to be too steep
    //auto has_steep_height_change = [&slicing_params](const std::vector<double>& profile, const double height_step) {
    //    //BBS: skip first layer
    //    size_t skip_count = slicing_params.first_object_layer_height_fixed() ? 4 : 0;
    //    size_t size = profile.size();
    //    //BBS: not enough data to smmoth, return false directly
    //    if ((int)size - (int)skip_count < 6)
    //        return false;

    //    //BBS: Don't need to check the difference between top layer and the last 2th layer
    //    for (size_t i = skip_count; i < size - 6; i += 2) {
    //        if (abs(profile[i + 1] - profile[i + 3]) > height_step)
    //            return true;
    //    }
    //    return false;
    //};

    int count = 0;
    std::vector<double> ret = profile;
    // bool has_steep_change = has_steep_height_change(ret, LAYER_HEIGHT_CHANGE_STEP);
    while (/*has_steep_change &&*/ count < 6) {
       ret = gauss_blur(ret, smoothing_params);
       //has_steep_change = has_steep_height_change(ret, LAYER_HEIGHT_CHANGE_STEP);
       count++;
    }
    return ret;
    // return gauss_blur(profile, smoothing_params);
}

void adjust_layer_height_profile(
    const ModelObject           &model_object,
    const SlicingParameters     &slicing_params,
    std::vector<coordf_t> 		&layer_height_profile,
    coordf_t 					 z,
    coordf_t 					 layer_thickness_delta,
    coordf_t 					 band_width,
    LayerHeightEditActionType    action)
{
     // Constrain the profile variability by the 1st layer height.
    std::pair<coordf_t, coordf_t> z_span_variable = 
        std::pair<coordf_t, coordf_t>(
            slicing_params.first_object_layer_height_fixed() ? slicing_params.first_object_layer_height : 0.,
            slicing_params.object_print_z_uncompensated_height());
    if (z < z_span_variable.first || z > z_span_variable.second)
        return;

	assert(layer_height_profile.size() >= 2);
    assert(std::abs(layer_height_profile[layer_height_profile.size() - 2] - slicing_params.object_print_z_uncompensated_height()) < EPSILON);

    // 1) Get the current layer thickness at z.
    coordf_t current_layer_height = slicing_params.layer_height;
    for (size_t i = 0; i < layer_height_profile.size(); i += 2) {
        if (i + 2 == layer_height_profile.size()) {
            current_layer_height = layer_height_profile[i + 1];
            break;
        } else if (layer_height_profile[i + 2] > z) {
            coordf_t z1 = layer_height_profile[i];
            coordf_t h1 = layer_height_profile[i + 1];
            coordf_t z2 = layer_height_profile[i + 2];
            coordf_t h2 = layer_height_profile[i + 3];
            current_layer_height = lerp(h1, h2, (z - z1) / (z2 - z1));
			break;
        }
    }

    for (auto const& [range,options] : model_object.layer_config_ranges) {
        if (z >= range.first - current_layer_height && z <= range.second + current_layer_height)
            return;
    };

    // 2) Is it possible to apply the delta?
    switch (action) {
        case LAYER_HEIGHT_EDIT_ACTION_DECREASE:
            layer_thickness_delta = - layer_thickness_delta;
            // fallthrough
        case LAYER_HEIGHT_EDIT_ACTION_INCREASE:
            if (layer_thickness_delta > 0) {
                if (current_layer_height >= slicing_params.max_layer_height - EPSILON)
                    return;
                layer_thickness_delta = std::min(layer_thickness_delta, slicing_params.max_layer_height - current_layer_height);
            } else {
                if (current_layer_height <= slicing_params.min_layer_height + EPSILON)
                    return;
                layer_thickness_delta = std::max(layer_thickness_delta, slicing_params.min_layer_height - current_layer_height);
            }
            break;
        case LAYER_HEIGHT_EDIT_ACTION_REDUCE:
        case LAYER_HEIGHT_EDIT_ACTION_SMOOTH:
            layer_thickness_delta = std::abs(layer_thickness_delta);
            layer_thickness_delta = std::min(layer_thickness_delta, std::abs(slicing_params.layer_height - current_layer_height));
            if (layer_thickness_delta < EPSILON)
                return;
            break;
        default:
            assert(false);
            break;
    }

    // 3) Densify the profile inside z +- band_width/2, remove duplicate Zs from the height profile inside the band.
	coordf_t lo = std::max(z_span_variable.first,  z - 0.5 * band_width);
    // Do not limit the upper side of the band, so that the modifications to the top point of the profile will be allowed.
    coordf_t hi = z + 0.5 * band_width;
    coordf_t z_step = 0.1;
    size_t idx = 0;
    while (idx < layer_height_profile.size() && layer_height_profile[idx] < lo)
        idx += 2;
    idx -= 2;

    std::vector<double> profile_new;
    profile_new.reserve(layer_height_profile.size());
	assert(idx >= 0 && idx + 1 < layer_height_profile.size());
	profile_new.insert(profile_new.end(), layer_height_profile.begin(), layer_height_profile.begin() + idx + 2);
    coordf_t zz = lo;
    size_t i_resampled_start = profile_new.size();
    while (zz < hi) {
        size_t next = idx + 2;
        coordf_t z1 = layer_height_profile[idx];
        coordf_t h1 = layer_height_profile[idx + 1];
        coordf_t height = h1;
        if (next < layer_height_profile.size()) {
            coordf_t z2 = layer_height_profile[next];
            coordf_t h2 = layer_height_profile[next + 1];
            height = lerp(h1, h2, (zz - z1) / (z2 - z1));
        }
        // Adjust height by layer_thickness_delta.
        coordf_t weight = std::abs(zz - z) < 0.5 * band_width ? (0.5 + 0.5 * cos(2. * M_PI * (zz - z) / band_width)) : 0.;
        switch (action) {
            case LAYER_HEIGHT_EDIT_ACTION_INCREASE:
            case LAYER_HEIGHT_EDIT_ACTION_DECREASE:
                height += weight * layer_thickness_delta;
                break;
            case LAYER_HEIGHT_EDIT_ACTION_REDUCE:
            {
                coordf_t delta = height - slicing_params.layer_height;
                coordf_t step  = weight * layer_thickness_delta;
                step = (std::abs(delta) > step) ?
                    (delta > 0) ? -step : step :
                    -delta;
                height += step;
                break;
            }
            case LAYER_HEIGHT_EDIT_ACTION_SMOOTH:
            {
                // Don't modify the profile during resampling process, do it at the next step.
                break;
            }
            default:
                assert(false);
                break;
        }
        height = std::clamp(height, slicing_params.min_layer_height, slicing_params.max_layer_height);
        if (zz == z_span_variable.second) {
            // This is the last point of the profile.
            if (profile_new[profile_new.size() - 2] + EPSILON > zz) {
                profile_new.pop_back();
                profile_new.pop_back();
            }
            profile_new.push_back(zz);
            profile_new.push_back(height);
			idx = layer_height_profile.size();
            break;
        }
        // Avoid entering a too short segment.
        if (profile_new[profile_new.size() - 2] + EPSILON < zz) {
            profile_new.push_back(zz);
            profile_new.push_back(height);
        }
        // Limit zz to the object height, so the next iteration the last profile point will be set.
		zz = std::min(zz + z_step, z_span_variable.second);
        idx = next;
        while (idx < layer_height_profile.size() && layer_height_profile[idx] < zz)
            idx += 2;
        idx -= 2;
    }

    idx += 2;
    assert(idx > 0);
    size_t i_resampled_end = profile_new.size();
	if (idx < layer_height_profile.size()) {
        assert(zz >= layer_height_profile[idx - 2]);
        assert(zz <= layer_height_profile[idx]);
		profile_new.insert(profile_new.end(), layer_height_profile.begin() + idx, layer_height_profile.end());
	}
	else if (profile_new[profile_new.size() - 2] + 0.5 * EPSILON < z_span_variable.second) { 
		profile_new.insert(profile_new.end(), layer_height_profile.end() - 2, layer_height_profile.end());
	}
    layer_height_profile = std::move(profile_new);

    if (action == LAYER_HEIGHT_EDIT_ACTION_SMOOTH) {
        if (i_resampled_start == 0)
            ++ i_resampled_start;
		if (i_resampled_end == layer_height_profile.size())
			i_resampled_end -= 2;
        size_t n_rounds = 6;
        for (size_t i_round = 0; i_round < n_rounds; ++ i_round) {
            profile_new = layer_height_profile;
            for (size_t i = i_resampled_start; i < i_resampled_end; i += 2) {
                coordf_t zz = profile_new[i];
                coordf_t t = std::abs(zz - z) < 0.5 * band_width ? (0.25 + 0.25 * cos(2. * M_PI * (zz - z) / band_width)) : 0.;
                assert(t >= 0. && t <= 0.5000001);
                if (i == 0)
                    layer_height_profile[i + 1] = (1. - t) * profile_new[i + 1] + t * profile_new[i + 3];
                else if (i + 1 == profile_new.size())
                    layer_height_profile[i + 1] = (1. - t) * profile_new[i + 1] + t * profile_new[i - 1];
                else
                    layer_height_profile[i + 1] = (1. - t) * profile_new[i + 1] + 0.5 * t * (profile_new[i - 1] + profile_new[i + 3]);
            }
        }
    }

	assert(layer_height_profile.size() > 2);
	assert(layer_height_profile.size() % 2 == 0);
	assert(layer_height_profile[0] == 0.);
    assert(std::abs(layer_height_profile[layer_height_profile.size() - 2] - slicing_params.object_print_z_uncompensated_height()) < EPSILON);
#ifdef _DEBUG
	for (size_t i = 2; i < layer_height_profile.size(); i += 2)
		assert(layer_height_profile[i - 2] <= layer_height_profile[i]);
	for (size_t i = 1; i < layer_height_profile.size(); i += 2) {
		assert(layer_height_profile[i] > slicing_params.min_layer_height - EPSILON);
		assert(layer_height_profile[i] < slicing_params.max_layer_height + EPSILON);
	}
#endif /* _DEBUG */
}

bool adjust_layer_series_to_align_object_height(const SlicingParameters &slicing_params, std::vector<coordf_t>& layer_series)
{
    coordf_t object_height = slicing_params.object_print_z_height();
    if (is_approx(layer_series.back(), object_height))
        return true;

    // need at least 5 + 1(first_layer) layers to adjust the height
    size_t layer_size = layer_series.size();
    if (layer_size < 12)
        return false;

    std::vector<coordf_t> last_5_layers_heght;
    for (size_t i = 0; i < 5; ++i) {
        last_5_layers_heght.emplace_back(layer_series[layer_size - 10 + 2 * i + 1] - layer_series[layer_size - 10 + 2 * i]);
    }

    coordf_t gap = abs(layer_series.back() - object_height);
    std::vector<bool> can_adjust(5, true); // to record whether every layer can adjust layer height
    bool taller_than_object = layer_series.back() < object_height;

    auto get_valid_size = [&can_adjust]() -> int {
        int valid_size = 0;
        for (auto b_adjust : can_adjust) {
            valid_size += b_adjust ? 1 : 0;
        }
        return valid_size;
    };

    auto adjust_layer_height = [&slicing_params, &last_5_layers_heght, &can_adjust, &get_valid_size, &taller_than_object](coordf_t gap) -> coordf_t {
        coordf_t delta_gap = gap / get_valid_size();
        coordf_t remain_gap = 0;
        for (size_t i = 0; i < last_5_layers_heght.size(); ++i) {
            coordf_t& l_height = last_5_layers_heght[i];
            if (taller_than_object) {
                if (can_adjust[i] && is_approx(l_height, slicing_params.max_layer_height)) {
                    remain_gap += delta_gap;
                    can_adjust[i] = false;
                    continue;
                }

                if (can_adjust[i] && l_height + delta_gap > slicing_params.max_layer_height) {
                    remain_gap += l_height + delta_gap - slicing_params.max_layer_height;
                    l_height      = slicing_params.max_layer_height;
                    can_adjust[i] = false;
                }
                else {
                    l_height += delta_gap;
                }
            }
            else {
                if (can_adjust[i] && is_approx(l_height, slicing_params.min_layer_height)) {
                    remain_gap += delta_gap;
                    can_adjust[i] = false;
                    continue;
                }

                if (can_adjust[i] && l_height - delta_gap < slicing_params.min_layer_height) {
                    remain_gap += slicing_params.min_layer_height + delta_gap - l_height;
                    l_height      = slicing_params.min_layer_height;
                    can_adjust[i] = false;
                }
                else {
                    l_height -= delta_gap;
                }
            }
        }
        return remain_gap;
    };

    while (gap > 0) {
        int valid_size = get_valid_size();
        if (valid_size == 0) {
            // 5 layers can not adjust z within valid layer height
            return false;
        }

        gap = adjust_layer_height(gap);
        if (is_approx(gap, 0.0)) {
            // adjust succeed
            break;
        }
    }

    for (size_t i = 0; i < last_5_layers_heght.size(); ++i) {
        if (i > 0) {
            layer_series[layer_size - 10 + 2 * i] = layer_series[layer_size - 10 + 2 * i - 1];
        }
        layer_series[layer_size - 10 + 2 * i + 1] = layer_series[layer_size - 10 + 2 * i] + last_5_layers_heght[i];
    }

    return true;
}

// Produce object layers as pairs of low / high layer boundaries, stored into a linear vector.
std::vector<coordf_t> generate_object_layers(
	const SlicingParameters 	&slicing_params,
	const std::vector<coordf_t> &layer_height_profile,
    bool is_precise_z_height)
{
    assert(! layer_height_profile.empty());

    coordf_t print_z = 0;
    coordf_t height  = 0;

    std::vector<coordf_t> out;

    if (slicing_params.first_object_layer_height_fixed()) {
        out.push_back(0);
        print_z = slicing_params.first_object_layer_height;
        out.push_back(print_z);
    }

    // Orca: XYZ shrinkage compensation
    const coordf_t shrinkage_compensation_z = slicing_params.object_shrinkage_compensation_z;
    size_t idx_layer_height_profile = 0;
    // loop until we have at least one layer and the max slice_z reaches the object height
    coordf_t slice_z = print_z + 0.5 * slicing_params.min_layer_height;
    while (slice_z < slicing_params.object_print_z_height()) {
        height = slicing_params.min_layer_height;
        if (idx_layer_height_profile < layer_height_profile.size()) {
            size_t next = idx_layer_height_profile + 2;
            for (;;) {
                // Orca: XYZ shrinkage compensation
                if (next >= layer_height_profile.size() || slice_z < layer_height_profile[next] * shrinkage_compensation_z)
                    break;
                idx_layer_height_profile = next;
                next += 2;
            }
            // Orca: XYZ shrinkage compensation
            const coordf_t z1 = layer_height_profile[idx_layer_height_profile] * shrinkage_compensation_z;
            const coordf_t h1 = layer_height_profile[idx_layer_height_profile + 1];
            height = h1;
            if (next < layer_height_profile.size()) {
                // Orca: XYZ shrinkage compensation
                const coordf_t z2 = layer_height_profile[next] * shrinkage_compensation_z;
                const coordf_t h2 = layer_height_profile[next + 1];
                height = lerp(h1, h2, (slice_z - z1) / (z2 - z1));
                assert(height >= slicing_params.min_layer_height - EPSILON && height <= slicing_params.max_layer_height + EPSILON);
            }
        }
        slice_z = print_z + 0.5 * height;
        if (slice_z >= slicing_params.object_print_z_height())
            break;
        assert(height > slicing_params.min_layer_height - EPSILON);
        assert(height < slicing_params.max_layer_height + EPSILON);
        out.push_back(print_z);
        print_z += height;
        slice_z = print_z + 0.5 * slicing_params.min_layer_height;
        out.push_back(print_z);
    }

    if (is_precise_z_height)
        adjust_layer_series_to_align_object_height(slicing_params, out);
    return out;
}

// Check whether the layer height profile describes a fixed layer height profile.
bool check_object_layers_fixed(
    const SlicingParameters     &slicing_params,
    const std::vector<coordf_t> &layer_height_profile)
{
    assert(layer_height_profile.size() >= 4);
    assert(layer_height_profile.size() % 2 == 0);
    assert(layer_height_profile[0] == 0);

    if (layer_height_profile.size() != 4 && layer_height_profile.size() != 8)
        return false;

    bool fixed_step1 = is_approx(layer_height_profile[1], layer_height_profile[3]);
    bool fixed_step2 = layer_height_profile.size() == 4 || 
            (layer_height_profile[2] == layer_height_profile[4] && is_approx(layer_height_profile[5], layer_height_profile[7]));

    if (! fixed_step1 || ! fixed_step2)
        return false;

    if (layer_height_profile[2] < 0.5 * slicing_params.first_object_layer_height + EPSILON ||
        ! is_approx(layer_height_profile[3], slicing_params.first_object_layer_height))
        return false;

    double z_max = layer_height_profile[layer_height_profile.size() - 2];
    double z_2nd = slicing_params.first_object_layer_height + 0.5 * slicing_params.layer_height;
    if (z_2nd > z_max)
        return true;
    if (z_2nd < *(layer_height_profile.end() - 4) + EPSILON ||
        ! is_approx(layer_height_profile.back(), slicing_params.layer_height))
        return false;

    return true;
}

int generate_layer_height_texture(
	const SlicingParameters 	&slicing_params,
	const std::vector<coordf_t> &layers,
	void *data, int rows, int cols, bool level_of_detail_2nd_level)
{
// https://github.com/aschn/gnuplot-colorbrewer
    std::vector<Vec3crd> palette_raw;
    palette_raw.push_back(Vec3crd(0x01A, 0x098, 0x050));
    palette_raw.push_back(Vec3crd(0x066, 0x0BD, 0x063));
    palette_raw.push_back(Vec3crd(0x0A6, 0x0D9, 0x06A));
    palette_raw.push_back(Vec3crd(0x0D9, 0x0F1, 0x0EB));
    palette_raw.push_back(Vec3crd(0x0FE, 0x0E6, 0x0EB));
    palette_raw.push_back(Vec3crd(0x0FD, 0x0AE, 0x061));
    palette_raw.push_back(Vec3crd(0x0F4, 0x06D, 0x043));
    palette_raw.push_back(Vec3crd(0x0D7, 0x030, 0x027));

    // Clear the main texture and the 2nd LOD level.
//	memset(data, 0, rows * cols * (level_of_detail_2nd_level ? 5 : 4));
    // 2nd LOD level data start
    unsigned char *data1 = reinterpret_cast<unsigned char*>(data) + rows * cols * 4;
    int ncells  = std::min((cols-1) * rows, int(ceil(16. * (slicing_params.object_print_z_height() / slicing_params.min_layer_height))));
    int ncells1 = ncells / 2;
    int cols1   = cols / 2;
    coordf_t z_to_cell = coordf_t(ncells-1) / slicing_params.object_print_z_height();
    coordf_t cell_to_z = slicing_params.object_print_z_height() / coordf_t(ncells-1);
    coordf_t z_to_cell1 = coordf_t(ncells1-1) / slicing_params.object_print_z_height();
    // for color scaling
	coordf_t hscale = 2.f * std::max(slicing_params.max_layer_height - slicing_params.layer_height, slicing_params.layer_height - slicing_params.min_layer_height);
	if (hscale == 0)
		// All layers have the same height. Provide some height scale to avoid division by zero.
		hscale = slicing_params.layer_height;
    for (size_t idx_layer = 0; idx_layer < layers.size(); idx_layer += 2) {
        coordf_t lo  = layers[idx_layer];
		coordf_t hi  = layers[idx_layer + 1];
        coordf_t mid = 0.5f * (lo + hi);
		assert(mid <= slicing_params.object_print_z_height());
		coordf_t h = hi - lo;
		hi = std::min(hi, slicing_params.object_print_z_height());
        int cell_first = std::clamp(int(ceil(lo * z_to_cell)), 0, ncells-1);
        int cell_last  = std::clamp(int(floor(hi * z_to_cell)), 0, ncells-1);
        for (int cell = cell_first; cell <= cell_last; ++ cell) {
            coordf_t idxf = (0.5 * hscale + (h - slicing_params.layer_height)) * coordf_t(palette_raw.size()-1) / hscale;
            int idx1 = std::clamp(int(floor(idxf)), 0, int(palette_raw.size() - 1));
            int idx2 = std::min(int(palette_raw.size() - 1), idx1 + 1);
			coordf_t t = idxf - coordf_t(idx1);
            const Vec3crd &color1 = palette_raw[idx1];
            const Vec3crd &color2 = palette_raw[idx2];
            coordf_t z = cell_to_z * coordf_t(cell);
            assert(lo - EPSILON <= z && z <= hi + EPSILON);
            // Intensity profile to visualize the layers.
            coordf_t intensity = cos(M_PI * 0.7 * (mid - z) / h);
            // Color mapping from layer height to RGB.
            Vec3d color(
                intensity * lerp(coordf_t(color1(0)), coordf_t(color2(0)), t), 
                intensity * lerp(coordf_t(color1(1)), coordf_t(color2(1)), t),
                intensity * lerp(coordf_t(color1(2)), coordf_t(color2(2)), t));
            int row = cell / (cols - 1);
            int col = cell - row * (cols - 1);
			assert(row >= 0 && row < rows);
			assert(col >= 0 && col < cols);
            unsigned char *ptr = (unsigned char*)data + (row * cols + col) * 4;
            ptr[0] = (unsigned char)std::clamp(int(floor(color(0) + 0.5)), 0, 255);
            ptr[1] = (unsigned char)std::clamp(int(floor(color(1) + 0.5)), 0, 255);
            ptr[2] = (unsigned char)std::clamp(int(floor(color(2) + 0.5)), 0, 255);
            ptr[3] = 255;
            if (col == 0 && row > 0) {
                // Duplicate the first value in a row as a last value of the preceding row.
                ptr[-4] = ptr[0];
                ptr[-3] = ptr[1];
                ptr[-2] = ptr[2];
                ptr[-1] = ptr[3];
            }
        }
        if (level_of_detail_2nd_level) {
            cell_first = std::clamp(int(ceil(lo * z_to_cell1)), 0, ncells1-1);
            cell_last  = std::clamp(int(floor(hi * z_to_cell1)), 0, ncells1-1);
            for (int cell = cell_first; cell <= cell_last; ++ cell) {
                coordf_t idxf = (0.5 * hscale + (h - slicing_params.layer_height)) * coordf_t(palette_raw.size()-1) / hscale;
                int idx1 = std::clamp(int(floor(idxf)), 0, int(palette_raw.size() - 1));
                int idx2 = std::min(int(palette_raw.size() - 1), idx1 + 1);
    			coordf_t t = idxf - coordf_t(idx1);
                const Vec3crd &color1 = palette_raw[idx1];
                const Vec3crd &color2 = palette_raw[idx2];
                // Color mapping from layer height to RGB.
                Vec3d color(
                    lerp(coordf_t(color1(0)), coordf_t(color2(0)), t), 
                    lerp(coordf_t(color1(1)), coordf_t(color2(1)), t),
                    lerp(coordf_t(color1(2)), coordf_t(color2(2)), t));
                int row = cell / (cols1 - 1);
                int col = cell - row * (cols1 - 1);
    			assert(row >= 0 && row < rows/2);
    			assert(col >= 0 && col < cols/2);
                unsigned char *ptr = data1 + (row * cols1 + col) * 4;
                ptr[0] = (unsigned char)std::clamp(int(floor(color(0) + 0.5)), 0, 255);
                ptr[1] = (unsigned char)std::clamp(int(floor(color(1) + 0.5)), 0, 255);
                ptr[2] = (unsigned char)std::clamp(int(floor(color(2) + 0.5)), 0, 255);
                ptr[3] = 255;
                if (col == 0 && row > 0) {
                    // Duplicate the first value in a row as a last value of the preceding row.
                    ptr[-4] = ptr[0];
                    ptr[-3] = ptr[1];
                    ptr[-2] = ptr[2];
                    ptr[-1] = ptr[3];
                }
            }
        }
    }

    // Returns number of cells of the 0th LOD level.
    return ncells;
}

}; // namespace Slic3r
