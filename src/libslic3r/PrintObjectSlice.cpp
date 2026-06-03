#include <algorithm>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <sstream>

#include <boost/log/trivial.hpp>

#include <tbb/parallel_for.h>

#include "ClipperUtils.hpp"
#include "ElephantFootCompensation.hpp"
#include "Exception.hpp"
#include "I18N.hpp"
#include "Layer.hpp"
#include "MultiMaterialSegmentation.hpp"
#include "Print.hpp"
#include "SVG.hpp"
//BBS
#include "ShortestPath.hpp"
#include "libslic3r/Feature/Interlocking/InterlockingGenerator.hpp"

//! macro used to mark string used at localization, return same string
#define L(s) Slic3r::I18N::translate(s)

namespace Slic3r {

bool PrintObject::clip_multipart_objects = true;
bool PrintObject::infill_only_where_needed = false;

static coordf_t compute_slice_z(PrintObject* print_object, size_t i_layer, coordf_t lo, coordf_t hi)
{
    bool zaa_active   = false;
    coordf_t z_offset = 0.0;

    size_t num_regions = print_object->num_printing_regions();
    for (size_t rid = 0; rid < num_regions; ++rid) {
        const auto& rcfg = print_object->printing_region(rid).config();
        if (rcfg.zaa_enabled) {
            if (!zaa_active || rcfg.zaa_min_z < z_offset)
                z_offset = rcfg.zaa_min_z;
            zaa_active = true;
        }
    }

    if (!zaa_active || i_layer == 0) {
        return 0.5 * (lo + hi);
    }

    coordf_t slice_z = lo + z_offset;
    if ((slice_z < lo && !is_approx(slice_z, lo)) || (slice_z > hi && !is_approx(slice_z, hi))) {
        throw RuntimeError("Bad min Z value");
    }
    return slice_z;
}

// Forward declarations for the surface_emboss_mixed debug subsystem (defined further below).
static bool        has_surface_emboss_mixed_volume(const PrintObject &print_object);
static std::string surface_emboss_mixed_debug_file_path(const PrintObject &print_object);
static void        reset_surface_emboss_mixed_debug_file(const PrintObject &print_object);
static void        dump_surface_emboss_mixed_layer_state(
    const char                                  *stage,
    const PrintObject                           &print_object,
    size_t                                       layer_id,
    const Layer                                 &layer,
    const PrintObjectRegions::LayerRangeRegions &layer_range,
    const std::vector<ExPolygons>               *segmentation_layer = nullptr);

LayerPtrs new_layers(
    PrintObject                 *print_object,
    // Object layers (pairs of bottom/top Z coordinate), without the raft.
    const std::vector<coordf_t> &object_layers)
{
    LayerPtrs out;
    out.reserve(object_layers.size());
    auto     id   = int(print_object->slicing_parameters().raft_layers());
    coordf_t zmin = print_object->slicing_parameters().object_print_z_min;
    Layer   *prev = nullptr;
    for (size_t i_layer = 0; i_layer < object_layers.size(); i_layer += 2) {
        coordf_t lo = object_layers[i_layer];
        coordf_t hi = object_layers[i_layer + 1];
        coordf_t slice_z = compute_slice_z(print_object, i_layer, lo, hi);

        Layer *layer = new Layer(id ++, print_object, hi - lo, hi + zmin, slice_z);
        out.emplace_back(layer);
        if (prev != nullptr) {
            prev->upper_layer = layer;
            layer->lower_layer = prev;
        }
        prev = layer;
    }
    return out;
}

// Slice single triangle mesh.
static std::vector<ExPolygons> slice_volume(
    const ModelVolume             &volume,
    const std::vector<float>      &zs,
    const MeshSlicingParamsEx     &params,
    const std::function<void()>   &throw_on_cancel_callback)
{
    std::vector<ExPolygons> layers;
    if (! zs.empty()) {
        indexed_triangle_set its = volume.mesh().its;
        if (its.indices.size() > 0) {
            MeshSlicingParamsEx params2 { params };
            params2.trafo = params2.trafo * volume.get_matrix();
            if (params2.trafo.rotation().determinant() < 0.)
                its_flip_triangles(its);
            layers = slice_mesh_ex(its, zs, params2, throw_on_cancel_callback);
            throw_on_cancel_callback();
        }
    }
    return layers;
}

// Slice single triangle mesh.
// Filter the zs not inside the ranges. The ranges are closed at the bottom and open at the top, they are sorted lexicographically and non overlapping.
static std::vector<ExPolygons> slice_volume(
    const ModelVolume                           &volume,
    const std::vector<float>                    &z,
    const std::vector<t_layer_height_range>     &ranges,
    const MeshSlicingParamsEx                   &params,
    const std::function<void()>                 &throw_on_cancel_callback)
{
    std::vector<ExPolygons> out;
    if (! z.empty() && ! ranges.empty()) {
        if (ranges.size() == 1 && z.front() >= ranges.front().first && z.back() < ranges.front().second) {
            // All layers fit into a single range.
            out = slice_volume(volume, z, params, throw_on_cancel_callback);
        } else {
            std::vector<float>                     z_filtered;
            std::vector<std::pair<size_t, size_t>> n_filtered;
            z_filtered.reserve(z.size());
            n_filtered.reserve(2 * ranges.size());
            size_t i = 0;
            for (const t_layer_height_range &range : ranges) {
                for (; i < z.size() && z[i] < range.first; ++ i) ;
                size_t first = i;
                for (; i < z.size() && z[i] < range.second; ++ i)
                    z_filtered.emplace_back(z[i]);
                if (i > first)
                    n_filtered.emplace_back(std::make_pair(first, i));
            }
            if (! n_filtered.empty()) {
                std::vector<ExPolygons> layers = slice_volume(volume, z_filtered, params, throw_on_cancel_callback);
                out.assign(z.size(), ExPolygons());
                i = 0;
                for (const std::pair<size_t, size_t> &span : n_filtered)
                    for (size_t j = span.first; j < span.second; ++ j)
                        out[j] = std::move(layers[i ++]);
            }
        }
    }
    return out;
}
static inline bool model_volume_needs_slicing(const ModelVolume &mv)
{
    ModelVolumeType type = mv.type();
    return type == ModelVolumeType::MODEL_PART || type == ModelVolumeType::NEGATIVE_VOLUME || type == ModelVolumeType::PARAMETER_MODIFIER;
}

// Slice printable volumes, negative volumes and modifier volumes, sorted by ModelVolume::id().
// Apply closing radius.
// Apply positive XY compensation to ModelVolumeType::MODEL_PART and ModelVolumeType::PARAMETER_MODIFIER, not to ModelVolumeType::NEGATIVE_VOLUME.
// Apply contour simplification.
static std::vector<VolumeSlices> slice_volumes_inner(
    const PrintConfig                                        &print_config,
    const PrintObjectConfig                                  &print_object_config,
    const Transform3d                                        &object_trafo,
    ModelVolumePtrs                                           model_volumes,
    const std::vector<PrintObjectRegions::LayerRangeRegions> &layer_ranges,
    const std::vector<float>                                 &zs,
    const std::function<void()>                              &throw_on_cancel_callback)
{
    model_volumes_sort_by_id(model_volumes);

    std::vector<VolumeSlices> out;
    out.reserve(model_volumes.size());

    std::vector<t_layer_height_range> slicing_ranges;
    if (layer_ranges.size() > 1)
        slicing_ranges.reserve(layer_ranges.size());

    MeshSlicingParamsEx params_base;
    params_base.closing_radius = print_object_config.slice_closing_radius.value;
    params_base.extra_offset   = 0;
    params_base.trafo          = object_trafo;
    //BBS: 0.0025mm is safe enough to simplify the data to speed slicing up for high-resolution model.
    //Also has on influence on arc fitting which has default resolution 0.0125mm.
    params_base.resolution = print_config.resolution <= 0.001 ? 0.0f : 0.0025;
    switch (print_object_config.slicing_mode.value) {
    case SlicingMode::Regular:    params_base.mode = MeshSlicingParams::SlicingMode::Regular; break;
    case SlicingMode::EvenOdd:    params_base.mode = MeshSlicingParams::SlicingMode::EvenOdd; break;
    case SlicingMode::CloseHoles: params_base.mode = MeshSlicingParams::SlicingMode::Positive; break;
    }

    params_base.mode_below     = params_base.mode;

    // BBS
    const size_t num_extruders = print_config.filament_diameter.size();
    const bool   is_mm_painted = num_extruders > 1 && std::any_of(model_volumes.cbegin(), model_volumes.cend(), [](const ModelVolume *mv) { return mv->is_mm_painted(); });
    // BBS: don't do size compensation when slice volume.
    // Will handle contour and hole size compensation seperately later.
    //const auto   extra_offset  = is_mm_painted ? 0.f : std::max(0.f, float(print_object_config.xy_contour_compensation.value));
    const auto   extra_offset = 0.f;

    for (const ModelVolume *model_volume : model_volumes)
        if (model_volume_needs_slicing(*model_volume)) {
            MeshSlicingParamsEx params { params_base };
            if (! model_volume->is_negative_volume())
                params.extra_offset = extra_offset;
            if (layer_ranges.size() == 1) {
                if (const PrintObjectRegions::LayerRangeRegions &layer_range = layer_ranges.front(); layer_range.has_volume(model_volume->id())) {
                    if (model_volume->is_model_part() && print_config.spiral_mode) {
                        auto it = std::find_if(layer_range.volume_regions.begin(), layer_range.volume_regions.end(),
                            [model_volume](const auto &slice){ return model_volume == slice.model_volume; });
                        params.mode = MeshSlicingParams::SlicingMode::PositiveLargestContour;
                        // Slice the bottom layers with SlicingMode::Regular.
                        // This needs to be in sync with LayerRegion::make_perimeters() spiral_mode!
                        const PrintRegionConfig &region_config = it->region->config();
                        params.slicing_mode_normal_below_layer = size_t(region_config.bottom_shell_layers.value);
                        for (; params.slicing_mode_normal_below_layer < zs.size() && zs[params.slicing_mode_normal_below_layer] < region_config.bottom_shell_thickness - EPSILON;
                            ++ params.slicing_mode_normal_below_layer);
                    }
                    out.push_back({
                        model_volume->id(),
                        slice_volume(*model_volume, zs, params, throw_on_cancel_callback)
                    });
                }
            } else {
                assert(! print_config.spiral_mode);
                slicing_ranges.clear();
                for (const PrintObjectRegions::LayerRangeRegions &layer_range : layer_ranges)
                    if (layer_range.has_volume(model_volume->id()))
                        slicing_ranges.emplace_back(layer_range.layer_height_range);
                if (! slicing_ranges.empty())
                    out.push_back({
                        model_volume->id(),
                        slice_volume(*model_volume, zs, slicing_ranges, params, throw_on_cancel_callback)
                    });
            }
            if (! out.empty() && out.back().slices.empty())
                out.pop_back();
        }

    return out;
}

static inline VolumeSlices& volume_slices_find_by_id(std::vector<VolumeSlices> &volume_slices, const ObjectID id)
{
    auto it = lower_bound_by_predicate(volume_slices.begin(), volume_slices.end(), [id](const VolumeSlices &vs) { return vs.volume_id < id; });
    assert(it != volume_slices.end() && it->volume_id == id);
    return *it;
}

static inline bool overlap_in_xy(const PrintObjectRegions::BoundingBox &l, const PrintObjectRegions::BoundingBox &r)
{
    return ! (l.max().x() < r.min().x() || l.min().x() > r.max().x() ||
              l.max().y() < r.min().y() || l.min().y() > r.max().y());
}

static std::vector<PrintObjectRegions::LayerRangeRegions>::const_iterator layer_range_first(const std::vector<PrintObjectRegions::LayerRangeRegions> &layer_ranges, double z)
{
    auto  it = lower_bound_by_predicate(layer_ranges.begin(), layer_ranges.end(),
        [z](const PrintObjectRegions::LayerRangeRegions &lr) {
            return lr.layer_height_range.second < z && abs(lr.layer_height_range.second - z) > EPSILON;
        });
    assert(it != layer_ranges.end() && it->layer_height_range.first <= z && z <= it->layer_height_range.second);
    if (z == it->layer_height_range.second)
        if (auto it_next = it; ++ it_next != layer_ranges.end() && it_next->layer_height_range.first == z)
            it = it_next;
    assert(it != layer_ranges.end() && it->layer_height_range.first <= z && z <= it->layer_height_range.second);
    return it;
}

static std::vector<PrintObjectRegions::LayerRangeRegions>::const_iterator layer_range_next(
    const std::vector<PrintObjectRegions::LayerRangeRegions>            &layer_ranges,
    std::vector<PrintObjectRegions::LayerRangeRegions>::const_iterator   it,
    double                                                               z)
{
    for (; it->layer_height_range.second <= z + EPSILON; ++ it)
        assert(it != layer_ranges.end());
    assert(it != layer_ranges.end() && it->layer_height_range.first <= z && z < it->layer_height_range.second);
    return it;
}

static std::vector<std::vector<ExPolygons>> slices_to_regions(
    const PrintConfig                                        &print_config,
    const PrintObject                                        &print_object,
    ModelVolumePtrs                                           model_volumes,
    const PrintObjectRegions                                 &print_object_regions,
    const std::vector<float>                                 &zs,
    std::vector<VolumeSlices>                               &&volume_slices,
    // If clipping is disabled, then ExPolygons produced by different volumes will never be merged, thus they will be allowed to overlap.
    // It is up to the model designer to handle these overlaps.
    const bool                                                clip_multipart_objects,
    const std::function<void()>                              &throw_on_cancel_callback)
{
    model_volumes_sort_by_id(model_volumes);

    std::vector<std::vector<ExPolygons>> slices_by_region(print_object_regions.all_regions.size(), std::vector<ExPolygons>(zs.size(), ExPolygons()));

    // First shuffle slices into regions if there is no overlap with another region possible, collect zs of the complex cases.
    std::vector<std::pair<size_t, float>> zs_complex;
    {
        size_t z_idx = 0;
        for (const PrintObjectRegions::LayerRangeRegions &layer_range : print_object_regions.layer_ranges) {
            for (; z_idx < zs.size() && zs[z_idx] < layer_range.layer_height_range.first; ++ z_idx) ;
            if (layer_range.volume_regions.empty()) {
            } else if (layer_range.volume_regions.size() == 1) {
                const ModelVolume *model_volume = layer_range.volume_regions.front().model_volume;
                assert(model_volume != nullptr);
                if (model_volume->is_model_part()) {
                    VolumeSlices &slices_src = volume_slices_find_by_id(volume_slices, model_volume->id());
                    auto         &slices_dst = slices_by_region[layer_range.volume_regions.front().region->print_object_region_id()];
                    for (; z_idx < zs.size() && zs[z_idx] < layer_range.layer_height_range.second; ++ z_idx)
                        slices_dst[z_idx] = std::move(slices_src.slices[z_idx]);
                }
            } else {
                zs_complex.reserve(zs.size());
                for (; z_idx < zs.size() && zs[z_idx] < layer_range.layer_height_range.second; ++ z_idx) {
                    float z                          = zs[z_idx];
                    int   idx_first_printable_region = -1;
                    bool  complex                    = false;
                    for (int idx_region = 0; idx_region < int(layer_range.volume_regions.size()); ++ idx_region) {
                        const PrintObjectRegions::VolumeRegion &region = layer_range.volume_regions[idx_region];
                        if (region.bbox->min().z() <= z && region.bbox->max().z() >= z) {
                            if (idx_first_printable_region == -1 && region.model_volume->is_model_part())
                                idx_first_printable_region = idx_region;
                            else if (idx_first_printable_region != -1) {
                                // Test for overlap with some other region.
                                for (int idx_region2 = idx_first_printable_region; idx_region2 < idx_region; ++ idx_region2) {
                                    const PrintObjectRegions::VolumeRegion &region2 = layer_range.volume_regions[idx_region2];
                                    if (region2.bbox->min().z() <= z && region2.bbox->max().z() >= z && overlap_in_xy(*region.bbox, *region2.bbox)) {
                                        complex = true;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    if (complex)
                        zs_complex.push_back({ z_idx, z });
                    else if (idx_first_printable_region >= 0) {
                        const PrintObjectRegions::VolumeRegion &region = layer_range.volume_regions[idx_first_printable_region];
                        slices_by_region[region.region->print_object_region_id()][z_idx] = std::move(volume_slices_find_by_id(volume_slices, region.model_volume->id()).slices[z_idx]);
                    }
                }
            }
            throw_on_cancel_callback();
        }
    }

    // Second perform region clipping and assignment in parallel.
    if (! zs_complex.empty()) {
        std::vector<std::vector<VolumeSlices*>> layer_ranges_regions_to_slices(print_object_regions.layer_ranges.size(), std::vector<VolumeSlices*>());
        for (const PrintObjectRegions::LayerRangeRegions &layer_range : print_object_regions.layer_ranges) {
            std::vector<VolumeSlices*> &layer_range_regions_to_slices = layer_ranges_regions_to_slices[&layer_range - print_object_regions.layer_ranges.data()];
            layer_range_regions_to_slices.reserve(layer_range.volume_regions.size());
            for (const PrintObjectRegions::VolumeRegion &region : layer_range.volume_regions)
                layer_range_regions_to_slices.push_back(&volume_slices_find_by_id(volume_slices, region.model_volume->id()));
        }
        tbb::parallel_for(
            tbb::blocked_range<size_t>(0, zs_complex.size()),
            [&slices_by_region, &print_object_regions, &zs_complex, &layer_ranges_regions_to_slices, clip_multipart_objects, &throw_on_cancel_callback]
                (const tbb::blocked_range<size_t> &range) {
                float z              = zs_complex[range.begin()].second;
                auto  it_layer_range = layer_range_first(print_object_regions.layer_ranges, z);
                // Per volume_regions slices at this Z height.
                struct RegionSlice {
                    ExPolygons  expolygons;
                    // Identifier of this region in PrintObjectRegions::all_regions
                    int         region_id;
                    ObjectID    volume_id;
                    bool operator<(const RegionSlice &rhs) const {
                        bool this_empty = this->region_id < 0 || this->expolygons.empty();
                        bool rhs_empty  = rhs.region_id < 0 || rhs.expolygons.empty();
                        // Sort the empty items to the end of the list.
                        // Sort by region_id & volume_id lexicographically.
                        return ! this_empty && (rhs_empty || (this->region_id < rhs.region_id || (this->region_id == rhs.region_id && volume_id < rhs.volume_id)));
                    }
                };

                // BBS
                auto trim_overlap = [](ExPolygons& expolys_a, ExPolygons& expolys_b) {
                    ExPolygons trimming_a;
                    ExPolygons trimming_b;

                    for (ExPolygon& expoly_a : expolys_a) {
                        BoundingBox bbox_a = get_extents(expoly_a);
                        ExPolygons expolys_new;
                        for (ExPolygon& expoly_b : expolys_b) {
                            BoundingBox bbox_b = get_extents(expoly_b);
                            if (!bbox_a.overlap(bbox_b))
                                continue;

                            ExPolygons temp = intersection_ex(expoly_b, expoly_a, ApplySafetyOffset::Yes);
                            if (temp.empty())
                                continue;

                            if (expoly_a.contour.length() > expoly_b.contour.length())
                                trimming_a.insert(trimming_a.end(), temp.begin(), temp.end());
                            else
                                trimming_b.insert(trimming_b.end(), temp.begin(), temp.end());
                        }
                    }

                    expolys_a = diff_ex(expolys_a, trimming_a);
                    expolys_b = diff_ex(expolys_b, trimming_b);
                };

                std::vector<RegionSlice> temp_slices;
                for (size_t zs_complex_idx = range.begin(); zs_complex_idx < range.end(); ++ zs_complex_idx) {
                    auto [z_idx, z] = zs_complex[zs_complex_idx];
                    it_layer_range = layer_range_next(print_object_regions.layer_ranges, it_layer_range, z);
                    const PrintObjectRegions::LayerRangeRegions &layer_range = *it_layer_range;
                    {
                        std::vector<VolumeSlices*> &layer_range_regions_to_slices = layer_ranges_regions_to_slices[it_layer_range - print_object_regions.layer_ranges.begin()];
                        // Per volume_regions slices at thiz Z height.
                        temp_slices.clear();
                        temp_slices.reserve(layer_range.volume_regions.size());
                        for (VolumeSlices* &slices : layer_range_regions_to_slices) {
                            const PrintObjectRegions::VolumeRegion &volume_region = layer_range.volume_regions[&slices - layer_range_regions_to_slices.data()];
                            temp_slices.push_back({ std::move(slices->slices[z_idx]), volume_region.region ? volume_region.region->print_object_region_id() : -1, volume_region.model_volume->id() });
                        }
                    }
                    for (int idx_region = 0; idx_region < int(layer_range.volume_regions.size()); ++ idx_region)
                        if (! temp_slices[idx_region].expolygons.empty()) {
                            const PrintObjectRegions::VolumeRegion &region = layer_range.volume_regions[idx_region];
                            if (region.model_volume->is_modifier()) {
                                assert(region.parent > -1);
                                bool next_region_same_modifier = idx_region + 1 < int(temp_slices.size()) && layer_range.volume_regions[idx_region + 1].model_volume == region.model_volume;
                                RegionSlice &parent_slice = temp_slices[region.parent];
                                RegionSlice &this_slice   = temp_slices[idx_region];
                                ExPolygons   source       = std::move(this_slice.expolygons);
                                if (parent_slice.expolygons.empty()) {
                                    this_slice  .expolygons.clear();
                                } else {
                                    this_slice  .expolygons = intersection_ex(parent_slice.expolygons, source);
                                    parent_slice.expolygons = diff_ex        (parent_slice.expolygons, source);
                                }
                                if (next_region_same_modifier)
                                    // To be used in the following iteration.
                                    temp_slices[idx_region + 1].expolygons = std::move(source);
                            } else if ((region.model_volume->is_model_part() && clip_multipart_objects) || region.model_volume->is_negative_volume()) {
                                // Clip every non-zero region preceding it.
                                for (int idx_region2 = 0; idx_region2 < idx_region; ++ idx_region2)
                                    if (! temp_slices[idx_region2].expolygons.empty()) {
                                        // Skip trim_overlap for now, because it slow down the performace so much for some special cases
#if 1
                                        if (const PrintObjectRegions::VolumeRegion& region2 = layer_range.volume_regions[idx_region2];
                                            !region2.model_volume->is_negative_volume() && overlap_in_xy(*region.bbox, *region2.bbox))
                                            temp_slices[idx_region2].expolygons = diff_ex(temp_slices[idx_region2].expolygons, temp_slices[idx_region].expolygons);
#else
                                        const PrintObjectRegions::VolumeRegion& region2 = layer_range.volume_regions[idx_region2];
                                        if (!region2.model_volume->is_negative_volume() && overlap_in_xy(*region.bbox, *region2.bbox))
                                            //BBS: handle negative_volume seperately, always minus the negative volume and don't need to trim overlap
                                            if (!region.model_volume->is_negative_volume())
                                                trim_overlap(temp_slices[idx_region2].expolygons, temp_slices[idx_region].expolygons);
                                            else
                                                temp_slices[idx_region2].expolygons = diff_ex(temp_slices[idx_region2].expolygons, temp_slices[idx_region].expolygons);
#endif
                                    }
                            }
                        }
                    // Sort by region_id, push empty slices to the end.
                    std::sort(temp_slices.begin(), temp_slices.end());
                    // Remove the empty slices.
                    temp_slices.erase(std::find_if(temp_slices.begin(), temp_slices.end(), [](const auto &slice) { return slice.region_id == -1 || slice.expolygons.empty(); }), temp_slices.end());
                    // Merge slices and store them to the output.
                    for (int i = 0; i < int(temp_slices.size());) {
                        // Find a range of temp_slices with the same region_id.
                        int j = i;
                        bool merged = false;
                        ExPolygons &expolygons = temp_slices[i].expolygons;
                        for (++ j; j < int(temp_slices.size()) && temp_slices[i].region_id == temp_slices[j].region_id; ++ j)
                            if (ExPolygons &expolygons2 = temp_slices[j].expolygons; ! expolygons2.empty()) {
                                if (expolygons.empty()) {
                                    expolygons = std::move(expolygons2);
                                } else {
                                    append(expolygons, std::move(expolygons2));
                                    merged = true;
                                }
                            }
                        // Don't unite the regions if ! clip_multipart_objects. In that case it is user's responsibility
                        // to handle region overlaps. Indeed, one may intentionally let the regions overlap to produce crossing perimeters
                        // for example.
                        if (merged && clip_multipart_objects)
                            expolygons = closing_ex(expolygons, float(scale_(EPSILON)));
                        slices_by_region[temp_slices[i].region_id][z_idx] = std::move(expolygons);
                        i = j;
                    }
                    throw_on_cancel_callback();
                }
            });
    }

    return slices_by_region;
}

//BBS: justify whether a volume is connected to another one
bool doesVolumeIntersect(VolumeSlices& vs1, VolumeSlices& vs2)
{
    if (vs1.volume_id == vs2.volume_id) return true;
    // two volumes in the same object should have same number of layers, otherwise the slicing is incorrect.
    if (vs1.slices.size() != vs2.slices.size()) return false;

    auto& vs1s = vs1.slices;
    auto& vs2s = vs2.slices;
    bool is_intersect = false;

    tbb::parallel_for(tbb::blocked_range<int>(0, vs1s.size()),
        [&vs1s, &vs2s, &is_intersect](const tbb::blocked_range<int>& range) {
            for (auto i = range.begin(); i != range.end(); ++i) {
                if (vs1s[i].empty()) continue;

                if (overlaps(vs1s[i], vs2s[i])) {
                    is_intersect = true;
                    break;
                }
                if (i + 1 != vs2s.size() && overlaps(vs1s[i], vs2s[i + 1])) {
                    is_intersect = true;
                    break;
                }
                if (i - 1 >= 0 && overlaps(vs1s[i], vs2s[i - 1])) {
                    is_intersect = true;
                    break;
                }
            }
        });
    return is_intersect;
}

//BBS: grouping the volumes of an object according to their connection relationship
bool groupingVolumes(std::vector<VolumeSlices> objSliceByVolume, std::vector<groupedVolumeSlices>& groups, double resolution, int firstLayerReplacedBy)
{
    std::vector<int> groupIndex(objSliceByVolume.size(), -1);
    double offsetValue = 0.05 / SCALING_FACTOR;

    std::vector<std::vector<int>> osvIndex;
    for (int i = 0; i != objSliceByVolume.size(); ++i) {
        for (int j = 0; j != objSliceByVolume[i].slices.size(); ++j) {
            osvIndex.push_back({ i,j });
        }
    }

    tbb::parallel_for(tbb::blocked_range<int>(0, osvIndex.size()),
        [&osvIndex, &objSliceByVolume, &offsetValue, &resolution](const tbb::blocked_range<int>& range) {
            for (auto k = range.begin(); k != range.end(); ++k) {
                for (ExPolygon& poly_ex : objSliceByVolume[osvIndex[k][0]].slices[osvIndex[k][1]])
                    poly_ex.douglas_peucker(resolution);
            }
        });

    tbb::parallel_for(tbb::blocked_range<int>(0, osvIndex.size()),
        [&osvIndex, &objSliceByVolume,&offsetValue, &resolution](const tbb::blocked_range<int>& range) {
            for (auto k = range.begin(); k != range.end(); ++k) {
                objSliceByVolume[osvIndex[k][0]].slices[osvIndex[k][1]] = offset_ex(objSliceByVolume[osvIndex[k][0]].slices[osvIndex[k][1]], offsetValue);
            }
        });

    for (int i = 0; i != objSliceByVolume.size(); ++i) {
        if (groupIndex[i] < 0) {
            groupIndex[i] = i;
        }
        for (int j = i + 1; j != objSliceByVolume.size(); ++j) {
            if (doesVolumeIntersect(objSliceByVolume[i], objSliceByVolume[j])) {
                if (groupIndex[j] < 0) groupIndex[j] = groupIndex[i];
                if (groupIndex[j] != groupIndex[i]) {
                    int retain = std::min(groupIndex[i], groupIndex[j]);
                    int cover = std::max(groupIndex[i], groupIndex[j]);
                    for (int k = 0; k != objSliceByVolume.size(); ++k) {
                        if (groupIndex[k] == cover) groupIndex[k] = retain;
                    }
                }
            }

        }
    }

    std::vector<int> groupVector{};
    for (int gi : groupIndex) {
        bool exist = false;
        for (int gv : groupVector) {
            if (gv == gi) {
                exist = true;
                break;
            }
        }
        if (!exist) groupVector.push_back(gi);
    }

    // group volumes and their slices according to the grouping Vector
    groups.clear();

    for (int gv : groupVector) {
        groupedVolumeSlices gvs;
        gvs.groupId = gv;
        for (int i = 0; i != objSliceByVolume.size(); ++i) {
            if (groupIndex[i] == gv) {
                gvs.volume_ids.push_back(objSliceByVolume[i].volume_id);
                append(gvs.slices, objSliceByVolume[i].slices[firstLayerReplacedBy]);
            }
        }

        // the slices of a group should be unioned
        gvs.slices = offset_ex(union_ex(gvs.slices), -offsetValue);
        for (ExPolygon& poly_ex : gvs.slices)
            poly_ex.douglas_peucker(resolution);

        groups.push_back(gvs);
    }
    return true;
}

//BBS: filter the members of "objSliceByVolume" such that only "model_part" are included
std::vector<VolumeSlices> findPartVolumes(const std::vector<VolumeSlices>& objSliceByVolume, ModelVolumePtrs model_volumes) {
    std::vector<VolumeSlices> outPut;
    for (const auto& vs : objSliceByVolume) {
        for (const auto& mv : model_volumes) {
            if (vs.volume_id == mv->id() && mv->is_model_part()) outPut.push_back(vs);
        }
    }
    return outPut;
}

void applyNegtiveVolumes(ModelVolumePtrs model_volumes, const std::vector<VolumeSlices>& objSliceByVolume, std::vector<groupedVolumeSlices>& groups, double resolution) {
    ExPolygons negTotal;
    for (const auto& vs : objSliceByVolume) {
        for (const auto& mv : model_volumes) {
            if (vs.volume_id == mv->id() && mv->is_negative_volume()) {
                if (vs.slices.size() > 0) {
                    append(negTotal, vs.slices.front());
                }
            }
        }
    }

    for (auto& g : groups) {
        g.slices = diff_ex(g.slices, negTotal);
        for (ExPolygon& poly_ex : g.slices)
            poly_ex.douglas_peucker(resolution);
    }
}

void reGroupingLayerPolygons(std::vector<groupedVolumeSlices>& gvss, ExPolygons &eps, double resolution)
{
    std::vector<int> epsIndex;
    epsIndex.resize(eps.size(), -1);

    auto gvssc = gvss;
    auto epsc = eps;

    for (ExPolygon& poly_ex : epsc)
        poly_ex.douglas_peucker(resolution);

    for (int i = 0; i != gvssc.size(); ++i) {
        for (ExPolygon& poly_ex : gvssc[i].slices)
            poly_ex.douglas_peucker(resolution);
    }

    tbb::parallel_for(tbb::blocked_range<int>(0, epsc.size()),
        [&epsc, &gvssc, &epsIndex](const tbb::blocked_range<int>& range) {
            for (auto ie = range.begin(); ie != range.end(); ++ie) {
                if (epsc[ie].area() <= 0)
                    continue;

                double minArea = epsc[ie].area();
                for (int iv = 0; iv != gvssc.size(); iv++) {
                    auto clipedExPolys = diff_ex(epsc[ie], gvssc[iv].slices);
                    double area = 0;
                    for (const auto& ce : clipedExPolys) {
                        area += ce.area();
                    }
                    if (area < minArea) {
                        minArea = area;
                        epsIndex[ie] = iv;
                    }
                }
            }
        });

    for (int iv = 0; iv != gvss.size(); iv++)
        gvss[iv].slices.clear();

    for (int ie = 0; ie != eps.size(); ie++) {
        if (epsIndex[ie] >= 0)
            gvss[epsIndex[ie]].slices.push_back(eps[ie]);
    }
}

/*
std::string fix_slicing_errors(PrintObject* object, LayerPtrs &layers, const std::function<void()> &throw_if_canceled, int &firstLayerReplacedBy)
{
    std::string error_msg;//BBS

    if (layers.size() == 0) return error_msg;

    // Collect layers with slicing errors.
    // These layers will be fixed in parallel.
    std::vector<size_t> buggy_layers;
    buggy_layers.reserve(layers.size());
    // BBS: get largest external perimenter width of all layers
    auto get_ext_peri_width = [](Layer* layer) {return layer->m_regions.empty() ? 0 : layer->m_regions[0]->flow(frExternalPerimeter).scaled_width(); };
    auto it = std::max_element(layers.begin(), layers.end(), [get_ext_peri_width](auto& a, auto& b) {return get_ext_peri_width(a) < get_ext_peri_width(b); });
    coord_t thresh = get_ext_peri_width(*it) * 0.5;// half of external perimeter width  // 0.5 * scale_(this->config().line_width);
    for (size_t idx_layer = 0; idx_layer < layers.size(); ++idx_layer) {
        // BBS: detect empty layers (layers with very small regions) and mark them as problematic, then these layers will copy the nearest good layer
        auto layer = layers[idx_layer];
        ExPolygons lslices;
        for (size_t region_id = 0; region_id < layer->m_regions.size(); ++region_id) {
            LayerRegion* layerm = layer->m_regions[region_id];
            for (auto& surface : layerm->slices.surfaces) {
                auto expoly = offset_ex(surface.expolygon, -thresh);
                lslices.insert(lslices.begin(), expoly.begin(), expoly.end());
            }
        }
        if (lslices.empty()) {
            layer->slicing_errors = true;
        }

        if (layers[idx_layer]->slicing_errors) {
            buggy_layers.push_back(idx_layer);
        }
        else
            break; // only detect empty layers near bed
    }

    BOOST_LOG_TRIVIAL(debug) << "Slicing objects - fixing slicing errors in parallel - begin";
    std::atomic<bool> is_replaced = false;
    tbb::parallel_for(
        tbb::blocked_range<size_t>(0, buggy_layers.size()),
        [&layers, &throw_if_canceled, &buggy_layers, &is_replaced](const tbb::blocked_range<size_t>& range) {
            for (size_t buggy_layer_idx = range.begin(); buggy_layer_idx < range.end(); ++ buggy_layer_idx) {
                throw_if_canceled();
                size_t idx_layer = buggy_layers[buggy_layer_idx];
                // BBS: only replace empty layers lower than 1mm
                const coordf_t thresh_empty_layer_height = 1;
                Layer* layer = layers[idx_layer];
                if (layer->print_z>= thresh_empty_layer_height)
                    continue;
                assert(layer->slicing_errors);
                // Try to repair the layer surfaces by merging all contours and all holes from neighbor layers.
                // BOOST_LOG_TRIVIAL(trace) << "Attempting to repair layer" << idx_layer;
                for (size_t region_id = 0; region_id < layer->region_count(); ++ region_id) {
                    LayerRegion *layerm = layer->get_region(region_id);
                    // Find the first valid layer below / above the current layer.
                    const Surfaces *upper_surfaces = nullptr;
                    const Surfaces *lower_surfaces = nullptr;
                    //BBS: only repair empty layers lowers than 1mm
                    for (size_t j = idx_layer + 1; j < layers.size(); ++j) {
                        if (!layers[j]->slicing_errors) {
                            upper_surfaces = &layers[j]->regions()[region_id]->slices.surfaces;
                            break;
                        }
                        if (layers[j]->print_z >= thresh_empty_layer_height) break;
                    }
                    for (int j = int(idx_layer) - 1; j >= 0; --j) {
                        if (layers[j]->print_z >= thresh_empty_layer_height) continue;
                        if (!layers[j]->slicing_errors) {
                            lower_surfaces = &layers[j]->regions()[region_id]->slices.surfaces;
                            break;
                        }
                    }
                    // Collect outer contours and holes from the valid layers above & below.
                    ExPolygons expolys;
                    expolys.reserve(
                        ((upper_surfaces == nullptr) ? 0 : upper_surfaces->size()) +
                        ((lower_surfaces == nullptr) ? 0 : lower_surfaces->size()));
                    if (upper_surfaces)
                        for (const auto &surface : *upper_surfaces) {
                            expolys.emplace_back(surface.expolygon);
                        }
                    if (lower_surfaces)
                        for (const auto &surface : *lower_surfaces) {
                            expolys.emplace_back(surface.expolygon);
                        }
                    if (!expolys.empty()) {
                        //BBS
                        is_replaced = true;
                        layerm->slices.set(union_ex(expolys), stInternal);
                    }
                }
                // Update layer slices after repairing the single regions.
                layer->make_slices();
            }
        });
    throw_if_canceled();
    BOOST_LOG_TRIVIAL(debug) << "Slicing objects - fixing slicing errors in parallel - end";

    if(is_replaced)
        error_msg = L("Empty layers around bottom are replaced by nearest normal layers.");

    // remove empty layers from bottom
    while (! layers.empty() && (layers.front()->lslices.empty() || layers.front()->empty())) {
        delete layers.front();
        layers.erase(layers.begin());
        layers.front()->lower_layer = nullptr;
        for (size_t i = 0; i < layers.size(); ++ i)
            layers[i]->set_id(layers[i]->id() - 1);
    }

    //BBS
    if(error_msg.empty() && !buggy_layers.empty())
        error_msg = L("The model has too many empty layers.");

    // BBS: first layer slices are sorted by volume group, if the first layer is empty and replaced by the 2nd layer
// the later will be stored in "object->firstLayerObjGroupsMod()"
    if (!buggy_layers.empty() && buggy_layers.front() == 0 && layers.size() > 1)
        firstLayerReplacedBy = 1;

    return error_msg;
}
*/

void groupingVolumesForBrim(PrintObject* object, LayerPtrs& layers, int firstLayerReplacedBy)
{
    const auto           scaled_resolution = scaled<double>(object->print()->config().resolution.value);
    auto partsObjSliceByVolume = findPartVolumes(object->firstLayerObjSliceMod(), object->model_object()->volumes);
    groupingVolumes(partsObjSliceByVolume, object->firstLayerObjGroupsMod(), scaled_resolution, firstLayerReplacedBy);
    applyNegtiveVolumes(object->model_object()->volumes, object->firstLayerObjSliceMod(), object->firstLayerObjGroupsMod(), scaled_resolution);

    // BBS: the actual first layer slices stored in layers are re-sorted by volume group and will be used to generate brim
    reGroupingLayerPolygons(object->firstLayerObjGroupsMod(), layers.front()->lslices, scaled_resolution);
}

// Called by make_perimeters()
// 1) Decides Z positions of the layers,
// 2) Initializes layers and their regions
// 3) Slices the object meshes
// 4) Slices the modifier meshes and reclassifies the slices of the object meshes by the slices of the modifier meshes
// 5) Applies size compensation (offsets the slices in XY plane)
// 6) Replaces bad slices by the slices reconstructed from the upper/lower layer
// Resulting expolygons of layer regions are marked as Internal.
void PrintObject::slice()
{
    if (! this->set_started(posSlice))
        return;
    //BBS: add flag to reload scene for shell rendering
    m_print->set_status(5, L("Slicing mesh"), PrintBase::SlicingStatus::RELOAD_SCENE);
    std::vector<coordf_t> layer_height_profile;
    this->update_layer_height_profile(*this->model_object(), m_slicing_params, layer_height_profile, this);
    m_print->throw_if_canceled();
    m_typed_slices = false;
    this->clear_layers();
    m_layers = new_layers(this, generate_object_layers(m_slicing_params, layer_height_profile, m_config.precise_z_height.value));
    if (has_surface_emboss_mixed_volume(*this)) {
        reset_surface_emboss_mixed_debug_file(*this);
        BOOST_LOG_TRIVIAL(warning) << "Surface emboss mixed debug enabled"
                                   << " object=" << (this->model_object() ? this->model_object()->name : std::string("<unknown>"))
                                   << " debug_file=" << surface_emboss_mixed_debug_file_path(*this);
    }
    this->slice_volumes();
    m_print->throw_if_canceled();
    int firstLayerReplacedBy = 0;

#if 0
    // Fix the model.
    //FIXME is this the right place to do? It is done repeateadly at the UI and now here at the backend.
    std::string warning = fix_slicing_errors(this, m_layers, [this](){ m_print->throw_if_canceled(); }, firstLayerReplacedBy);
    m_print->throw_if_canceled();
    //BBS: send warning message to slicing callback
    // This warning is inaccurate, because the empty layers may have been replaced, or the model has supports.
    //if (!warning.empty()) {
    //    BOOST_LOG_TRIVIAL(info) << warning;
    //    this->active_step_add_warning(PrintStateBase::WarningLevel::CRITICAL, warning, PrintStateBase::SlicingReplaceInitEmptyLayers);
    //}
#endif

    // Detect and process holes that should be converted to polyholes
    this->_transform_hole_to_polyholes();

    // BBS: the actual first layer slices stored in layers are re-sorted by volume group and will be used to generate brim
    groupingVolumesForBrim(this, m_layers, firstLayerReplacedBy);

    // Update bounding boxes, back up raw slices of complex models.
    tbb::parallel_for(
        tbb::blocked_range<size_t>(0, m_layers.size()),
        [this](const tbb::blocked_range<size_t>& range) {
            for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++ layer_idx) {
                m_print->throw_if_canceled();
                Layer &layer = *m_layers[layer_idx];
                layer.lslices_bboxes.clear();
                layer.lslices_bboxes.reserve(layer.lslices.size());
                for (const ExPolygon &expoly : layer.lslices)
                	layer.lslices_bboxes.emplace_back(get_extents(expoly));
                layer.backup_untyped_slices();
            }
        });
    if (m_layers.empty())
        throw Slic3r::SlicingError(L("No layers were detected. You might want to repair your STL file(s) or check their size or thickness and retry.\n"));

    // BBS
    this->set_done(posSlice);
}

static bool bool_from_full_config(const DynamicPrintConfig &full_cfg, const char *key, bool fallback)
{
    if (!full_cfg.has(key))
        return fallback;
    if (const ConfigOptionBool *opt = full_cfg.option<ConfigOptionBool>(key))
        return opt->value;
    if (const ConfigOptionInt *opt = full_cfg.option<ConfigOptionInt>(key))
        return opt->value != 0;
    return fallback;
}

static coordf_t float_from_full_config(const DynamicPrintConfig &full_cfg, const char *key, coordf_t fallback)
{
    if (!full_cfg.has(key))
        return fallback;
    if (const ConfigOptionFloat *opt = full_cfg.option<ConfigOptionFloat>(key))
        return coordf_t(opt->value);
    return coordf_t(full_cfg.opt_float(key));
}

// Forward declarations — defined after the Task 30/31 helpers further below.
template<typename ThrowOnCancel>
static void build_local_z_plan(PrintObject &print_object, const std::vector<std::vector<ExPolygons>> &segmentation, ThrowOnCancel throw_on_cancel);
// Task 30 helpers (also defined below).
static std::vector<std::vector<ExPolygons>> whole_object_local_z_segmentation_by_mixed_wall(const PrintObject &print_object);
static std::vector<std::vector<ExPolygons>> local_z_planner_segmentation_with_whole_object_mixed_wall(
    const PrintObject &print_object, const std::vector<std::vector<ExPolygons>> &paint_segmentation);
// Surface-emboss override is defined alongside the surface_emboss_mixed debug subsystem far
// below; declared here so slice_volumes() can call it.
template<typename ThrowOnCancel>
static bool apply_surface_emboss_mixed_region_override(PrintObject &print_object, ThrowOnCancel throw_on_cancel);

// Forward declarations for FS mixed-filament surface helpers, defined further below alongside
// segmentation_channel_filament_id / collect_layer_region_slices.
static inline unsigned int segmentation_channel_filament_id(size_t channel_idx);
static coordf_t clamped_mixed_component_surface_offset(const MixedFilamentManager &mixed_mgr,
                                                       const PrintConfig          &print_cfg,
                                                       unsigned int                filament_id,
                                                       size_t                      num_physical,
                                                       int                         layer_index,
                                                       float                       layer_print_z,
                                                       float                       layer_height,
                                                       bool                        force_height_weighted = false);

template<typename ThrowOnCancel>
static inline void apply_mm_segmentation(PrintObject &print_object, std::vector<std::vector<ExPolygons>> segmentation, ThrowOnCancel throw_on_cancel)
{
    assert(segmentation.size() == print_object.layer_count());
    const PrintConfig        &print_cfg = print_object.print()->config();
    const DynamicPrintConfig &full_cfg  = print_object.print()->full_print_config();
    const size_t              num_physical = print_cfg.filament_diameter.size();
    const coordf_t            preferred_a = float_from_full_config(full_cfg, "mixed_color_layer_height_a",
                                                                   coordf_t(print_cfg.mixed_color_layer_height_a.value));
    const coordf_t            preferred_b = float_from_full_config(full_cfg, "mixed_color_layer_height_b",
                                                                   coordf_t(print_cfg.mixed_color_layer_height_b.value));
    const coordf_t            base_height = std::max<coordf_t>(0.01f, coordf_t(print_object.config().layer_height.value));
    const bool                collapse_mixed_regions =
        bool_from_full_config(full_cfg, "mixed_filament_region_collapse", print_cfg.mixed_filament_region_collapse.value);
    const bool                bias_mode_enabled =
        bool_from_full_config(full_cfg, "mixed_filament_component_bias_enabled", print_cfg.mixed_filament_component_bias_enabled.value);
    const MixedFilamentManager &mixed_mgr = print_object.print()->mixed_filament_manager();

    tbb::parallel_for(
        tbb::blocked_range<size_t>(0, segmentation.size(), std::max(segmentation.size() / 128, size_t(1))),
        [&print_object, &segmentation, &mixed_mgr, num_physical, preferred_a, preferred_b, base_height, collapse_mixed_regions, bias_mode_enabled, throw_on_cancel](const tbb::blocked_range<size_t> &range) {
            const auto  &layer_ranges   = print_object.shared_regions()->layer_ranges;
            double       z              = print_object.get_layer(int(range.begin()))->slice_z;
            auto         it_layer_range = layer_range_first(layer_ranges, z);
            // MM segmentation channel 0 is the underlying / default color of the parent
            // region. Remaining channels correspond to filament IDs (1-based), which
            // now include enabled mixed / virtual filaments.
            const size_t num_channels  = segmentation.empty() ? 0 : segmentation.front().size();
            const size_t num_extruders = num_channels > 0 ? num_channels - 1 : 0;

            struct ByExtruder {
                ExPolygons  expolygons;
                BoundingBox bbox;
            };

            auto intersect_surfaces_preserve_types = [](const SurfaceCollection &src, const ExPolygons &mask) {
                SurfaceCollection out;
                if (src.empty() || mask.empty())
                    return out;

                std::array<SurfacesPtr, size_t(stCount)> by_surface;
                for (const Surface &surface : src.surfaces)
                    by_surface[size_t(surface.surface_type)].emplace_back(&surface);

                for (size_t surface_type = 0; surface_type < size_t(stCount); ++surface_type) {
                    const SurfacesPtr &typed_surfaces = by_surface[surface_type];
                    if (typed_surfaces.empty())
                        continue;
                    ExPolygons clipped = intersection_ex(typed_surfaces, mask);
                    if (!clipped.empty())
                        out.append(std::move(clipped), SurfaceType(surface_type));
                }
                return out;
            };

            struct ByRegion {
                SurfaceCollection surfaces;
                bool       needs_merge { false };
            };

            auto normalize_region_surfaces = [](SurfaceCollection &src) {
                if (src.surfaces.empty())
                    return;

                std::array<ExPolygons, size_t(stCount)> by_surface;
                for (Surface &surface : src.surfaces)
                    by_surface[size_t(surface.surface_type)].emplace_back(std::move(surface.expolygon));

                src.surfaces.clear();
                for (size_t surface_type = 0; surface_type < size_t(stCount); ++surface_type) {
                    ExPolygons &typed = by_surface[surface_type];
                    if (typed.empty())
                        continue;
                    if (typed.size() > 1)
                        typed = closing_ex(std::move(typed), scaled<float>(10. * EPSILON));
                    src.append(std::move(typed), SurfaceType(surface_type));
                }
            };

            std::vector<ByExtruder> by_extruder;
            std::vector<ByRegion>   by_region;
            for (size_t layer_id = range.begin(); layer_id < range.end(); ++layer_id) {
                throw_on_cancel();
                Layer &layer = *print_object.get_layer(int(layer_id));
                it_layer_range = layer_range_next(layer_ranges, it_layer_range, layer.slice_z);
                const PrintObjectRegions::LayerRangeRegions &layer_range = *it_layer_range;
                // Gather per extruder expolygons.
                assert(segmentation[layer_id].size() == num_channels);
                by_extruder.assign(num_extruders, ByExtruder());
                by_region.assign(layer.region_count(), ByRegion());
                bool layer_split = false;
                size_t missing_target_regions = 0;
                std::vector<int> missing_target_extruders;
                ExPolygons  default_segmentation = num_channels > 0 ? std::move(segmentation[layer_id][0]) : ExPolygons();
                BoundingBox default_bbox;
                bool        layer_has_component_bias = false;
                if (!default_segmentation.empty()) {
                    default_bbox = get_extents(default_segmentation);
                    layer_split  = true;
                }
                for (size_t channel_idx = 1; channel_idx < num_channels; ++ channel_idx) {
                    const unsigned int channel_id = unsigned(channel_idx);
                    const unsigned int effective_filament_id = collapse_mixed_regions ?
                        mixed_mgr.effective_painted_region_filament_id(channel_id,
                                                                       num_physical,
                                                                       int(layer_id),
                                                                       float(layer.print_z),
                                                                       float(layer.height),
                                                                       float(preferred_a),
                                                                       float(preferred_b),
                                                                       float(base_height)) :
                        channel_id;
                    const size_t effective_idx =
                        effective_filament_id >= 1 && effective_filament_id <= num_extruders ? size_t(effective_filament_id - 1) : size_t(channel_idx - 1);
                    ByExtruder &region = by_extruder[effective_idx];
                    append(region.expolygons, std::move(segmentation[layer_id][channel_idx]));
                    if (! region.expolygons.empty()) {
                        region.bbox = get_extents(region.expolygons);
                        layer_split = true;
                    }

                    if (!region.expolygons.empty() &&
                        bias_mode_enabled &&
                        mixed_mgr.is_mixed(channel_id, num_physical) &&
                        std::abs(mixed_mgr.component_surface_offset(channel_id,
                                                                   num_physical,
                                                                   int(layer_id),
                                                                   float(layer.print_z),
                                                                   float(layer.height))) > EPSILON)
                        layer_has_component_bias = true;
                }

                if (!layer_split)
                    continue;

                ExPolygons  layer_geometry_mask;
                BoundingBox layer_geometry_bbox;
                if (layer_has_component_bias) {
                    if (!default_segmentation.empty())
                        append(layer_geometry_mask, default_segmentation);
                    for (const ByExtruder &segmented : by_extruder) {
                        if (!segmented.expolygons.empty())
                            append(layer_geometry_mask, segmented.expolygons);
                    }
                    if (!layer_geometry_mask.empty()) {
                        if (layer_geometry_mask.size() > 1)
                            layer_geometry_mask = closing_ex(union_ex(std::move(layer_geometry_mask)), scaled<float>(5. * EPSILON));
                        layer_geometry_bbox = get_extents(layer_geometry_mask);
                    }
                }

                // Split LayerRegions by by_extruder regions.
                // layer_range.painted_regions are sorted by extruder ID and parent PrintObject region ID.
                auto it_painted_region_begin = layer_range.painted_regions.cbegin();
                for (int parent_layer_region_idx = 0; parent_layer_region_idx < layer.region_count(); ++parent_layer_region_idx) {
                    const LayerRegion &parent_layer_region = *layer.get_region(parent_layer_region_idx);
                    const PrintRegion &parent_print_region = parent_layer_region.region();
                    assert(parent_print_region.print_object_region_id() == parent_layer_region_idx);
                    if (parent_layer_region.slices.empty())
                        continue;

                    auto preserve_parent_region = [&by_region, &parent_layer_region, &parent_print_region]() {
                        if (!parent_layer_region.slices.empty())
                            by_region[parent_print_region.print_object_region_id()].surfaces = parent_layer_region.slices;
                    };

                    if (it_painted_region_begin == layer_range.painted_regions.cend()) {
                        preserve_parent_region();
                        continue;
                    }

                    // Find the first PaintedRegion, which overrides the parent PrintRegion.
                    auto it_first_painted_region = std::find_if(it_painted_region_begin, layer_range.painted_regions.cend(), [&layer_range, &parent_print_region](const auto &painted_region) {
                        return layer_range.volume_regions[painted_region.parent].region->print_object_region_id() == parent_print_region.print_object_region_id();
                    });

                    if (it_first_painted_region == layer_range.painted_regions.cend()) {
                        preserve_parent_region();
                        continue; // This LayerRegion isn't overrides by any PaintedRegion.
                    }

                    assert(&parent_print_region == layer_range.volume_regions[it_first_painted_region->parent].region);

                    // Update the beginning PaintedRegion iterator for the next iteration.
                    it_painted_region_begin = it_first_painted_region;

                    const BoundingBox parent_layer_region_bbox = get_extents(parent_layer_region.slices.surfaces);
                    const bool        clamp_parent_to_geometry =
                        layer_has_component_bias &&
                        layer_geometry_bbox.defined &&
                        parent_layer_region_bbox.overlap(layer_geometry_bbox);
                    ExPolygons        clamped_parent_expolygons;
                    if (clamp_parent_to_geometry)
                        clamped_parent_expolygons = intersection_ex(parent_layer_region.slices.surfaces, layer_geometry_mask);

                    int               self_extruder_id         = -1; // 1-based extruder ID
                    ExPolygons        explicit_self_expolygons;
                    ExPolygons        default_self_expolygons;
                    if (const int cfg_wall = parent_print_region.config().wall_filament.value;
                        cfg_wall >= 1 && cfg_wall <= int(by_extruder.size()))
                        self_extruder_id = cfg_wall;
                    if (default_bbox.defined && parent_layer_region_bbox.overlap(default_bbox))
                        default_self_expolygons = intersection_ex(parent_layer_region.slices.surfaces, default_segmentation);
                    std::vector<bool> assigned_extruder(by_extruder.size(), false);
                    std::vector<int>  alias_to_self_extruders;
                    for (int extruder_id = 1; extruder_id <= int(by_extruder.size()); ++extruder_id) {
                        const ByExtruder &segmented = by_extruder[extruder_id - 1];
                        if (!segmented.bbox.defined || !parent_layer_region_bbox.overlap(segmented.bbox))
                            continue;

                        // Find the matching target region for this parent and extruder ID.
                        auto it_target_region = std::find_if(it_painted_region_begin, layer_range.painted_regions.cend(), [&layer_range, &parent_print_region, extruder_id](const auto &painted_region) {
                            return layer_range.volume_regions[painted_region.parent].region == &parent_print_region &&
                                   int(painted_region.extruder_id) == extruder_id;
                        });

                        if (it_target_region == layer_range.painted_regions.cend()) {
                            ++missing_target_regions;
                            missing_target_extruders.emplace_back(extruder_id);
                            continue;
                        }

                        // Update the beginning PaintedRegion iterator for the next iteration.
                        it_painted_region_begin = it_target_region;

                        // FIXME: Don't trim by self, it is not reliable.
                        if (it_target_region->region == &parent_print_region) {
                            if (self_extruder_id < 0)
                                self_extruder_id = extruder_id;
                            if (extruder_id != self_extruder_id)
                                alias_to_self_extruders.emplace_back(extruder_id);
                            ExPolygons self_segmented = intersection_ex(parent_layer_region.slices.surfaces, segmented.expolygons);
                            if (!self_segmented.empty()) {
                                if (explicit_self_expolygons.empty())
                                    explicit_self_expolygons = std::move(self_segmented);
                                else
                                    append(explicit_self_expolygons, std::move(self_segmented));
                            }
                            continue;
                        }

                        assigned_extruder[size_t(extruder_id - 1)] = true;

                        // Steal from this region.
                        int        target_region_id = it_target_region->region->print_object_region_id();
                        ExPolygons stolen           = intersection_ex(parent_layer_region.slices.surfaces, segmented.expolygons);
                        if (!stolen.empty()) {
                            ByRegion &dst = by_region[target_region_id];
                            SurfaceCollection stolen_surfaces = intersect_surfaces_preserve_types(parent_layer_region.slices, stolen);
                            if (stolen_surfaces.empty())
                                continue;
                            if (dst.surfaces.empty()) {
                                dst.surfaces = std::move(stolen_surfaces);
                            } else {
                                dst.surfaces.append(std::move(stolen_surfaces));
                                dst.needs_merge = true;
                            }
                        }
                    }

                    // Trim slices of this LayerRegion with all the MM regions.
                    // Mixed bias can intentionally shrink a painted layer's true silhouette.
                    // Clamp the parent region to the post-bias segmentation union so the
                    // vacated area stays empty instead of falling back to the parent tool.
                    Polygons mine = clamp_parent_to_geometry ? to_polygons(clamped_parent_expolygons) :
                                                              to_polygons(parent_layer_region.slices.surfaces);
                    for (size_t extruder_idx = 0; extruder_idx < by_extruder.size(); ++extruder_idx) {
                        const ByExtruder &segmented = by_extruder[extruder_idx];
                        if (!assigned_extruder[extruder_idx])
                            continue;
                        if (int(extruder_idx + 1) != self_extruder_id && segmented.bbox.defined && parent_layer_region_bbox.overlap(segmented.bbox)) {
                            mine = diff(mine, segmented.expolygons);
                            if (mine.empty())
                                break;
                        }
                    }

                    if (!explicit_self_expolygons.empty())
                        explicit_self_expolygons = union_ex(explicit_self_expolygons);
                    if (!default_self_expolygons.empty())
                        default_self_expolygons = union_ex(default_self_expolygons);

                    ExPolygons preserved_self_expolygons;
                    if (!explicit_self_expolygons.empty())
                        append(preserved_self_expolygons, explicit_self_expolygons);
                    if (!default_self_expolygons.empty())
                        append(preserved_self_expolygons, default_self_expolygons);
                    if (!preserved_self_expolygons.empty())
                        preserved_self_expolygons = union_ex(preserved_self_expolygons);

                    ExPolygons mine_expolygons;
                    if (!mine.empty()) {
                        if (!preserved_self_expolygons.empty())
                            mine = diff(mine, preserved_self_expolygons);

                        // Filter out unprintable polygons produced by subtraction multi-material painted regions from layerm.region().
                        // ExPolygon returned from multi-material segmentation does not precisely match ExPolygons in layerm.region()
                        // (because of preprocessing of the input regions in multi-material segmentation). Therefore, subtraction from
                        // layerm.region() could produce a huge number of small unprintable regions for the model's base extruder.
                        // This could, on some models, produce bulges with the model's base color (#7109).
                        if (!mine.empty())
                            mine = opening(union_ex(mine), scaled<float>(5. * EPSILON), scaled<float>(5. * EPSILON));

                        if (!mine.empty())
                            mine_expolygons = union_ex(mine);
                    }

                    if (!preserved_self_expolygons.empty()) {
                        append(mine_expolygons, preserved_self_expolygons);
                        mine_expolygons = union_ex(mine_expolygons);
                    }

                    if (!mine_expolygons.empty()) {
                        SurfaceCollection mine_surfaces = intersect_surfaces_preserve_types(parent_layer_region.slices, mine_expolygons);
                        if (!mine_surfaces.empty()) {
                            ByRegion &dst = by_region[parent_print_region.print_object_region_id()];
                            if (dst.surfaces.empty()) {
                                dst.surfaces = std::move(mine_surfaces);
                            } else {
                                dst.surfaces.append(std::move(mine_surfaces));
                                dst.needs_merge = true;
                            }
                        }
                    }

                    if (!alias_to_self_extruders.empty()) {
                        std::sort(alias_to_self_extruders.begin(), alias_to_self_extruders.end());
                        alias_to_self_extruders.erase(std::unique(alias_to_self_extruders.begin(), alias_to_self_extruders.end()), alias_to_self_extruders.end());
                        std::string alias_ids;
                        for (size_t i = 0; i < alias_to_self_extruders.size(); ++i) {
                            if (i > 0)
                                alias_ids += ",";
                            alias_ids += std::to_string(alias_to_self_extruders[i]);
                        }
                        BOOST_LOG_TRIVIAL(warning) << "MM segmentation alias-to-parent channels ignored"
                                                   << " object=" << (print_object.model_object() ? print_object.model_object()->name : std::string("<unknown>"))
                                                   << " layer_id=" << layer_id
                                                   << " parent_region_id=" << parent_print_region.print_object_region_id()
                                                   << " self_extruder_id=" << self_extruder_id
                                                   << " alias_extruders=[" << alias_ids << "]";
                    }
                }

                if (missing_target_regions > 0) {
                    std::sort(missing_target_extruders.begin(), missing_target_extruders.end());
                    missing_target_extruders.erase(std::unique(missing_target_extruders.begin(), missing_target_extruders.end()), missing_target_extruders.end());
                    std::string missing_ids;
                    for (size_t i = 0; i < missing_target_extruders.size(); ++i) {
                        if (i > 0)
                            missing_ids += ",";
                        missing_ids += std::to_string(missing_target_extruders[i]);
                    }
                    BOOST_LOG_TRIVIAL(warning) << "MM segmentation missing painted target regions"
                                               << " object=" << (print_object.model_object() ? print_object.model_object()->name : std::string("<unknown>"))
                                               << " layer_id=" << layer_id
                                               << " missing_targets=" << missing_target_regions
                                               << " missing_extruders=[" << missing_ids << "]"
                                               << " segmentation_channels=" << num_extruders
                                               << " painted_regions=" << layer_range.painted_regions.size();
                }

                // Re-create Surfaces of LayerRegions.
                for (int region_id = 0; region_id < layer.region_count(); ++region_id) {
                    ByRegion &src = by_region[region_id];
                    if (src.needs_merge) {
                        // Multiple regions were merged into one.
                        normalize_region_surfaces(src.surfaces);
                    }

                    layer.get_region(region_id)->slices.set(std::move(src.surfaces));
                }

                dump_surface_emboss_mixed_layer_state("post-mm-segmentation",
                                                      print_object,
                                                      layer_id,
                                                      layer,
                                                      layer_range,
                                                      &segmentation[layer_id]);
            }
        });
}

// Adjust virtual mixed-state masks by `mixed_filament_surface_indentation` mm. Positive values
// shrink the mixed footprint (revealing the parent masks beneath); negative values expand the
// mixed footprint outward, clipping against already-occupied physical-channel masks.
static bool apply_mixed_surface_indentation(PrintObject &print_object, std::vector<std::vector<ExPolygons>> &segmentation)
{
    const Print *print = print_object.print();
    if (print == nullptr || segmentation.empty())
        return false;

    const PrintConfig        &print_cfg = print->config();
    const DynamicPrintConfig &full_cfg  = print->full_print_config();
    coordf_t indentation_mm = float_from_full_config(full_cfg, "mixed_filament_surface_indentation",
                                                     coordf_t(print_cfg.mixed_filament_surface_indentation.value));
    indentation_mm = std::clamp(indentation_mm, coordf_t(-2.f), coordf_t(2.f));
    if (std::abs(indentation_mm) <= EPSILON)
        return false;

    const size_t num_physical = print_cfg.filament_colour.size();
    const size_t num_channels = segmentation.front().size();
    if (num_channels <= num_physical)
        return false;

    const MixedFilamentManager &mixed_mgr = print->mixed_filament_manager();
    const bool  expand_outward = indentation_mm < 0.f;
    const float delta_scaled = float(scale_(std::abs(double(indentation_mm))));
    if (delta_scaled <= float(EPSILON))
        return false;

    size_t changed_layers = 0;
    size_t changed_states = 0;
    size_t emptied_states = 0;
    size_t overlap_clipped_states = 0;
    size_t outside_trimmed_states = 0;

    for (size_t layer_id = 0; layer_id < segmentation.size(); ++layer_id) {
        if (segmentation[layer_id].size() != num_channels)
            continue;

        bool       layer_changed = false;
        ExPolygons outside_trim_band;
        ExPolygons occupied;
        if (expand_outward) {
            for (size_t channel_idx = 0; channel_idx < num_channels; ++channel_idx) {
                const ExPolygons &state_masks = segmentation[layer_id][channel_idx];
                if (state_masks.empty())
                    continue;

                const unsigned int state_id = unsigned(segmentation_channel_filament_id(channel_idx));
                if (!mixed_mgr.is_mixed(state_id, num_physical))
                    append(occupied, state_masks);
            }
            if (occupied.size() > 1)
                occupied = union_ex(occupied);
        } else {
            ExPolygons layer_masks;
            for (size_t channel_idx = 0; channel_idx < num_channels; ++channel_idx) {
                const ExPolygons &state_masks = segmentation[layer_id][channel_idx];
                if (!state_masks.empty())
                    append(layer_masks, state_masks);
            }
            if (!layer_masks.empty()) {
                if (layer_masks.size() > 1)
                    layer_masks = union_ex(layer_masks);

                ExPolygons layer_inner = offset_ex(layer_masks, -delta_scaled);
                if (!layer_inner.empty() && layer_inner.size() > 1)
                    layer_inner = union_ex(layer_inner);

                outside_trim_band = layer_inner.empty() ? layer_masks : diff_ex(layer_masks, layer_inner, ApplySafetyOffset::Yes);
                if (!outside_trim_band.empty() && outside_trim_band.size() > 1)
                    outside_trim_band = union_ex(outside_trim_band);
            }
        }

        for (size_t channel_idx = num_physical; channel_idx < num_channels; ++channel_idx) {
            ExPolygons &state_masks = segmentation[layer_id][channel_idx];
            if (state_masks.empty())
                continue;

            const unsigned int state_id = unsigned(segmentation_channel_filament_id(channel_idx));
            if (!mixed_mgr.is_mixed(state_id, num_physical))
                continue;

            ExPolygons adjusted;
            if (expand_outward) {
                adjusted = offset_ex(state_masks, delta_scaled);
                if (!adjusted.empty() && adjusted.size() > 1)
                    adjusted = union_ex(adjusted);

                if (!adjusted.empty() && !occupied.empty()) {
                    ExPolygons clipped = diff_ex(adjusted, occupied, ApplySafetyOffset::Yes);
                    if (std::abs(area(clipped)) + EPSILON < std::abs(area(adjusted)))
                        ++overlap_clipped_states;
                    adjusted = std::move(clipped);
                    if (!adjusted.empty() && adjusted.size() > 1)
                        adjusted = union_ex(adjusted);
                }
            } else {
                adjusted = outside_trim_band.empty() ? state_masks : diff_ex(state_masks, outside_trim_band, ApplySafetyOffset::Yes);
                if (std::abs(area(adjusted)) + EPSILON < std::abs(area(state_masks)))
                    ++outside_trimmed_states;
                if (!adjusted.empty() && adjusted.size() > 1)
                    adjusted = union_ex(adjusted);
            }

            state_masks = std::move(adjusted);
            if (state_masks.empty())
                ++emptied_states;
            ++changed_states;
            layer_changed = true;

            if (expand_outward && !state_masks.empty()) {
                append(occupied, state_masks);
                if (occupied.size() > 1)
                    occupied = union_ex(occupied);
            }
        }

        if (layer_changed)
            ++changed_layers;
    }

    if (changed_states == 0)
        return false;

    BOOST_LOG_TRIVIAL(warning) << "Mixed surface indentation applied"
                               << " object=" << (print_object.model_object() ? print_object.model_object()->name : std::string("<unknown>"))
                               << " indentation_mm=" << indentation_mm
                               << " direction=" << (expand_outward ? "outward" : "inward")
                               << " changed_layers=" << changed_layers
                               << " changed_states=" << changed_states
                               << " emptied_states=" << emptied_states
                               << " overlap_clipped_states=" << overlap_clipped_states
                               << " outside_trimmed_states=" << outside_trimmed_states;
    return true;
}

// Apply per-component surface offset bias to mixed-channel segmentation masks. Positive offsets
// contract the channel (revealing siblings); negative offsets expand the channel and steal area
// from siblings. Disabled when dithering local-Z mode owns the layer instead.
static bool apply_mixed_component_surface_offsets(PrintObject &print_object, std::vector<std::vector<ExPolygons>> &segmentation)
{
    const Print *print = print_object.print();
    if (print == nullptr || segmentation.empty())
        return false;

    const PrintConfig        &print_cfg = print->config();
    const DynamicPrintConfig &full_cfg  = print->full_print_config();
    if (bool_from_full_config(full_cfg, "dithering_local_z_mode", print_cfg.dithering_local_z_mode.value))
        return false;
    if (!bool_from_full_config(full_cfg, "mixed_filament_component_bias_enabled", print_cfg.mixed_filament_component_bias_enabled.value))
        return false;

    const size_t num_physical = print_cfg.filament_colour.size();
    const size_t num_channels = segmentation.front().size();
    if (num_channels <= num_physical + 1)
        return false;

    const MixedFilamentManager &mixed_mgr = print->mixed_filament_manager();
    bool has_component_offsets = false;
    for (const MixedFilament &mf : mixed_mgr.mixed_filaments()) {
        if (!mf.enabled || mf.deleted)
            continue;
        if (std::abs(mf.component_a_surface_offset) > EPSILON || std::abs(mf.component_b_surface_offset) > EPSILON) {
            has_component_offsets = true;
            break;
        }
    }
    if (!has_component_offsets)
        return false;

    size_t changed_layers = 0;
    size_t changed_states = 0;
    size_t emptied_states = 0;
    size_t expanded_states = 0;
    size_t contracted_states = 0;

    for (size_t layer_id = 0; layer_id < segmentation.size(); ++layer_id) {
        if (segmentation[layer_id].size() != num_channels)
            continue;

        const Layer *layer = layer_id < size_t(print_object.layer_count()) ? print_object.get_layer(int(layer_id)) : nullptr;
        const float layer_print_z = layer ? float(layer->print_z) : 0.f;
        const float layer_height  = layer ? float(layer->height) : 0.f;
        bool layer_changed = false;

        for (size_t channel_idx = 1; channel_idx < num_channels; ++channel_idx) {
            ExPolygons &state_masks = segmentation[layer_id][channel_idx];
            if (state_masks.empty())
                continue;

            const unsigned int state_id = segmentation_channel_filament_id(channel_idx);
            if (!mixed_mgr.is_mixed(state_id, num_physical))
                continue;

            const coordf_t offset_mm = clamped_mixed_component_surface_offset(mixed_mgr,
                                                                              print_cfg,
                                                                              state_id,
                                                                              num_physical,
                                                                              int(layer_id),
                                                                              layer_print_z,
                                                                              layer_height);
            if (std::abs(offset_mm) <= EPSILON)
                continue;

            const float delta_scaled = float(scale_(std::abs(double(offset_mm))));
            if (delta_scaled <= float(EPSILON))
                continue;

            ExPolygons adjusted = offset_mm > 0 ? offset_ex(state_masks, -delta_scaled) : offset_ex(state_masks, delta_scaled);
            if (!adjusted.empty() && adjusted.size() > 1)
                adjusted = union_ex(adjusted);

            if (offset_mm < 0 && !adjusted.empty()) {
                ExPolygons occupied_other;
                for (size_t other_idx = 0; other_idx < num_channels; ++other_idx) {
                    if (other_idx == channel_idx)
                        continue;
                    if (!segmentation[layer_id][other_idx].empty())
                        append(occupied_other, segmentation[layer_id][other_idx]);
                }
                if (occupied_other.size() > 1)
                    occupied_other = union_ex(occupied_other);
                if (!occupied_other.empty()) {
                    ExPolygons clipped = diff_ex(adjusted, occupied_other, ApplySafetyOffset::Yes);
                    adjusted = std::move(clipped);
                    if (!adjusted.empty() && adjusted.size() > 1)
                        adjusted = union_ex(adjusted);
                }
            }

            state_masks = std::move(adjusted);
            if (state_masks.empty())
                ++emptied_states;
            if (offset_mm < 0)
                ++expanded_states;
            else
                ++contracted_states;
            ++changed_states;
            layer_changed = true;
        }

        if (layer_changed)
            ++changed_layers;
    }

    if (changed_states == 0)
        return false;

    BOOST_LOG_TRIVIAL(warning) << "Mixed component surface offsets applied"
                               << " object=" << (print_object.model_object() ? print_object.model_object()->name : std::string("<unknown>"))
                               << " changed_layers=" << changed_layers
                               << " changed_states=" << changed_states
                               << " contracted_states=" << contracted_states
                               << " expanded_states=" << expanded_states
                               << " emptied_states=" << emptied_states;
    return true;
}

// Region-level mixed-component surface bias for objects without MM-painted segmentation.
// Operates directly on LayerRegion::slices after they have been built. Negative offsets
// expand the mixed region and steal area from non-mixed siblings; positive offsets contract.
static bool apply_mixed_region_surface_offsets(PrintObject &print_object)
{
    const Print *print = print_object.print();
    if (print == nullptr || print_object.layer_count() == 0)
        return false;

    const PrintConfig        &print_cfg = print->config();
    const DynamicPrintConfig &full_cfg  = print->full_print_config();
    if (bool_from_full_config(full_cfg, "dithering_local_z_mode", print_cfg.dithering_local_z_mode.value))
        return false;
    if (!bool_from_full_config(full_cfg, "mixed_filament_component_bias_enabled", print_cfg.mixed_filament_component_bias_enabled.value))
        return false;

    const size_t num_physical = print_cfg.filament_diameter.size();
    if (num_physical == 0)
        return false;

    const MixedFilamentManager &mixed_mgr = print->mixed_filament_manager();
    bool has_component_offsets = false;
    for (const MixedFilament &mf : mixed_mgr.mixed_filaments()) {
        if (!mf.enabled || mf.deleted)
            continue;
        if (std::abs(mf.component_a_surface_offset) > EPSILON || std::abs(mf.component_b_surface_offset) > EPSILON) {
            has_component_offsets = true;
            break;
        }
    }
    if (!has_component_offsets)
        return false;

    size_t changed_layers    = 0;
    size_t changed_regions   = 0;
    size_t contracted_regions = 0;
    size_t expanded_regions  = 0;
    size_t stolen_regions    = 0;

    struct PendingRegionOffset {
        int        region_id { -1 };
        coordf_t   offset_mm { 0.f };
        ExPolygons adjusted;
    };

    for (size_t layer_id = 0; layer_id < print_object.layer_count(); ++layer_id) {
        Layer &layer = *print_object.get_layer(int(layer_id));
        std::vector<PendingRegionOffset> pending;
        pending.reserve(size_t(layer.region_count()));

        for (int region_id = 0; region_id < layer.region_count(); ++region_id) {
            LayerRegion *layerm = layer.get_region(region_id);
            if (layerm == nullptr || layerm->slices.empty())
                continue;

            const unsigned int filament_id = unsigned(std::max(0, layerm->region().config().wall_filament.value));
            if (!mixed_mgr.is_mixed(filament_id, num_physical))
                continue;

            const coordf_t offset_mm = clamped_mixed_component_surface_offset(mixed_mgr,
                                                                              print_cfg,
                                                                              filament_id,
                                                                              num_physical,
                                                                              int(layer_id),
                                                                              float(layer.print_z),
                                                                              float(layer.height));
            if (std::abs(offset_mm) <= EPSILON)
                continue;

            const float delta_scaled = float(scale_(std::abs(double(offset_mm))));
            if (delta_scaled <= float(EPSILON))
                continue;

            const ExPolygons original = to_expolygons(layerm->slices.surfaces);
            ExPolygons adjusted = offset_ex(original, offset_mm > 0 ? -delta_scaled : delta_scaled);
            if (!adjusted.empty() && adjusted.size() > 1)
                adjusted = union_ex(adjusted);

            pending.push_back({ region_id, offset_mm, std::move(adjusted) });
        }

        if (pending.empty())
            continue;

        bool layer_changed = false;
        for (const PendingRegionOffset &entry : pending) {
            LayerRegion *layerm = layer.get_region(entry.region_id);
            if (layerm == nullptr)
                continue;

            if (entry.offset_mm < 0 && !entry.adjusted.empty()) {
                for (int other_region_id = 0; other_region_id < layer.region_count(); ++other_region_id) {
                    if (other_region_id == entry.region_id)
                        continue;

                    LayerRegion *other = layer.get_region(other_region_id);
                    if (other == nullptr || other->slices.empty())
                        continue;

                    ExPolygons stolen = intersection_ex(other->slices.surfaces, entry.adjusted);
                    if (stolen.empty())
                        continue;

                    Polygons remaining = diff(to_polygons(other->slices.surfaces), entry.adjusted);
                    other->slices.set(union_ex(remaining), stInternal);
                    ++stolen_regions;
                    layer_changed = true;
                }
            }

            layerm->slices.set(entry.adjusted, stInternal);
            ++changed_regions;
            if (entry.offset_mm > 0)
                ++contracted_regions;
            else
                ++expanded_regions;
            layer_changed = true;
        }

        if (layer_changed)
            ++changed_layers;
    }

    if (changed_regions == 0)
        return false;

    BOOST_LOG_TRIVIAL(warning) << "Mixed region surface offsets applied"
                               << " object=" << (print_object.model_object() ? print_object.model_object()->name : std::string("<unknown>"))
                               << " changed_layers=" << changed_layers
                               << " changed_regions=" << changed_regions
                               << " contracted_regions=" << contracted_regions
                               << " expanded_regions=" << expanded_regions
                               << " stolen_regions=" << stolen_regions;
    return true;
}

template<typename ThrowOnCancel>
void apply_fuzzy_skin_segmentation(PrintObject &print_object, ThrowOnCancel throw_on_cancel)
{
    // Returns fuzzy skin segmentation based on painting in the fuzzy skin painting gizmo.
    std::vector<std::vector<ExPolygons>> segmentation = fuzzy_skin_segmentation_by_painting(print_object, throw_on_cancel);
    assert(segmentation.size() == print_object.layer_count());

    struct ByRegion
    {
        ExPolygons expolygons;
        bool       needs_merge { false };
    };

    tbb::parallel_for(tbb::blocked_range<size_t>(0, segmentation.size(), std::max(segmentation.size() / 128, size_t(1))), [&print_object, &segmentation, throw_on_cancel](const tbb::blocked_range<size_t> &range) {
        const auto &layer_ranges   = print_object.shared_regions()->layer_ranges;
        auto        it_layer_range = layer_range_first(layer_ranges, print_object.get_layer(int(range.begin()))->slice_z);

        for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++layer_idx) {
            throw_on_cancel();

            Layer &layer = *print_object.get_layer(int(layer_idx));
            it_layer_range = layer_range_next(layer_ranges, it_layer_range, layer.slice_z);
            const PrintObjectRegions::LayerRangeRegions &layer_range = *it_layer_range;

            assert(segmentation[layer_idx].size() == 2);
            const ExPolygons &fuzzy_skin_segmentation      = segmentation[layer_idx][1];
            const BoundingBox fuzzy_skin_segmentation_bbox = get_extents(fuzzy_skin_segmentation);
            if (fuzzy_skin_segmentation.empty())
                continue;

            // Split LayerRegions by painted fuzzy skin regions.
            // layer_range.fuzzy_skin_painted_regions are sorted by parent PrintObject region ID.
            std::vector<ByRegion> by_region(layer.region_count());
            auto                  it_fuzzy_skin_region_begin = layer_range.fuzzy_skin_painted_regions.cbegin();
            for (int parent_layer_region_idx = 0; parent_layer_region_idx < layer.region_count(); ++parent_layer_region_idx) {
                if (it_fuzzy_skin_region_begin == layer_range.fuzzy_skin_painted_regions.cend())
                    continue;

                const LayerRegion &parent_layer_region = *layer.get_region(parent_layer_region_idx);
                const PrintRegion &parent_print_region = parent_layer_region.region();
                assert(parent_print_region.print_object_region_id() == parent_layer_region_idx);
                if (parent_layer_region.slices.empty())
                    continue;

                // Find the first FuzzySkinPaintedRegion, which overrides the parent PrintRegion.
                auto it_fuzzy_skin_region = std::find_if(it_fuzzy_skin_region_begin, layer_range.fuzzy_skin_painted_regions.cend(), [&layer_range, &parent_print_region](const auto &fuzzy_skin_region) {
                    return fuzzy_skin_region.parent_print_object_region_id(layer_range) == parent_print_region.print_object_region_id();
                });

                if (it_fuzzy_skin_region == layer_range.fuzzy_skin_painted_regions.cend())
                    continue; // This LayerRegion isn't overrides by any FuzzySkinPaintedRegion.

                assert(it_fuzzy_skin_region->parent_print_object_region(layer_range) == &parent_print_region);

                // Update the beginning FuzzySkinPaintedRegion iterator for the next iteration.
                it_fuzzy_skin_region_begin = std::next(it_fuzzy_skin_region);

                const BoundingBox parent_layer_region_bbox        = get_extents(parent_layer_region.slices.surfaces);
                Polygons          layer_region_remaining_polygons = to_polygons(parent_layer_region.slices.surfaces);
                // Don't trim by self, it is not reliable.
                if (parent_layer_region_bbox.overlap(fuzzy_skin_segmentation_bbox) && it_fuzzy_skin_region->region != &parent_print_region) {
                    // Steal from this region.
                    const int  target_region_id = it_fuzzy_skin_region->region->print_object_region_id();
                    ExPolygons stolen           = intersection_ex(parent_layer_region.slices.surfaces, fuzzy_skin_segmentation);
                    if (!stolen.empty()) {
                        ByRegion &dst = by_region[target_region_id];
                        if (dst.expolygons.empty()) {
                            dst.expolygons = std::move(stolen);
                        } else {
                            append(dst.expolygons, std::move(stolen));
                            dst.needs_merge = true;
                        }
                    }

                    // Trim slices of this LayerRegion by the fuzzy skin region.
                    layer_region_remaining_polygons = diff(layer_region_remaining_polygons, fuzzy_skin_segmentation);

                    // Filter out unprintable polygons. Detailed explanation is inside apply_mm_segmentation.
                    if (!layer_region_remaining_polygons.empty()) {
                        layer_region_remaining_polygons = opening(union_ex(layer_region_remaining_polygons), scaled<float>(5. * EPSILON), scaled<float>(5. * EPSILON));
                    }
                }

                if (!layer_region_remaining_polygons.empty()) {
                    ByRegion &dst = by_region[parent_print_region.print_object_region_id()];
                    if (dst.expolygons.empty()) {
                        dst.expolygons = union_ex(layer_region_remaining_polygons);
                    } else {
                        append(dst.expolygons, union_ex(layer_region_remaining_polygons));
                        dst.needs_merge = true;
                    }
                }
            }

            // Re-create Surfaces of LayerRegions.
            for (int region_id = 0; region_id < layer.region_count(); ++region_id) {
                ByRegion &src = by_region[region_id];
                if (src.needs_merge) {
                    // Multiple regions were merged into one.
                    src.expolygons = closing_ex(src.expolygons, scaled<float>(10. * EPSILON));
                }

                layer.get_region(region_id)->slices.set(std::move(src.expolygons), stInternal);
            }
        }
    }); // end of parallel_for
}

// 1) Decides Z positions of the layers,
// 2) Initializes layers and their regions
// 3) Slices the object meshes
// 4) Slices the modifier meshes and reclassifies the slices of the object meshes by the slices of the modifier meshes
// 5) Applies size compensation (offsets the slices in XY plane)
// 6) Replaces bad slices by the slices reconstructed from the upper/lower layer
// Resulting expolygons of layer regions are marked as Internal.
//
// this should be idempotent
void PrintObject::slice_volumes()
{
    BOOST_LOG_TRIVIAL(info) << "Slicing volumes..." << log_memory_info();
    const Print *print                      = this->print();
    const auto   throw_on_cancel_callback   = std::function<void()>([print](){ print->throw_if_canceled(); });

    // Clear old LayerRegions, allocate for new PrintRegions.
    for (Layer* layer : m_layers) {
        //BBS: should delete all LayerRegionPtr to avoid memory leak
        while (!layer->m_regions.empty()) {
            if (layer->m_regions.back())
                delete layer->m_regions.back();
            layer->m_regions.pop_back();
        }
        layer->m_regions.reserve(m_shared_regions->all_regions.size());
        for (const std::unique_ptr<PrintRegion> &pr : m_shared_regions->all_regions)
            layer->m_regions.emplace_back(new LayerRegion(layer, pr.get()));
    }

    std::vector<float>                   slice_zs      = zs_from_layers(m_layers);
    std::vector<VolumeSlices> objSliceByVolume;
    if (!slice_zs.empty()) {
        objSliceByVolume = slice_volumes_inner(
            print->config(), this->config(), this->trafo_centered(),
            this->model_object()->volumes, m_shared_regions->layer_ranges, slice_zs, throw_on_cancel_callback);
    }

    //BBS: "model_part" volumes are grouded according to their connections
    //const auto           scaled_resolution = scaled<double>(print->config().resolution.value);
    //firstLayerObjSliceByVolume = findPartVolumes(objSliceByVolume, this->model_object()->volumes);
    //groupingVolumes(objSliceByVolumeParts, firstLayerObjSliceByGroups, scaled_resolution);
    //applyNegtiveVolumes(this->model_object()->volumes, objSliceByVolume, firstLayerObjSliceByGroups, scaled_resolution);
    firstLayerObjSliceByVolume = objSliceByVolume;

    std::vector<std::vector<ExPolygons>> region_slices =
        slices_to_regions(print->config(), *this, this->model_object()->volumes, *m_shared_regions, slice_zs,
                          std::move(objSliceByVolume), PrintObject::clip_multipart_objects, throw_on_cancel_callback);

    for (size_t region_id = 0; region_id < region_slices.size(); ++ region_id) {
        std::vector<ExPolygons> &by_layer = region_slices[region_id];
        for (size_t layer_id = 0; layer_id < by_layer.size(); ++ layer_id)
            m_layers[layer_id]->regions()[region_id]->slices.append(std::move(by_layer[layer_id]), stInternal);
    }
    region_slices.clear();

    BOOST_LOG_TRIVIAL(debug) << "Slicing volumes - removing top empty layers";
    while (! m_layers.empty()) {
        const Layer *layer = m_layers.back();
        if (! layer->empty())
            break;
        delete layer;
        m_layers.pop_back();
    }
    if (! m_layers.empty())
        m_layers.back()->upper_layer = nullptr;
    m_print->throw_if_canceled();

    this->apply_conical_overhang();

    // Capture once: needed both inside the MMU block and in the wall-only Local-Z
    // fallback after fuzzy-skin segmentation, so the dithering_local_z_whole_objects
    // path can build a Local-Z plan even when no MM painting is present.
    const PrintConfig        &print_cfg = m_print->config();
    const DynamicPrintConfig &full_cfg  = m_print->full_print_config();
    const bool local_z_whole_objects_enabled =
        bool_from_full_config(full_cfg, "dithering_local_z_whole_objects",
                              print_cfg.dithering_local_z_whole_objects.value);

    // Is any ModelVolume multi-material painted?
    if (const auto& volumes = this->model_object()->volumes;
        m_print->config().filament_diameter.size() > 1 && // BBS
        std::find_if(volumes.begin(), volumes.end(), [](const ModelVolume* v) { return !v->mmu_segmentation_facets.empty(); }) != volumes.end()) {

        // If XY Size compensation is also enabled, notify the user that XY Size compensation
        // would not be used because the object is multi-material painted.
        if (m_config.xy_hole_compensation.value != 0.f || m_config.xy_contour_compensation.value != 0.f) {
            this->active_step_add_warning(
                PrintStateBase::WarningLevel::CRITICAL,
                L("An object's XY size compensation will not be used because it is also color-painted.\nXY Size "
                  "compensation cannot be combined with color-painting."));
            BOOST_LOG_TRIVIAL(info) << "xy compensation will not work for object " << this->model_object()->name << " for multi filament.";
        }

        BOOST_LOG_TRIVIAL(debug) << "Slicing volumes - MMU segmentation";
        std::vector<std::vector<ExPolygons>> mm_segmentation = multi_material_segmentation_by_painting(*this, [print]() { print->throw_if_canceled(); });
        apply_mixed_surface_indentation(*this, mm_segmentation);
        apply_mixed_component_surface_offsets(*this, mm_segmentation);
        // Same-layer pointillisme is applied in G-code path domain (segment-level assignment),
        // not by XY state mask splitting, to avoid boolean-induced voids.
        BOOST_LOG_TRIVIAL(info) << "Same-layer pointillisme uses path-domain G-code segmentation";
        std::vector<std::vector<ExPolygons>> local_z_segmentation =
            local_z_whole_objects_enabled
                ? local_z_planner_segmentation_with_whole_object_mixed_wall(*this, mm_segmentation)
                : mm_segmentation;
        build_local_z_plan(*this, local_z_segmentation, [print]() { print->throw_if_canceled(); });
        apply_mm_segmentation(*this, std::move(mm_segmentation), [print]() { print->throw_if_canceled(); });
    }

    apply_mixed_region_surface_offsets(*this);

    if (local_z_whole_objects_enabled && this->local_z_intervals().empty()) {
        std::vector<std::vector<ExPolygons>> whole_object_local_z_segmentation =
            whole_object_local_z_segmentation_by_mixed_wall(*this);
        if (!whole_object_local_z_segmentation.empty())
            build_local_z_plan(*this, whole_object_local_z_segmentation, [print]() { print->throw_if_canceled(); });
    }

    // Is any ModelVolume fuzzy skin painted?
    if (this->model_object()->is_fuzzy_skin_painted()) {
        // If XY Size compensation is also enabled, notify the user that XY Size compensation
        // would not be used because the object has custom fuzzy skin painted.
        if (m_config.xy_hole_compensation.value != 0.f || m_config.xy_contour_compensation.value != 0.f) {
            this->active_step_add_warning(
                PrintStateBase::WarningLevel::CRITICAL,
                _u8L("An object has enabled XY Size compensation which will not be used because it is also fuzzy skin painted.\nXY Size "
                     "compensation cannot be combined with fuzzy skin painting.") +
                    "\n" + (_u8L("Object name")) + ": " + this->model_object()->name);
        }

        BOOST_LOG_TRIVIAL(debug) << "Slicing volumes - Fuzzy skin segmentation";
        apply_fuzzy_skin_segmentation(*this, [print]() { print->throw_if_canceled(); });
    }

    apply_surface_emboss_mixed_region_override(*this, [print]() { print->throw_if_canceled(); });

    InterlockingGenerator::generate_interlocking_structure(this, [print]() { print->throw_if_canceled(); });
    m_print->throw_if_canceled();

    BOOST_LOG_TRIVIAL(debug) << "Slicing volumes - make_slices in parallel - begin";
    {
        // Compensation value, scaled. Only applying the negative scaling here, as the positive scaling has already been applied during slicing.
        const size_t num_extruders = print->config().filament_diameter.size();
        const auto   xy_hole_scaled = (num_extruders > 1 && this->is_mm_painted()) ? scaled<float>(0.f) : scaled<float>(m_config.xy_hole_compensation.value);
        const auto   xy_contour_scaled            = (num_extruders > 1 && this->is_mm_painted()) ? scaled<float>(0.f) : scaled<float>(m_config.xy_contour_compensation.value);
        const float  elephant_foot_compensation_scaled = (m_config.raft_layers == 0) ?
        	// Only enable Elephant foot compensation if printing directly on the print bed.
            float(scale_(m_config.elefant_foot_compensation.value)) :
        	0.f;
        // Uncompensated slices for the layers in case the Elephant foot compensation is applied.
        std::vector<ExPolygons> lslices_elfoot_uncompensated;
        lslices_elfoot_uncompensated.resize(elephant_foot_compensation_scaled > 0 ? std::min(m_config.elefant_foot_compensation_layers.value, (int)m_layers.size()) : 0);
        //BBS: this part has been changed a lot to support seperated contour and hole size compensation
	    tbb::parallel_for(
	        tbb::blocked_range<size_t>(0, m_layers.size()),
			[this, xy_hole_scaled, xy_contour_scaled, elephant_foot_compensation_scaled, &lslices_elfoot_uncompensated](const tbb::blocked_range<size_t>& range) {
	            for (size_t layer_id = range.begin(); layer_id < range.end(); ++ layer_id) {
	                m_print->throw_if_canceled();
	                Layer *layer = m_layers[layer_id];
	                // Apply size compensation and perform clipping of multi-part objects.
	                float elfoot = elephant_foot_compensation_scaled > 0 && layer_id < m_config.elefant_foot_compensation_layers.value ? 
                        elephant_foot_compensation_scaled - (elephant_foot_compensation_scaled / m_config.elefant_foot_compensation_layers.value) * layer_id : 
                        0.f;
	                if (layer->m_regions.size() == 1) {
	                    // Optimized version for a single region layer.
	                    // Single region, growing or shrinking.
	                    LayerRegion *layerm = layer->m_regions.front();
                        if (elfoot > 0) {
		                    // Apply the elephant foot compensation and store the original layer slices without the Elephant foot compensation applied.
                            ExPolygons expolygons_to_compensate = to_expolygons(std::move(layerm->slices.surfaces));
                            if (xy_contour_scaled > 0 || xy_hole_scaled > 0) {
                                expolygons_to_compensate = _shrink_contour_holes(std::max(0.f, xy_contour_scaled),
                                                                   std::max(0.f, xy_hole_scaled),
                                                                   expolygons_to_compensate);
                            }
                            if (xy_contour_scaled < 0 || xy_hole_scaled < 0) {
                                expolygons_to_compensate = _shrink_contour_holes(std::min(0.f, xy_contour_scaled),
                                                                   std::min(0.f, xy_hole_scaled),
                                                                   expolygons_to_compensate);
                            }
                            lslices_elfoot_uncompensated[layer_id] = expolygons_to_compensate;
							layerm->slices.set(
								union_ex(
									Slic3r::elephant_foot_compensation(expolygons_to_compensate,
	                            		layerm->flow(frExternalPerimeter), unscale<double>(elfoot))),
								stInternal);
	                    } else {
	                        // Apply the XY contour and hole size compensation.
                            if (xy_contour_scaled != 0.0f || xy_hole_scaled != 0.0f) {
                                ExPolygons expolygons = to_expolygons(std::move(layerm->slices.surfaces));
                                if (xy_contour_scaled > 0 || xy_hole_scaled > 0) {
                                    expolygons = _shrink_contour_holes(std::max(0.f, xy_contour_scaled),
                                                                       std::max(0.f, xy_hole_scaled),
                                                                       expolygons);
                                }
                                if (xy_contour_scaled < 0 || xy_hole_scaled < 0) {
                                    expolygons = _shrink_contour_holes(std::min(0.f, xy_contour_scaled),
                                                                       std::min(0.f, xy_hole_scaled),
                                                                       expolygons);
                                }
                                layerm->slices.set(std::move(expolygons), stInternal);
                            }
	                    }
	                } else {
                        float max_growth = std::max(xy_hole_scaled, xy_contour_scaled);
                        float min_growth = std::min(xy_hole_scaled, xy_contour_scaled);
                        ExPolygons merged_poly_for_holes_growing;
                        if (max_growth > 0) {
                            //BBS: merge polygons because region can cut "holes".
                            //Then, cut them to give them again later to their region
                            merged_poly_for_holes_growing = layer->merged(float(SCALED_EPSILON));
                            merged_poly_for_holes_growing = _shrink_contour_holes(std::max(0.f, xy_contour_scaled),
                                                                                  std::max(0.f, xy_hole_scaled),
                                                                                  union_ex(merged_poly_for_holes_growing));

                            // BBS: clipping regions, priority is given to the first regions.
                            Polygons processed;
                            for (size_t region_id = 0; region_id < layer->regions().size(); ++region_id) {
                                ExPolygons slices = to_expolygons(std::move(layer->m_regions[region_id]->slices.surfaces));
                                if (max_growth > 0.f) {
                                    slices = intersection_ex(offset_ex(slices, max_growth), merged_poly_for_holes_growing);
                                }

                                //BBS: Trim by the slices of already processed regions.
                                if (region_id > 0)
                                    slices = diff_ex(to_polygons(std::move(slices)), processed);
                                if (region_id + 1 < layer->regions().size())
                                    // Collect the already processed regions to trim the to be processed regions.
                                    polygons_append(processed, slices);
                                layer->m_regions[region_id]->slices.set(std::move(slices), stInternal);
                            }
                        }
                        if (min_growth < 0.f || elfoot > 0.f) {
                            // Apply the negative XY compensation. (the ones that is <0)
                            ExPolygons trimming;
                            static const float eps = float(scale_(m_config.slice_closing_radius.value) * 1.5);
                            if (elfoot > 0.f) {
                                ExPolygons expolygons_to_compensate = offset_ex(layer->merged(eps), -eps);
                                lslices_elfoot_uncompensated[layer_id] = expolygons_to_compensate;
                                trimming = Slic3r::elephant_foot_compensation(expolygons_to_compensate,
                                    layer->m_regions.front()->flow(frExternalPerimeter), unscale<double>(elfoot));
                            } else {
                                trimming = layer->merged(float(SCALED_EPSILON));
                            }
                            if (min_growth < 0.0f)
                                trimming = _shrink_contour_holes(std::min(0.f, xy_contour_scaled),
                                                                 std::min(0.f, xy_hole_scaled),
                                                                 trimming);
                            //BBS: trim surfaces
                            for (size_t region_id = 0; region_id < layer->regions().size(); ++region_id) {
                                // BBS: split trimming result by region
                                ExPolygons contour_exp = to_expolygons(std::move(layer->regions()[region_id]->slices.surfaces));

                                layer->regions()[region_id]->slices.set(intersection_ex(contour_exp, to_polygons(trimming)), stInternal);
                            }
                        }
	                }
	                // Merge all regions' slices to get islands, chain them by a shortest path.
	                layer->make_slices();
	            }
	        });
	    if (elephant_foot_compensation_scaled > 0.f && ! m_layers.empty()) {
	    	// The Elephant foot has been compensated, therefore the elefant_foot_compensation_layers layer's lslices are shrank with the Elephant foot compensation value.
	    	// Store the uncompensated value there.
	    	assert(m_layers.front()->id() == 0);
            //BBS: sort the lslices_elfoot_uncompensated according to shortest path before saving
            //Otherwise the travel of the layer layer would be mess.
            for (int i = 0; i < lslices_elfoot_uncompensated.size(); i++) {
                ExPolygons &expolygons_uncompensated = lslices_elfoot_uncompensated[i];
                Points ordering_points;
                ordering_points.reserve(expolygons_uncompensated.size());
                for (const ExPolygon &ex : expolygons_uncompensated)
                    ordering_points.push_back(ex.contour.first_point());
                std::vector<Points::size_type> order = chain_points(ordering_points);
                ExPolygons lslices_sorted;
                lslices_sorted.reserve(expolygons_uncompensated.size());
                for (size_t i : order)
                    lslices_sorted.emplace_back(std::move(expolygons_uncompensated[i]));
                m_layers[i]->lslices = std::move(lslices_sorted);
            }
		}
	}

    m_print->throw_if_canceled();
    BOOST_LOG_TRIVIAL(debug) << "Slicing volumes - make_slices in parallel - end";
}

void PrintObject::apply_conical_overhang() {
    BOOST_LOG_TRIVIAL(info) << "Make overhang printable...";

    if (m_layers.empty()) {
        return;
    }
    
    const double conical_overhang_angle = this->config().make_overhang_printable_angle;
    if (conical_overhang_angle == 90.0) {
        return;
    }
    const double angle_radians = conical_overhang_angle * M_PI / 180.;
    const double max_hole_area = this->config().make_overhang_printable_hole_size; // in MM^2
    const double tan_angle = tan(angle_radians); // the XY-component of the angle
    BOOST_LOG_TRIVIAL(info) << "angle " << angle_radians << " maxHoleArea " << max_hole_area << " tan_angle "
                            << tan_angle;
    const coordf_t layer_thickness = m_config.layer_height.value;
    const coordf_t max_dist_from_lower_layer = tan_angle * layer_thickness; // max dist which can be bridged, in MM
    BOOST_LOG_TRIVIAL(info) << "layer_thickness " << layer_thickness << " max_dist_from_lower_layer "
                            << max_dist_from_lower_layer;

    // Pre-scale config
    const coordf_t scaled_max_dist_from_lower_layer = -float(scale_(max_dist_from_lower_layer));
    const coordf_t scaled_max_hole_area = float(scale_(scale_(max_hole_area)));


    for (auto i = m_layers.rbegin() + 1; i != m_layers.rend(); ++i) {
        m_print->throw_if_canceled();
        Layer *layer = *i;
        Layer *upper_layer = layer->upper_layer;

        if (upper_layer->empty()) {
          continue;
        }

        // Skip if entire layer has this disabled
        if (std::all_of(layer->m_regions.begin(), layer->m_regions.end(),
                        [](const LayerRegion *r) { return  r->slices.empty() || !r->region().config().make_overhang_printable; })) {
            continue;
        }

        //layer->export_region_slices_to_svg_debug("layer_before_conical_overhang");
        //upper_layer->export_region_slices_to_svg_debug("upper_layer_before_conical_overhang");


        // Merge the upper layer because we want to offset the entire layer uniformly, otherwise
        // the model could break at the region boundary.
        auto upper_poly = upper_layer->merged(float(SCALED_EPSILON));
        upper_poly = union_ex(upper_poly);

        // Merge layer for the same reason
        auto current_poly = layer->merged(float(SCALED_EPSILON));
        current_poly = union_ex(current_poly);

        // Avoid closing up of recessed holes in the base of a model.
        // Detects when a hole is completely covered by the layer above and removes the hole from the layer above before
        // adding it in.
        // This should have no effect any time a hole in a layer interacts with any polygon in the layer above
        if (scaled_max_hole_area > 0.0) {

            // Now go through all the holes in the current layer and check if they intersect anything in the layer above
            // If not, then they're the top of a hole and should be cut from the layer above before the union
            for (auto layer_polygon : current_poly) {
                for (auto hole : layer_polygon.holes) {
                    if (std::abs(hole.area()) < scaled_max_hole_area) {
                        ExPolygon hole_poly(hole);
                        auto hole_with_above = intersection_ex(upper_poly, hole_poly);
                        if (!hole_with_above.empty()) {
                            // The hole had some intersection with the above layer, check if it's a complete overlap
                            auto hole_difference = xor_ex(hole_with_above, hole_poly);
                            if (hole_difference.empty()) {
                                // The layer above completely cover it, remove it from the layer above
                                upper_poly = diff_ex(upper_poly, hole_poly);
                            }
                        }
                    }
                }
            }
        }

        // Now offset the upper layer to be added into current layer
        upper_poly = offset_ex(upper_poly, scaled_max_dist_from_lower_layer);

        for (size_t region_id = 0; region_id < this->num_printing_regions(); ++region_id) {
            // export_to_svg(debug_out_path("Surface-obj-%d-layer-%d-region-%d.svg", id().id, layer->id(), region_id).c_str(),
            //               layer->m_regions[region_id]->slices.surfaces);

            // Disable on given region
            if (!upper_layer->m_regions[region_id]->region().config().make_overhang_printable) {
                continue;
            }

            // Calculate the scaled upper poly that belongs to current region
            auto p = union_ex(intersection_ex(upper_layer->m_regions[region_id]->slices.surfaces, upper_poly));

            // Remove all islands that have already been fully covered by current layer
            p.erase(std::remove_if(p.begin(), p.end(), [&current_poly](const ExPolygon& ex) {
                return diff_ex(ex, current_poly).empty();
            }), p.end());

            // And now union it with current region
            ExPolygons layer_polygons = to_expolygons(layer->m_regions[region_id]->slices.surfaces);
            layer->m_regions[region_id]->slices.set(union_ex(layer_polygons, p), stInternal);

            // Then remove it from all other regions, to avoid overlapping regions
            for (size_t other_region = 0; other_region < this->num_printing_regions(); ++other_region) {
                if (other_region == region_id) {
                    continue;
                }
                ExPolygons s = to_expolygons(layer->m_regions[other_region]->slices.surfaces);
                layer->m_regions[other_region]->slices.set(diff_ex(s, p, ApplySafetyOffset::Yes), stInternal);
            }
        }
        //layer->export_region_slices_to_svg_debug("layer_after_conical_overhang");
    }
}

//BBS: this function is used to offset contour and holes of expolygons seperately by different value
ExPolygons PrintObject::_shrink_contour_holes(double contour_delta, double hole_delta, const ExPolygons& polys) const
{
    ExPolygons new_ex_polys;
    for (const ExPolygon& ex_poly : polys) {
        Polygons contours;
        Polygons holes;
        //BBS: modify hole
        for (const Polygon& hole : ex_poly.holes) {
            if (hole_delta != 0) {
                for (Polygon& newHole : offset(hole, -hole_delta)) {
                    newHole.make_counter_clockwise();
                    holes.emplace_back(std::move(newHole));
                }
            } else {
                holes.push_back(hole);
                holes.back().make_counter_clockwise();
            }
        }
        //BBS: modify contour
        if (contour_delta != 0) {
            Polygons new_contours = offset(ex_poly.contour, contour_delta);
            if (new_contours.size() == 0)
                continue;
            contours.insert(contours.end(), std::make_move_iterator(new_contours.begin()), std::make_move_iterator(new_contours.end()));
        } else {
            contours.push_back(ex_poly.contour);
        }
        ExPolygons temp = diff_ex(union_(contours), union_(holes));
        new_ex_polys.insert(new_ex_polys.end(), std::make_move_iterator(temp.begin()), std::make_move_iterator(temp.end()));
    }
    return union_ex(new_ex_polys);
}

std::vector<Polygons> PrintObject::slice_support_volumes(const ModelVolumeType model_volume_type) const
{
    auto it_volume     = this->model_object()->volumes.begin();
    auto it_volume_end = this->model_object()->volumes.end();
    for (; it_volume != it_volume_end && (*it_volume)->type() != model_volume_type; ++ it_volume) ;
    std::vector<Polygons> slices;
    if (it_volume != it_volume_end) {
        // Found at least a single support volume of model_volume_type.
        std::vector<float> zs = zs_from_layers(this->layers());
        std::vector<char>  merge_layers;
        bool               merge = false;
        const Print       *print = this->print();
        auto               throw_on_cancel_callback = std::function<void()>([print](){ print->throw_if_canceled(); });
        MeshSlicingParamsEx params;
        params.trafo = this->trafo_centered();
        for (; it_volume != it_volume_end; ++ it_volume)
            if ((*it_volume)->type() == model_volume_type) {
                std::vector<ExPolygons> slices2 = slice_volume(*(*it_volume), zs, params, throw_on_cancel_callback);
                if (slices.empty()) {
                    slices.reserve(slices2.size());
                    for (ExPolygons &src : slices2)
                        slices.emplace_back(to_polygons(std::move(src)));
                } else if (!slices2.empty()) {
                    if (merge_layers.empty())
                        merge_layers.assign(zs.size(), false);
                    for (size_t i = 0; i < zs.size(); ++ i) {
                        if (slices[i].empty())
                            slices[i] = to_polygons(std::move(slices2[i]));
                        else if (! slices2[i].empty()) {
                            append(slices[i], to_polygons(std::move(slices2[i])));
                            merge_layers[i] = true;
                            merge = true;
                        }
                    }
                }
            }
        if (merge) {
            std::vector<Polygons*> to_merge;
            to_merge.reserve(zs.size());
            for (size_t i = 0; i < zs.size(); ++ i)
                if (merge_layers[i])
                    to_merge.emplace_back(&slices[i]);
            tbb::parallel_for(
                tbb::blocked_range<size_t>(0, to_merge.size()),
                [&to_merge](const tbb::blocked_range<size_t> &range) {
                    for (size_t i = range.begin(); i < range.end(); ++ i)
                        *to_merge[i] = union_(*to_merge[i]);
            });
        }
    }
    return slices;
}

// ============================================================
// Local-Z plan generator — pair cadence path (Task 29)
// ============================================================

// Channel 0 in MM segmentation is the background/default region.
// Channels 1..N correspond to 1-based filament IDs.
static inline unsigned int segmentation_channel_filament_id(size_t channel_idx)
{
    return unsigned(channel_idx);
}

// Average nozzle width across a mixed filament's referenced components. Used as
// the reference width when clamping mixed-component surface offsets.
static float mixed_filament_reference_nozzle_mm(const MixedFilament &mixed_row, const ConfigOptionFloats &nozzle_diameters)
{
    std::vector<float> samples;
    samples.reserve(2);

    auto append_if_valid = [&samples, &nozzle_diameters](unsigned int component_id) {
        if (component_id >= 1 && component_id <= nozzle_diameters.size())
            samples.emplace_back(std::max(0.05f, float(nozzle_diameters.get_at(component_id - 1))));
    };

    append_if_valid(mixed_row.component_a);
    append_if_valid(mixed_row.component_b);

    if (samples.empty())
        return 0.4f;
    return std::accumulate(samples.begin(), samples.end(), 0.0f) / float(samples.size());
}

// Resolve the per-layer mixed-component surface offset for `filament_id` and
// clamp it to MixedFilamentManager::max_component_surface_offset_mm() based on
// the average nozzle width of the mixed row's referenced components.
static coordf_t clamped_mixed_component_surface_offset(const MixedFilamentManager &mixed_mgr,
                                                       const PrintConfig          &print_cfg,
                                                       unsigned int                filament_id,
                                                       size_t                      num_physical,
                                                       int                         layer_index,
                                                       float                       layer_print_z,
                                                       float                       layer_height,
                                                       bool                        force_height_weighted)
{
    const MixedFilament *mixed_row = mixed_mgr.mixed_filament_from_id(filament_id, num_physical);
    if (mixed_row == nullptr)
        return 0.f;

    const coordf_t offset_mm = coordf_t(mixed_mgr.component_surface_offset(
        filament_id,
        num_physical,
        layer_index,
        layer_print_z,
        layer_height,
        force_height_weighted));
    if (std::abs(offset_mm) <= EPSILON)
        return 0.f;

    const float reference_nozzle = mixed_filament_reference_nozzle_mm(*mixed_row, print_cfg.nozzle_diameter);
    const coordf_t limit_mm = coordf_t(MixedFilamentManager::max_component_surface_offset_mm(reference_nozzle));
    return std::clamp(offset_mm, -limit_mm, limit_mm);
}

// --- pass-height helpers ---

static bool fit_pass_heights_to_interval(std::vector<double> &passes, double base_height, double lo, double hi)
{
    if (passes.empty() || base_height <= EPSILON)
        return false;

    double sum = std::accumulate(passes.begin(), passes.end(), 0.0);
    double delta = base_height - sum;

    auto within = [lo, hi](double h) { return h >= lo - EPSILON && h <= hi + EPSILON; };
    if (std::abs(delta) > EPSILON) {
        if (within(passes.back() + delta)) {
            passes.back() += delta;
            delta = 0.0;
        } else if (delta > 0.0) {
            for (size_t i = passes.size(); i > 0 && delta > EPSILON; --i) {
                double &h = passes[i - 1];
                const double room = hi - h;
                if (room <= EPSILON)
                    continue;
                const double take = std::min(room, delta);
                h += take;
                delta -= take;
            }
        } else {
            for (size_t i = passes.size(); i > 0 && delta < -EPSILON; --i) {
                double &h = passes[i - 1];
                const double room = h - lo;
                if (room <= EPSILON)
                    continue;
                const double take = std::min(room, -delta);
                h -= take;
                delta += take;
            }
        }
    }

    if (std::abs(delta) > 1e-6)
        return false;
    return std::all_of(passes.begin(), passes.end(), within);
}

static bool sanitize_local_z_pass_heights(std::vector<double> &passes, double base_height, double lower_bound, double upper_bound)
{
    if (passes.empty() || base_height <= EPSILON)
        return false;

    const double lo = std::max<double>(0.01, lower_bound);
    const double hi = std::max<double>(lo, upper_bound);
    for (double &h : passes) {
        if (!std::isfinite(h))
            h = lo;
        h = std::clamp(h, lo, hi);
    }
    return fit_pass_heights_to_interval(passes, base_height, lo, hi);
}

static std::vector<double> build_uniform_local_z_pass_heights(double base_height,
                                                              double lo,
                                                              double hi,
                                                              size_t max_passes_limit = 0)
{
    std::vector<double> out;
    if (base_height <= EPSILON)
        return out;

    size_t min_passes = size_t(std::max<double>(1.0, std::ceil((base_height - EPSILON) / hi)));
    size_t max_passes = size_t(std::max<double>(1.0, std::floor((base_height + EPSILON) / lo)));
    size_t pass_count = min_passes;

    if (max_passes >= min_passes) {
        const double target_step = 0.5 * (lo + hi);
        const size_t target_passes =
            size_t(std::max<double>(1.0, std::llround(base_height / std::max<double>(target_step, EPSILON))));
        pass_count = std::clamp(target_passes, min_passes, max_passes);
    }

    if (max_passes_limit > 0) {
        const size_t capped_limit = std::max<size_t>(1, max_passes_limit);
        if (pass_count > capped_limit)
            pass_count = capped_limit;
    }

    if (pass_count == 1 && base_height >= 2.0 * lo - EPSILON && max_passes >= 2)
        pass_count = 2;

    if (pass_count <= 1) {
        out.emplace_back(base_height);
        return out;
    }

    const double uniform_height = base_height / double(pass_count);
    out.assign(pass_count, uniform_height);

    double accumulated = 0.0;
    for (size_t i = 0; i + 1 < out.size(); ++i)
        accumulated += out[i];
    out.back() = std::max<double>(EPSILON, base_height - accumulated);
    return out;
}

static std::vector<double> build_uniform_local_z_pass_heights_exact(double base_height,
                                                                    double lower_bound,
                                                                    double upper_bound,
                                                                    size_t pass_count)
{
    if (base_height <= EPSILON || pass_count == 0)
        return {};

    const double lo = std::max<double>(0.01, lower_bound);
    const double hi = std::max<double>(lo, upper_bound);
    if (pass_count == 1)
        return { base_height };

    if (double(pass_count) * lo > base_height + EPSILON || double(pass_count) * hi < base_height - EPSILON)
        return {};

    std::vector<double> out(pass_count, base_height / double(pass_count));
    if (!fit_pass_heights_to_interval(out, base_height, lo, hi))
        return {};
    return out;
}

static inline void compute_local_z_gradient_component_heights(int mix_b_percent, double lower_bound, double upper_bound,
                                                              double &h_a, double &h_b)
{
    const int mix_b = std::clamp(mix_b_percent, 0, 100);
    const double pct_b = double(mix_b) / 100.0;
    const double pct_a = 1.0 - pct_b;
    const double lo    = std::max<double>(0.01, lower_bound);
    const double hi    = std::max<double>(lo, upper_bound);
    h_a = lo + pct_a * (hi - lo);
    h_b = lo + pct_b * (hi - lo);
}

static bool choose_local_z_start_with_component_a(const std::vector<double> &pass_heights,
                                                  double                     expected_h_a,
                                                  double                     expected_h_b,
                                                  size_t                     cadence_index)
{
    double err_ab = 0.0;
    double err_ba = 0.0;
    for (size_t pass_i = 0; pass_i < pass_heights.size(); ++pass_i) {
        const double expected_ab = (pass_i % 2) == 0 ? expected_h_a : expected_h_b;
        const double expected_ba = (pass_i % 2) == 0 ? expected_h_b : expected_h_a;
        err_ab += std::abs(pass_heights[pass_i] - expected_ab);
        err_ba += std::abs(pass_heights[pass_i] - expected_ba);
    }

    if (err_ab + 1e-6 < err_ba)
        return true;
    if (err_ba + 1e-6 < err_ab)
        return false;

    if (std::abs(expected_h_a - expected_h_b) <= 1e-6)
        return (cadence_index % 2) == 0;

    return expected_h_a >= expected_h_b;
}

static std::vector<double> build_local_z_alternating_pass_heights(double base_height,
                                                                   double lower_bound,
                                                                   double upper_bound,
                                                                   double gradient_h_a,
                                                                   double gradient_h_b,
                                                                   size_t max_passes_limit = 0)
{
    if (base_height <= EPSILON)
        return {};

    const double lo = std::max<double>(0.01, lower_bound);
    const double hi = std::max<double>(lo, upper_bound);
    if (base_height < 2.0 * lo - EPSILON)
        return { base_height };

    const double cycle_h = std::max<double>(EPSILON, gradient_h_a + gradient_h_b);
    const double ratio_a = std::clamp(gradient_h_a / cycle_h, 0.0, 1.0);

    size_t min_passes = size_t(std::max<double>(2.0, std::ceil((base_height - EPSILON) / hi)));
    if ((min_passes % 2) != 0)
        ++min_passes;

    size_t max_passes = size_t(std::max<double>(2.0, std::floor((base_height + EPSILON) / lo)));
    if ((max_passes % 2) != 0)
        --max_passes;
    if (max_passes_limit > 0) {
        size_t capped_limit = std::max<size_t>(2, max_passes_limit);
        if ((capped_limit % 2) != 0)
            --capped_limit;
        if (capped_limit >= 2)
            max_passes = std::min(max_passes, capped_limit);
    }
    if (max_passes < 2)
        return build_uniform_local_z_pass_heights(base_height, lo, hi, max_passes_limit);
    if (min_passes > max_passes)
        min_passes = max_passes;
    if (min_passes < 2)
        min_passes = 2;
    if ((min_passes % 2) != 0)
        ++min_passes;
    if (min_passes > max_passes)
        return build_uniform_local_z_pass_heights(base_height, lo, hi, max_passes_limit);

    const double target_step = 0.5 * (lo + hi);
    size_t target_passes =
        size_t(std::max<double>(2.0, std::llround(base_height / std::max<double>(target_step, EPSILON))));
    if ((target_passes % 2) != 0) {
        const size_t round_up = (target_passes < max_passes) ? (target_passes + 1) : max_passes;
        const size_t round_down = (target_passes > min_passes) ? (target_passes - 1) : min_passes;
        if (round_up > max_passes)
            target_passes = round_down;
        else if (round_down < min_passes)
            target_passes = round_up;
        else {
            const size_t up_dist = round_up - target_passes;
            const size_t down_dist = target_passes - round_down;
            target_passes = (up_dist <= down_dist) ? round_up : round_down;
        }
    }
    target_passes = std::clamp(target_passes, min_passes, max_passes);

    bool                has_best           = false;
    std::vector<double> best_passes;
    double              best_ratio_error   = 0.0;
    size_t              best_pass_distance = 0;
    double              best_max_height    = 0.0;
    size_t              best_pass_count    = 0;

    for (size_t pass_count = min_passes; pass_count <= max_passes; pass_count += 2) {
        const size_t pair_count = pass_count / 2;
        if (pair_count == 0)
            continue;
        const double pair_h = base_height / double(pair_count);

        const double h_a_min = std::max(lo, pair_h - hi);
        const double h_a_max = std::min(hi, pair_h - lo);
        if (h_a_min > h_a_max + EPSILON)
            continue;

        const double h_a = std::clamp(pair_h * ratio_a, h_a_min, h_a_max);
        const double h_b = pair_h - h_a;

        std::vector<double> out;
        out.reserve(pass_count);
        for (size_t pair_idx = 0; pair_idx < pair_count; ++pair_idx) {
            out.emplace_back(h_a);
            out.emplace_back(h_b);
        }
        if (!fit_pass_heights_to_interval(out, base_height, lo, hi))
            continue;

        const double ratio_actual  = (h_a + h_b > EPSILON) ? (h_a / (h_a + h_b)) : 0.5;
        const double ratio_error   = std::abs(ratio_actual - ratio_a);
        const size_t pass_distance =
            (pass_count > target_passes) ? (pass_count - target_passes) : (target_passes - pass_count);
        const double max_height = std::max(h_a, h_b);

        const bool better_ratio      = !has_best || (ratio_error + 1e-6 < best_ratio_error);
        const bool similar_ratio     = has_best && std::abs(ratio_error - best_ratio_error) <= 1e-6;
        const bool better_distance   = similar_ratio && (pass_distance < best_pass_distance);
        const bool similar_distance  = similar_ratio && (pass_distance == best_pass_distance);
        const bool better_max_height = similar_distance && (max_height + 1e-6 < best_max_height);
        const bool similar_max_height = similar_distance && std::abs(max_height - best_max_height) <= 1e-6;
        const bool better_pass_count = similar_max_height && (pass_count > best_pass_count);

        if (better_ratio || better_distance || better_max_height || better_pass_count) {
            has_best           = true;
            best_passes        = std::move(out);
            best_ratio_error   = ratio_error;
            best_pass_distance = pass_distance;
            best_max_height    = max_height;
            best_pass_count    = pass_count;
        }
    }

    if (has_best)
        return best_passes;
    return build_uniform_local_z_pass_heights(base_height, lo, hi, max_passes_limit);
}

static std::vector<double> build_local_z_two_pass_heights(double base_height,
                                                          double lower_bound,
                                                          double upper_bound,
                                                          double gradient_h_a,
                                                          double gradient_h_b)
{
    if (base_height <= EPSILON)
        return {};

    const double lo = std::max<double>(0.01, lower_bound);
    const double hi = std::max<double>(lo, upper_bound);
    if (base_height < 2.0 * lo - EPSILON || base_height > 2.0 * hi + EPSILON)
        return { base_height };

    const double cycle_h = std::max<double>(EPSILON, gradient_h_a + gradient_h_b);
    const double ratio_a = std::clamp(gradient_h_a / cycle_h, 0.0, 1.0);

    const double h_a_min = std::max(lo, base_height - hi);
    const double h_a_max = std::min(hi, base_height - lo);
    if (h_a_min > h_a_max + EPSILON)
        return { base_height };

    const double h_a = std::clamp(base_height * ratio_a, h_a_min, h_a_max);
    const double h_b = base_height - h_a;

    std::vector<double> out { h_a, h_b };
    if (!fit_pass_heights_to_interval(out, base_height, lo, hi))
        return { base_height };
    return out;
}

static std::vector<double> build_local_z_shared_pass_heights(double base_height, double lower_bound, double upper_bound)
{
    if (base_height <= EPSILON)
        return {};

    const double lo = std::max<double>(0.01, lower_bound);
    const double hi = std::max<double>(lo, upper_bound);
    if (base_height < 2.0 * lo - EPSILON)
        return { base_height };

    double h_small = lo;
    double h_large = base_height - h_small;
    if (h_large > hi + EPSILON) {
        h_large = hi;
        h_small = base_height - h_large;
    }
    if (h_small < lo - EPSILON || h_small > hi + EPSILON ||
        h_large < lo - EPSILON || h_large > hi + EPSILON)
        return build_uniform_local_z_pass_heights(base_height, lo, hi);

    std::vector<double> out { h_small, h_large };
    if (!fit_pass_heights_to_interval(out, base_height, lo, hi))
        return build_uniform_local_z_pass_heights(base_height, lo, hi);
    if (out.size() == 2 && out[0] > out[1])
        std::swap(out[0], out[1]);
    return out;
}

static std::vector<double> build_local_z_pass_heights(double base_height,
                                                      double lower_bound,
                                                      double upper_bound,
                                                      double preferred_a,
                                                      double preferred_b,
                                                      size_t max_passes_limit = 0)
{
    if (base_height <= EPSILON)
        return {};

    const double lo = std::max<double>(0.01, lower_bound);
    const double hi = std::max<double>(lo, upper_bound);

    std::vector<double> cadence_unit;
    if (preferred_a > EPSILON)
        cadence_unit.push_back(std::clamp(preferred_a, lo, hi));
    if (preferred_b > EPSILON)
        cadence_unit.push_back(std::clamp(preferred_b, lo, hi));

    if (!cadence_unit.empty()) {
        std::vector<double> out;
        out.reserve(size_t(std::ceil(base_height / lo)) + 2);

        double z_used = 0.0;
        size_t idx = 0;
        size_t guard = 0;
        while (z_used + cadence_unit[idx] < base_height - EPSILON && guard++ < 100000) {
            out.push_back(cadence_unit[idx]);
            z_used += cadence_unit[idx];
            idx = (idx + 1) % cadence_unit.size();
        }

        const double remainder = base_height - z_used;
        if (remainder > EPSILON)
            out.push_back(remainder);

        if (fit_pass_heights_to_interval(out, base_height, lo, hi) &&
            (max_passes_limit == 0 || out.size() <= max_passes_limit))
            return out;

        if (max_passes_limit > 0 && preferred_a > EPSILON && preferred_b > EPSILON)
            return build_local_z_alternating_pass_heights(base_height,
                                                          lower_bound,
                                                          upper_bound,
                                                          preferred_a,
                                                          preferred_b,
                                                          max_passes_limit);
    }

    return build_uniform_local_z_pass_heights(base_height, lo, hi, max_passes_limit);
}

// --- gradient component ID / weight decoders ---

static std::vector<unsigned int> decode_gradient_component_ids(const MixedFilament &mf, size_t num_physical)
{
    std::vector<unsigned int> ids;
    if (mf.gradient_component_ids.empty() || num_physical == 0)
        return ids;

    bool seen[10] = { false };
    ids.reserve(mf.gradient_component_ids.size());
    for (const char c : mf.gradient_component_ids) {
        if (c < '1' || c > '9')
            continue;
        const unsigned int id = unsigned(c - '0');
        if (id == 0 || id > num_physical || seen[id])
            continue;
        seen[id] = true;
        ids.emplace_back(id);
    }
    return ids;
}

static std::vector<int> decode_gradient_component_weights(const MixedFilament &mf, size_t expected_components)
{
    std::vector<int> out;
    if (mf.gradient_component_weights.empty() || expected_components == 0)
        return out;

    std::string token;
    for (const char c : mf.gradient_component_weights) {
        if (c >= '0' && c <= '9') {
            token.push_back(c);
            continue;
        }
        if (!token.empty()) {
            out.emplace_back(std::max(0, std::atoi(token.c_str())));
            token.clear();
        }
    }
    if (!token.empty())
        out.emplace_back(std::max(0, std::atoi(token.c_str())));
    if (out.size() != expected_components)
        return {};

    int sum = 0;
    for (const int v : out)
        sum += std::max(0, v);
    if (sum <= 0)
        return {};
    return out;
}

// --- weighted sequence builder ---

static void reduce_weight_counts_to_cycle_limit(std::vector<int> &counts, size_t cycle_limit)
{
    if (counts.empty() || cycle_limit == 0)
        return;

    int total = std::accumulate(counts.begin(), counts.end(), 0);
    if (total <= 0 || size_t(total) <= cycle_limit)
        return;

    std::vector<size_t> positive_indices;
    positive_indices.reserve(counts.size());
    for (size_t i = 0; i < counts.size(); ++i)
        if (counts[i] > 0)
            positive_indices.emplace_back(i);

    if (positive_indices.empty()) {
        counts.assign(counts.size(), 0);
        return;
    }

    std::vector<int> reduced(counts.size(), 0);
    if (cycle_limit < positive_indices.size()) {
        std::sort(positive_indices.begin(), positive_indices.end(), [&counts](size_t lhs, size_t rhs) {
            if (counts[lhs] != counts[rhs])
                return counts[lhs] > counts[rhs];
            return lhs < rhs;
        });
        for (size_t i = 0; i < cycle_limit; ++i)
            reduced[positive_indices[i]] = 1;
        counts = std::move(reduced);
        return;
    }

    size_t remaining_slots = cycle_limit;
    for (const size_t idx : positive_indices) {
        reduced[idx] = 1;
        --remaining_slots;
    }

    int total_extras = 0;
    std::vector<int> extra_counts(counts.size(), 0);
    for (const size_t idx : positive_indices) {
        extra_counts[idx] = std::max(0, counts[idx] - 1);
        total_extras += extra_counts[idx];
    }
    if (remaining_slots == 0 || total_extras <= 0) {
        counts = std::move(reduced);
        return;
    }

    std::vector<double> remainders(counts.size(), -1.0);
    size_t assigned_slots = 0;
    for (const size_t idx : positive_indices) {
        if (extra_counts[idx] == 0)
            continue;
        const double exact = double(remaining_slots) * double(extra_counts[idx]) / double(total_extras);
        const int assigned = int(std::floor(exact));
        reduced[idx] += assigned;
        assigned_slots += size_t(assigned);
        remainders[idx] = exact - double(assigned);
    }

    size_t missing_slots = remaining_slots > assigned_slots ? (remaining_slots - assigned_slots) : size_t(0);
    while (missing_slots > 0) {
        size_t best_idx      = size_t(-1);
        double best_remainder = -1.0;
        int    best_extra     = -1;
        for (const size_t idx : positive_indices) {
            if (extra_counts[idx] == 0)
                continue;
            if (remainders[idx] > best_remainder ||
                (std::abs(remainders[idx] - best_remainder) <= 1e-9 && extra_counts[idx] > best_extra) ||
                (std::abs(remainders[idx] - best_remainder) <= 1e-9 && extra_counts[idx] == best_extra && idx < best_idx)) {
                best_idx       = idx;
                best_remainder = remainders[idx];
                best_extra     = extra_counts[idx];
            }
        }
        if (best_idx == size_t(-1))
            break;
        ++reduced[best_idx];
        remainders[best_idx] = -1.0;
        --missing_slots;
    }

    counts = std::move(reduced);
}

static std::vector<unsigned int> build_weighted_gradient_sequence(const std::vector<unsigned int> &ids,
                                                                  const std::vector<int>          &weights,
                                                                  size_t                           max_cycle_limit = 0)
{
    if (ids.empty())
        return {};

    std::vector<unsigned int> filtered_ids;
    std::vector<int>          counts;
    filtered_ids.reserve(ids.size());
    counts.reserve(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
        const int w = (i < weights.size()) ? std::max(0, weights[i]) : 0;
        if (w <= 0)
            continue;
        filtered_ids.emplace_back(ids[i]);
        counts.emplace_back(w);
    }
    if (filtered_ids.empty()) {
        filtered_ids = ids;
        counts.assign(ids.size(), 1);
    }

    int g = 0;
    for (const int c : counts)
        g = std::gcd(g, std::max(1, c));
    if (g > 1) {
        for (int &c : counts)
            c = std::max(1, c / g);
    }

    constexpr size_t k_max_cycle = 48;
    const size_t effective_cycle_limit =
        max_cycle_limit > 0 ? std::min(k_max_cycle, std::max<size_t>(1, max_cycle_limit)) : k_max_cycle;
    reduce_weight_counts_to_cycle_limit(counts, effective_cycle_limit);

    std::vector<unsigned int> reduced_ids;
    std::vector<int>          reduced_counts;
    reduced_ids.reserve(filtered_ids.size());
    reduced_counts.reserve(counts.size());
    for (size_t i = 0; i < counts.size(); ++i) {
        if (counts[i] <= 0)
            continue;
        reduced_ids.emplace_back(filtered_ids[i]);
        reduced_counts.emplace_back(counts[i]);
    }
    if (reduced_ids.empty())
        return {};
    filtered_ids = std::move(reduced_ids);
    counts       = std::move(reduced_counts);

    const int total = std::accumulate(counts.begin(), counts.end(), 0);
    if (total <= 0)
        return {};

    const size_t cycle = size_t(total);
    std::vector<unsigned int> sequence;
    sequence.reserve(cycle);
    std::vector<int> emitted(counts.size(), 0);
    for (size_t pos = 0; pos < cycle; ++pos) {
        size_t best_idx   = 0;
        double best_score = -1e9;
        for (size_t i = 0; i < counts.size(); ++i) {
            const double target = double(pos + 1) * double(counts[i]) / double(total);
            const double score  = target - double(emitted[i]);
            if (score > best_score) {
                best_score = score;
                best_idx   = i;
            }
        }
        ++emitted[best_idx];
        sequence.emplace_back(filtered_ids[best_idx]);
    }
    return sequence;
}

// --- eligibility / pair helpers ---

static size_t unique_extruder_count(const std::vector<unsigned int> &sequence, size_t num_physical)
{
    if (sequence.empty() || num_physical == 0)
        return 0;

    std::vector<bool> seen(num_physical + 1, false);
    size_t            unique_count = 0;
    for (const unsigned int extruder_id : sequence) {
        if (extruder_id == 0 || extruder_id > num_physical)
            continue;
        if (!seen[extruder_id]) {
            seen[extruder_id] = true;
            ++unique_count;
        }
    }
    return unique_count;
}

static bool local_z_eligible_mixed_row(const MixedFilament &mf)
{
    // Local-Z flow-height modulation applies to all mixed rows resolved as A/B
    // blends on model surfaces. Exclude explicit manual patterns and same-layer
    // pointillism rows, which have their own distribution semantics.
    return mf.enabled &&
           mf.manual_pattern.empty() &&
           mf.distribution_mode != int(MixedFilament::SameLayerPointillisme);
}

// Returns false for pair-cadence rows (handled by Task 29); true for 3+ component rows (Task 31).
static bool local_z_direct_multicolor_row(const MixedFilament        &mf,
                                          size_t                      num_physical,
                                          std::vector<unsigned int>  *component_ids    = nullptr,
                                          std::vector<int>           *component_weights = nullptr)
{
    if (!local_z_eligible_mixed_row(mf))
        return false;

    const std::vector<unsigned int> ids = decode_gradient_component_ids(mf, num_physical);
    if (ids.size() < 3)
        return false;

    if (component_ids != nullptr)
        *component_ids = ids;
    if (component_weights != nullptr) {
        std::vector<int> weights = decode_gradient_component_weights(mf, ids.size());
        if (weights.empty())
            weights.assign(ids.size(), 1);
        *component_weights = std::move(weights);
    }
    return true;
}

struct LocalZActivePair
{
    unsigned int component_a             = 0;
    unsigned int component_b             = 0;
    int          mix_b_percent           = 50;
    bool         uses_layer_cycle_sequence = false;

    bool valid_pair(size_t num_physical) const
    {
        return component_a > 0 && component_a <= num_physical &&
               component_b > 0 && component_b <= num_physical &&
               component_a != component_b;
    }
};

static void append_local_z_pair_option(std::vector<LocalZActivePair> &out,
                                       unsigned int                   component_a,
                                       unsigned int                   component_b,
                                       int                            weight_a,
                                       int                            weight_b)
{
    if (component_a == 0 || component_b == 0 || component_a == component_b)
        return;

    LocalZActivePair pair;
    pair.component_a              = component_a;
    pair.component_b              = component_b;
    pair.uses_layer_cycle_sequence = true;

    const int safe_weight_a = std::max(0, weight_a);
    const int safe_weight_b = std::max(0, weight_b);
    const int pair_total    = std::max(1, safe_weight_a + safe_weight_b);
    pair.mix_b_percent =
        std::clamp(int(std::lround(100.0 * double(safe_weight_b) / double(pair_total))), 1, 99);
    out.emplace_back(pair);
}

static std::vector<LocalZActivePair> build_local_z_pair_cycle_for_row(const MixedFilament &mf, size_t num_physical)
{
    std::vector<LocalZActivePair> pair_options;
    if (!mf.enabled || num_physical == 0 || mf.distribution_mode == int(MixedFilament::Simple))
        return pair_options;

    const std::vector<unsigned int> gradient_ids = decode_gradient_component_ids(mf, num_physical);
    if (gradient_ids.size() < 3)
        return pair_options;

    std::vector<int> gradient_weights = decode_gradient_component_weights(mf, gradient_ids.size());
    if (gradient_weights.empty())
        gradient_weights.assign(gradient_ids.size(), 1);

    std::vector<int> pair_weights;
    if (gradient_ids.size() >= 4) {
        append_local_z_pair_option(pair_options, gradient_ids[0], gradient_ids[1], gradient_weights[0], gradient_weights[1]);
        append_local_z_pair_option(pair_options, gradient_ids[2], gradient_ids[3], gradient_weights[2], gradient_weights[3]);
        pair_weights.emplace_back(std::max(1, gradient_weights[0] + gradient_weights[1]));
        pair_weights.emplace_back(std::max(1, gradient_weights[2] + gradient_weights[3]));
    } else {
        append_local_z_pair_option(pair_options, gradient_ids[0], gradient_ids[1], gradient_weights[0], gradient_weights[1]);
        append_local_z_pair_option(pair_options, gradient_ids[0], gradient_ids[2], gradient_weights[0], gradient_weights[2]);
        append_local_z_pair_option(pair_options, gradient_ids[1], gradient_ids[2], gradient_weights[1], gradient_weights[2]);
        pair_weights.emplace_back(std::max(1, gradient_weights[0] + gradient_weights[1]));
        pair_weights.emplace_back(std::max(1, gradient_weights[0] + gradient_weights[2]));
        pair_weights.emplace_back(std::max(1, gradient_weights[1] + gradient_weights[2]));
    }

    if (pair_options.size() < 2 || pair_options.size() != pair_weights.size())
        return {};

    std::vector<unsigned int> pair_ids(pair_options.size(), 0);
    for (size_t idx = 0; idx < pair_ids.size(); ++idx)
        pair_ids[idx] = unsigned(idx + 1);

    const size_t max_pair_layers =
        mf.local_z_max_sublayers >= 2 ? std::max<size_t>(1, size_t(mf.local_z_max_sublayers) / 2) : size_t(0);
    const std::vector<unsigned int> pair_sequence = build_weighted_gradient_sequence(pair_ids, pair_weights, max_pair_layers);
    if (pair_sequence.empty())
        return {};

    std::vector<LocalZActivePair> out;
    out.reserve(pair_sequence.size());
    for (const unsigned int pair_token : pair_sequence) {
        if (pair_token < 1 || pair_token > pair_options.size())
            continue;
        out.emplace_back(pair_options[size_t(pair_token - 1)]);
    }
    return out;
}

// Task 31: Direct multicolor pass heights solver (3+ components).
// Allocates sub-layer pass heights proportional to component weights,
// respecting lo/hi bounds. Falls back to uniform if no valid split found.
static std::vector<double> build_local_z_direct_multicolor_pass_heights(const MixedFilament    &mf,
                                                                        const std::vector<int> &component_weights,
                                                                        double                  base_height,
                                                                        double                  lower_bound,
                                                                        double                  upper_bound,
                                                                        size_t                  component_count)
{
    if (base_height <= EPSILON || component_count == 0)
        return {};

    const double lo = std::max<double>(0.01, lower_bound);
    const double hi = std::max<double>(lo, upper_bound);
    const size_t min_passes = size_t(std::max<double>(1.0, std::ceil((base_height - EPSILON) / hi)));
    const size_t max_passes = size_t(std::max<double>(1.0, std::floor((base_height + EPSILON) / lo)));
    if (max_passes == 0)
        return { base_height };

    std::vector<int> positive_weights;
    positive_weights.reserve(component_weights.size());
    for (const int weight : component_weights)
        if (weight > 0)
            positive_weights.emplace_back(weight);
    if (positive_weights.empty())
        positive_weights.assign(component_count, 1);

    const int total_weight = std::max(1, std::accumulate(positive_weights.begin(), positive_weights.end(), 0));
    std::vector<double> component_targets;
    component_targets.reserve(positive_weights.size());
    size_t ideal_passes = 0;
    for (const int weight : positive_weights) {
        const double target = base_height * double(weight) / double(total_weight);
        component_targets.emplace_back(target);
        ideal_passes += size_t(std::max<double>(1.0, std::ceil((target - EPSILON) / hi)));
    }

    size_t pass_limit = max_passes;
    if (mf.local_z_max_sublayers >= 2)
        pass_limit = std::min(pass_limit, size_t(std::max(2, mf.local_z_max_sublayers)));
    pass_limit = std::max(pass_limit, min_passes);

    size_t desired_passes = std::clamp(std::max(component_targets.size(), ideal_passes), min_passes, pass_limit);

    std::vector<double> bins = component_targets;
    while (bins.size() > desired_passes) {
        std::sort(bins.begin(), bins.end());
        const double merged = bins[0] + bins[1];
        bins.erase(bins.begin(), bins.begin() + 2);
        bins.emplace_back(merged);
    }

    while (bins.size() < desired_passes) {
        auto it = std::max_element(bins.begin(), bins.end());
        if (it == bins.end())
            break;
        const double value = *it;
        if (value < 2.0 * lo - EPSILON)
            break;
        double first = std::clamp(value * 0.5, lo, value - lo);
        double second = value - first;
        if (first < lo - EPSILON || second < lo - EPSILON)
            break;
        *it = first;
        bins.emplace_back(second);
    }

    if (bins.empty())
        return build_uniform_local_z_pass_heights(base_height, lo, hi, desired_passes);

    std::sort(bins.begin(), bins.end(), std::greater<double>());
    for (double &value : bins)
        value = std::clamp(value, lo, hi);
    if (fit_pass_heights_to_interval(bins, base_height, lo, hi))
        return bins;

    for (size_t pass_count = desired_passes; pass_count >= min_passes; --pass_count) {
        std::vector<double> exact = build_uniform_local_z_pass_heights_exact(base_height, lower_bound, upper_bound, pass_count);
        if (!exact.empty())
            return exact;
        if (pass_count == min_passes)
            break;
    }

    return build_uniform_local_z_pass_heights(base_height, lo, hi, desired_passes);
}

// Task 31: Direct multicolor sequence solver with carry-over.
// Assigns each pass to a component proportionally using remaining-need scoring.
// carry_error_mm accumulates the rounding residual across nominal layers.
static std::vector<unsigned int> build_local_z_direct_multicolor_sequence(const std::vector<unsigned int> &component_ids,
                                                                          const std::vector<int>          &component_weights,
                                                                          const std::vector<double>       &pass_heights,
                                                                          std::vector<double>            &carry_error_mm)
{
    if (component_ids.empty() || pass_heights.empty())
        return {};

    std::vector<unsigned int> filtered_ids;
    std::vector<int>          filtered_weights;
    filtered_ids.reserve(component_ids.size());
    filtered_weights.reserve(component_ids.size());
    for (size_t idx = 0; idx < component_ids.size(); ++idx) {
        const int weight = idx < component_weights.size() ? std::max(0, component_weights[idx]) : 0;
        if (weight <= 0)
            continue;
        filtered_ids.emplace_back(component_ids[idx]);
        filtered_weights.emplace_back(weight);
    }
    if (filtered_ids.empty()) {
        filtered_ids = component_ids;
        filtered_weights.assign(component_ids.size(), 1);
    }
    if (filtered_ids.empty())
        return {};

    if (carry_error_mm.size() != filtered_ids.size())
        carry_error_mm.assign(filtered_ids.size(), 0.0);

    const double total_height = std::accumulate(pass_heights.begin(), pass_heights.end(), 0.0);
    const int total_weight = std::max(1, std::accumulate(filtered_weights.begin(), filtered_weights.end(), 0));

    std::vector<double> desired_heights(filtered_ids.size(), 0.0);
    for (size_t idx = 0; idx < filtered_ids.size(); ++idx)
        desired_heights[idx] = total_height * double(filtered_weights[idx]) / double(total_weight) + carry_error_mm[idx];

    std::vector<double>       assigned_heights(filtered_ids.size(), 0.0);
    std::vector<unsigned int> sequence;
    sequence.reserve(pass_heights.size());
    int previous_choice = -1;

    for (const double pass_height : pass_heights) {
        size_t best_idx   = 0;
        double best_score = -std::numeric_limits<double>::infinity();
        double best_need  = -std::numeric_limits<double>::infinity();
        for (size_t idx = 0; idx < filtered_ids.size(); ++idx) {
            const double remaining_need = desired_heights[idx] - assigned_heights[idx];
            double       score          = remaining_need;
            if (int(idx) == previous_choice)
                score -= 0.35 * pass_height;

            if (score > best_score + 1e-9 ||
                (std::abs(score - best_score) <= 1e-9 &&
                 (remaining_need > best_need + 1e-9 ||
                  (std::abs(remaining_need - best_need) <= 1e-9 && filtered_ids[idx] < filtered_ids[best_idx])))) {
                best_idx   = idx;
                best_score = score;
                best_need  = remaining_need;
            }
        }

        assigned_heights[best_idx] += pass_height;
        previous_choice = int(best_idx);
        sequence.emplace_back(filtered_ids[best_idx]);
    }

    for (size_t idx = 0; idx < filtered_ids.size(); ++idx)
        carry_error_mm[idx] = desired_heights[idx] - assigned_heights[idx];

    const double error_sum = std::accumulate(carry_error_mm.begin(), carry_error_mm.end(), 0.0);
    if (!carry_error_mm.empty() && std::abs(error_sum) > 1e-9) {
        const double correction = error_sum / double(carry_error_mm.size());
        for (double &value : carry_error_mm)
            value -= correction;
    }

    return sequence;
}

static LocalZActivePair derive_local_z_active_pair(const MixedFilament                &mf,
                                                   const std::vector<LocalZActivePair> &pair_cycle,
                                                   size_t                              num_physical,
                                                   int                                 cadence_index)
{
    LocalZActivePair out;

    if (!pair_cycle.empty()) {
        const int    cycle_i = int(pair_cycle.size());
        const size_t pos     = size_t(((cadence_index % cycle_i) + cycle_i) % cycle_i);
        return pair_cycle[pos];
    }

    out.component_a            = mf.component_a;
    out.component_b            = mf.component_b;
    out.mix_b_percent          = std::clamp(mf.mix_b_percent, 0, 100);
    out.uses_layer_cycle_sequence = false;
    return out;
}

static std::vector<ExPolygons> collect_local_z_fixed_state_masks_by_extruder(const std::vector<ExPolygons> &layer_segmentation,
                                                                             const size_t                   num_physical)
{
    std::vector<ExPolygons> masks_by_extruder(num_physical);
    for (size_t channel_idx = 1; channel_idx < layer_segmentation.size(); ++channel_idx) {
        const ExPolygons &state_masks = layer_segmentation[channel_idx];
        if (state_masks.empty())
            continue;

        const unsigned int state_id = segmentation_channel_filament_id(channel_idx);
        if (state_id == 0 || state_id > num_physical)
            continue;
        append(masks_by_extruder[state_id - 1], state_masks);
    }
    for (ExPolygons &masks : masks_by_extruder)
        if (masks.size() > 1)
            masks = union_ex(masks);
    return masks_by_extruder;
}

static ExPolygons collect_layer_region_slices(const Layer &layer)
{
    ExPolygons out;
    for (const LayerRegion *layerm : layer.regions())
        append(out, to_expolygons(layerm->slices.surfaces));
    if (!out.empty())
        out = union_ex(out);
    return out;
}

// ---------------------------------------------------------------------------
// surface_emboss_mixed debug subsystem
// ---------------------------------------------------------------------------
// Forward declaration for emboss_surface_mixed_shell_override_delta — defined
// below the debug subsystem alongside apply_surface_emboss_mixed_region_override.
static float emboss_surface_mixed_shell_override_delta(const LayerRegion &layerm, const ModelVolume &volume);

struct SurfaceEmbossMixedDebugCandidate
{
    const ModelVolume *volume { nullptr };
    int                region_id { -1 };
};

static bool has_surface_emboss_mixed_volume(const PrintObject &print_object)
{
    const Print *print = print_object.print();
    if (print == nullptr)
        return false;

    const size_t                num_physical = print->config().filament_diameter.size();
    const MixedFilamentManager &mixed_mgr    = print->mixed_filament_manager();
    for (const ModelVolume *volume : print_object.model_object()->volumes)
        if (volume->is_model_part() &&
            volume->emboss_shape.has_value() &&
            volume->emboss_shape->projection.use_surface &&
            mixed_mgr.is_mixed(unsigned(std::max(0, volume->extruder_id())), num_physical))
            return true;
    return false;
}

static std::string surface_emboss_mixed_debug_file_path(const PrintObject &print_object)
{
    return debug_out_path("emboss-mixed/obj-%d-debug.txt", int(print_object.id().id));
}

static void reset_surface_emboss_mixed_debug_file(const PrintObject &print_object)
{
    std::ofstream out(surface_emboss_mixed_debug_file_path(print_object), std::ios::out | std::ios::trunc);
    out << "surface emboss mixed debug"
        << " object_id=" << int(print_object.id().id)
        << " object_name=" << (print_object.model_object() ? print_object.model_object()->name : std::string("<unknown>"))
        << "\n";
}

static void append_surface_emboss_mixed_debug_line(const PrintObject &print_object, const std::string &line)
{
    std::ofstream out(surface_emboss_mixed_debug_file_path(print_object), std::ios::out | std::ios::app);
    out << line << '\n';
}

static std::vector<SurfaceEmbossMixedDebugCandidate> collect_surface_emboss_mixed_debug_candidates(
    const Layer                                  &layer,
    const PrintObjectRegions::LayerRangeRegions  &layer_range,
    const MixedFilamentManager                   &mixed_mgr,
    size_t                                        num_physical)
{
    std::vector<SurfaceEmbossMixedDebugCandidate> out;
    std::vector<int>                              processed_region_ids;
    processed_region_ids.reserve(layer_range.volume_regions.size());

    for (const PrintObjectRegions::VolumeRegion &volume_region : layer_range.volume_regions) {
        const ModelVolume *volume = volume_region.model_volume;
        if (volume == nullptr || !volume->is_model_part() || !volume->emboss_shape.has_value() || !volume->emboss_shape->projection.use_surface)
            continue;
        if (volume_region.region == nullptr)
            continue;

        const int region_id = volume_region.region->print_object_region_id();
        if (region_id < 0 || region_id >= layer.region_count())
            continue;
        if (std::find(processed_region_ids.begin(), processed_region_ids.end(), region_id) != processed_region_ids.end())
            continue;
        processed_region_ids.emplace_back(region_id);

        if (!mixed_mgr.is_mixed(unsigned(std::max(0, volume_region.region->config().wall_filament.value)), num_physical))
            continue;

        out.push_back({ volume, region_id });
    }

    return out;
}

static void export_surface_emboss_mixed_layer_svg(
    const char                                          *stage,
    const PrintObject                                   &print_object,
    size_t                                               layer_id,
    const Layer                                         &layer,
    const std::vector<SurfaceEmbossMixedDebugCandidate> &candidates,
    const ExPolygons                                    *overlay,
    const std::string                                   &overlay_legend)
{
    std::vector<std::pair<ExPolygons, SVG::ExPolygonAttributes>> items;
    items.reserve(size_t(layer.region_count()) + ((overlay != nullptr && !overlay->empty()) ? 1 : 0));

    for (int region_id = 0; region_id < layer.region_count(); ++region_id) {
        const LayerRegion *layerm = layer.get_region(region_id);
        if (layerm == nullptr || layerm->slices.empty())
            continue;

        ExPolygons expolygons = to_expolygons(layerm->slices.surfaces);
        if (expolygons.empty())
            continue;

        const bool is_candidate = std::find_if(candidates.begin(), candidates.end(), [region_id](const auto &candidate) {
            return candidate.region_id == region_id;
        }) != candidates.end();

        SVG::ExPolygonAttributes attrs(
            "region " + std::to_string(region_id) + " wall=" + std::to_string(layerm->region().config().wall_filament.value),
            is_candidate ? "#3b82f6" : "#bfc5cc",
            is_candidate ? 0.35f : 0.14f);
        attrs.outline_width = scale_(0.05f);
        attrs.color_contour = is_candidate ? "blue" : "black";
        attrs.color_holes   = attrs.color_contour;
        items.emplace_back(std::move(expolygons), std::move(attrs));
    }

    if (overlay != nullptr && !overlay->empty()) {
        SVG::ExPolygonAttributes attrs(overlay_legend, "#ef4444", 0.28f);
        attrs.outline_width = scale_(0.05f);
        attrs.color_contour = "red";
        attrs.color_holes   = "red";
        items.emplace_back(*overlay, std::move(attrs));
    }

    if (!items.empty())
        SVG::export_expolygons(debug_out_path("emboss-mixed/obj-%d-layer-%03d-%s.svg",
                                              int(print_object.id().id),
                                              int(layer_id),
                                              stage),
                               items);
}

static void dump_surface_emboss_mixed_layer_state(
    const char                                          *stage,
    const PrintObject                                   &print_object,
    size_t                                               layer_id,
    const Layer                                         &layer,
    const PrintObjectRegions::LayerRangeRegions         &layer_range,
    const std::vector<ExPolygons>                       *segmentation_layer)
{
    const Print *print = print_object.print();
    if (print == nullptr)
        return;

    const size_t                num_physical = print->config().filament_diameter.size();
    const MixedFilamentManager &mixed_mgr    = print->mixed_filament_manager();
    const std::vector<SurfaceEmbossMixedDebugCandidate> candidates =
        collect_surface_emboss_mixed_debug_candidates(layer, layer_range, mixed_mgr, num_physical);
    if (candidates.empty())
        return;

    std::ostringstream header;
    header << std::fixed << std::setprecision(4)
           << "stage=" << stage
           << " layer=" << layer_id
           << " print_z=" << layer.print_z
           << " slice_z=" << layer.slice_z
           << " regions=" << layer.region_count()
           << " candidates=" << candidates.size();
    append_surface_emboss_mixed_debug_line(print_object, header.str());

    for (int region_id = 0; region_id < layer.region_count(); ++region_id) {
        const LayerRegion *layerm = layer.get_region(region_id);
        if (layerm == nullptr)
            continue;
        const double slice_area = layerm->slices.empty() ? 0.0 : std::abs(area(to_expolygons(layerm->slices.surfaces)));
        std::ostringstream line;
        line << std::fixed << std::setprecision(4)
             << "  region=" << region_id
             << " wall=" << layerm->region().config().wall_filament.value
             << " sparse=" << layerm->region().config().sparse_infill_filament.value
             << " solid=" << layerm->region().config().solid_infill_filament.value
             << " area=" << slice_area;
        append_surface_emboss_mixed_debug_line(print_object, line.str());
    }

    for (const SurfaceEmbossMixedDebugCandidate &candidate : candidates) {
        const LayerRegion *layerm = layer.get_region(candidate.region_id);
        if (layerm == nullptr)
            continue;

        const double slice_area = layerm->slices.empty() ? 0.0 : std::abs(area(to_expolygons(layerm->slices.surfaces)));
        const float  shell_delta_scaled = emboss_surface_mixed_shell_override_delta(*layerm, *candidate.volume);
        std::ostringstream line;
        line << std::fixed << std::setprecision(4)
             << "  candidate region=" << candidate.region_id
             << " volume_name=" << candidate.volume->name
             << " volume_extruder=" << candidate.volume->extruder_id()
             << " cfg_wall=" << layerm->region().config().wall_filament.value
             << " depth=" << float(candidate.volume->emboss_shape->projection.depth)
             << " shell_delta_mm=" << unscale<double>(shell_delta_scaled)
             << " area=" << slice_area;
        append_surface_emboss_mixed_debug_line(print_object, line.str());

        if (segmentation_layer != nullptr) {
            const int cfg_wall = layerm->region().config().wall_filament.value;
            if (cfg_wall >= 1 && cfg_wall <= int(segmentation_layer->size())) {
                const double seg_area = std::abs(area((*segmentation_layer)[size_t(cfg_wall - 1)]));
                std::ostringstream seg_line;
                seg_line << std::fixed << std::setprecision(4)
                         << "    segmentation channel=" << cfg_wall
                         << " area=" << seg_area;
                append_surface_emboss_mixed_debug_line(print_object, seg_line.str());
            }
        }
    }

    export_surface_emboss_mixed_layer_svg(stage, print_object, layer_id, layer, candidates, nullptr, "");
}

static float emboss_surface_mixed_shell_override_delta(const LayerRegion &layerm, const ModelVolume &volume)
{
    if (!volume.emboss_shape.has_value() || !volume.emboss_shape->projection.use_surface)
        return 0.f;

    const float depth_mm = std::max(0.f, float(volume.emboss_shape->projection.depth));
    if (depth_mm <= EPSILON)
        return 0.f;

    const PrintRegionConfig &config = layerm.region().config();
    if (config.wall_loops.value <= 0)
        return 0.f;

    const Flow   ext_flow        = layerm.flow(frExternalPerimeter);
    const Flow   perimeter_flow  = layerm.flow(frPerimeter);
    const coord_t shell_scaled   = ext_flow.scaled_width() / 2 +
                                   ext_flow.scaled_spacing() / 2 +
                                   std::max(0, config.wall_loops.value - 1) * perimeter_flow.scaled_spacing();
    const float  shell_depth_mm  = float(unscale<double>(shell_scaled));
    const float  delta_mm        = std::max(0.f, shell_depth_mm - depth_mm);
    return delta_mm <= EPSILON ? 0.f : scaled<float>(delta_mm);
}

// For each LayerRegion that hosts a use_surface emboss volume painted with a mixed filament,
// override sibling regions' slices that intersect the emboss footprint so that the mixed-color
// surface paints on top of the neighbor instead of being trimmed away by the modifier hierarchy.
template<typename ThrowOnCancel>
static bool apply_surface_emboss_mixed_region_override(PrintObject &print_object, ThrowOnCancel throw_on_cancel)
{
    const Print *print = print_object.print();
    if (print == nullptr || print_object.layer_count() == 0 || print_object.shared_regions() == nullptr)
        return false;

    const size_t                 num_physical = print->config().filament_diameter.size();
    const MixedFilamentManager  &mixed_mgr    = print->mixed_filament_manager();
    const auto                  &volumes      = print_object.model_object()->volumes;
    if (num_physical == 0 ||
        std::find_if(volumes.begin(), volumes.end(), [&mixed_mgr, num_physical](const ModelVolume *volume) {
            return volume->is_model_part() &&
                   volume->emboss_shape.has_value() &&
                   volume->emboss_shape->projection.use_surface &&
                   mixed_mgr.is_mixed(unsigned(std::max(0, volume->extruder_id())), num_physical);
        }) == volumes.end())
        return false;

    const auto &layer_ranges = print_object.shared_regions()->layer_ranges;
    auto        it_layer_range = layer_range_first(layer_ranges, print_object.get_layer(0)->slice_z);

    size_t changed_layers  = 0;
    size_t changed_regions = 0;
    size_t stolen_regions  = 0;

    for (size_t layer_id = 0; layer_id < print_object.layer_count(); ++layer_id) {
        throw_on_cancel();

        Layer &layer = *print_object.get_layer(int(layer_id));
        it_layer_range = layer_range_next(layer_ranges, it_layer_range, layer.slice_z);
        const PrintObjectRegions::LayerRangeRegions &layer_range = *it_layer_range;
        const std::vector<SurfaceEmbossMixedDebugCandidate> candidates =
            collect_surface_emboss_mixed_debug_candidates(layer, layer_range, mixed_mgr, num_physical);
        if (!candidates.empty())
            dump_surface_emboss_mixed_layer_state("pre-emboss-override", print_object, layer_id, layer, layer_range);

        bool              layer_changed = false;
        ExPolygons        layer_masks;
        std::vector<int>  processed_region_ids;
        processed_region_ids.reserve(layer_range.volume_regions.size());

        for (const PrintObjectRegions::VolumeRegion &volume_region : layer_range.volume_regions) {
            const ModelVolume *volume = volume_region.model_volume;
            if (volume == nullptr || !volume->is_model_part() || !volume->emboss_shape.has_value() || !volume->emboss_shape->projection.use_surface)
                continue;
            if (volume_region.region == nullptr)
                continue;

            const int region_id = volume_region.region->print_object_region_id();
            if (region_id < 0 || region_id >= layer.region_count())
                continue;
            if (std::find(processed_region_ids.begin(), processed_region_ids.end(), region_id) != processed_region_ids.end())
                continue;
            processed_region_ids.emplace_back(region_id);

            const unsigned int filament_id = unsigned(std::max(0, volume_region.region->config().wall_filament.value));
            if (!mixed_mgr.is_mixed(filament_id, num_physical))
                continue;

            LayerRegion *emboss_layerm = layer.get_region(region_id);
            if (emboss_layerm == nullptr || emboss_layerm->slices.empty())
                continue;

            ExPolygons override_mask = to_expolygons(emboss_layerm->slices.surfaces);
            if (override_mask.empty())
                continue;

            if (const float delta_scaled = emboss_surface_mixed_shell_override_delta(*emboss_layerm, *volume);
                delta_scaled > float(EPSILON)) {
                override_mask = offset_ex(override_mask, delta_scaled);
                if (override_mask.empty())
                    continue;
                if (layer_masks.empty())
                    layer_masks = collect_layer_region_slices(layer);
                if (!layer_masks.empty())
                    override_mask = intersection_ex(override_mask, layer_masks);
                if (override_mask.empty())
                    continue;
            }

            {
                std::ostringstream line;
                line << std::fixed << std::setprecision(4)
                     << "stage=override-mask"
                     << " layer=" << layer_id
                     << " region=" << region_id
                     << " volume_name=" << volume->name
                     << " volume_extruder=" << volume->extruder_id()
                     << " cfg_wall=" << volume_region.region->config().wall_filament.value
                     << " depth=" << float(volume->emboss_shape->projection.depth)
                     << " shell_delta_mm=" << unscale<double>(emboss_surface_mixed_shell_override_delta(*emboss_layerm, *volume))
                     << " mask_area=" << std::abs(area(override_mask));
                append_surface_emboss_mixed_debug_line(print_object, line.str());
            }
            const std::string overlay_stage = "override-mask-r" + std::to_string(region_id);
            export_surface_emboss_mixed_layer_svg(overlay_stage.c_str(),
                                                  print_object,
                                                  layer_id,
                                                  layer,
                                                  candidates,
                                                  &override_mask,
                                                  "override mask");

            ExPolygons emboss_slices = to_expolygons(emboss_layerm->slices.surfaces);
            bool       emboss_changed = false;

            for (int target_region_id = 0; target_region_id < layer.region_count(); ++target_region_id) {
                if (target_region_id == region_id)
                    continue;

                LayerRegion *target_layerm = layer.get_region(target_region_id);
                if (target_layerm == nullptr || target_layerm->slices.empty())
                    continue;
                if (target_layerm->region().config().wall_filament.value == int(filament_id))
                    continue;

                ExPolygons stolen = intersection_ex(target_layerm->slices.surfaces, override_mask);
                if (stolen.empty())
                    continue;

                append(emboss_slices, stolen);
                emboss_changed = true;
                ++stolen_regions;

                std::ostringstream line;
                line << std::fixed << std::setprecision(4)
                     << "stage=override-steal"
                     << " layer=" << layer_id
                     << " emboss_region=" << region_id
                     << " from_region=" << target_region_id
                     << " from_wall=" << target_layerm->region().config().wall_filament.value
                     << " stolen_area=" << std::abs(area(stolen));
                append_surface_emboss_mixed_debug_line(print_object, line.str());

                Polygons remaining = diff(to_polygons(target_layerm->slices.surfaces), override_mask);
                if (!remaining.empty())
                    remaining = opening(union_ex(remaining), scaled<float>(5. * EPSILON), scaled<float>(5. * EPSILON));
                target_layerm->slices.set(union_ex(remaining), stInternal);
                layer_changed = true;
            }

            if (emboss_changed) {
                if (emboss_slices.size() > 1)
                    emboss_slices = closing_ex(emboss_slices, scaled<float>(10. * EPSILON));
                emboss_layerm->slices.set(std::move(emboss_slices), stInternal);
                ++changed_regions;
                layer_changed = true;
            } else {
                std::ostringstream line;
                line << "stage=override-no-steal"
                     << " layer=" << layer_id
                     << " region=" << region_id
                     << " volume_name=" << volume->name;
                append_surface_emboss_mixed_debug_line(print_object, line.str());
            }
        }

        if (!candidates.empty())
            dump_surface_emboss_mixed_layer_state("post-emboss-override", print_object, layer_id, layer, layer_range);

        if (layer_changed)
            ++changed_layers;
    }

    if (changed_regions == 0)
        return false;

    BOOST_LOG_TRIVIAL(warning) << "Surface emboss mixed-region override applied"
                               << " object=" << (print_object.model_object() ? print_object.model_object()->name : std::string("<unknown>"))
                               << " changed_layers=" << changed_layers
                               << " changed_regions=" << changed_regions
                               << " stolen_regions=" << stolen_regions;
    return true;
}

// Clip fixed-state extruder masks for a single pass of a split interval.
static std::vector<ExPolygons> build_local_z_transition_fixed_masks_for_pass(
    const std::vector<ExPolygons> &current_masks_by_extruder,
    const std::vector<ExPolygons> &prev_masks_by_extruder,
    const std::vector<ExPolygons> &next_masks_by_extruder,
    const size_t                   pass_idx,
    const size_t                   pass_count)
{
    if (pass_count <= 1)
        return current_masks_by_extruder;

    std::vector<ExPolygons> pass_masks_by_extruder(current_masks_by_extruder.size());
    const bool is_lowest_pass  = pass_idx == 0;
    const bool is_highest_pass = pass_idx + 1 >= pass_count;

    for (size_t extruder_idx = 0; extruder_idx < current_masks_by_extruder.size(); ++extruder_idx) {
        const ExPolygons &current_masks = current_masks_by_extruder[extruder_idx];
        if (current_masks.empty())
            continue;

        const ExPolygons prev_masks = extruder_idx < prev_masks_by_extruder.size() ? prev_masks_by_extruder[extruder_idx] : ExPolygons();
        const ExPolygons next_masks = extruder_idx < next_masks_by_extruder.size() ? next_masks_by_extruder[extruder_idx] : ExPolygons();

        const ExPolygons current_and_prev = prev_masks.empty() ? ExPolygons() : intersection_ex(current_masks, prev_masks);
        const ExPolygons current_and_next = next_masks.empty() ? ExPolygons() : intersection_ex(current_masks, next_masks);
        const ExPolygons persistent =
            current_and_prev.empty() || current_and_next.empty() ? ExPolygons() : intersection_ex(current_and_prev, current_and_next);

        ExPolygons entering = current_and_next;
        if (!entering.empty() && !current_and_prev.empty())
            entering = diff_ex(entering, current_and_prev);

        ExPolygons exiting = current_and_prev;
        if (!exiting.empty() && !current_and_next.empty())
            exiting = diff_ex(exiting, current_and_next);

        ExPolygons covered;
        if (!persistent.empty())
            append(covered, persistent);
        if (!entering.empty())
            append(covered, entering);
        if (!exiting.empty())
            append(covered, exiting);
        if (covered.size() > 1)
            covered = union_ex(covered);

        const ExPolygons isolated = covered.empty() ? current_masks : diff_ex(current_masks, covered);

        ExPolygons assigned;
        if (!persistent.empty())
            append(assigned, persistent);
        if (is_lowest_pass && !exiting.empty())
            append(assigned, exiting);
        if (is_highest_pass) {
            if (!entering.empty())
                append(assigned, entering);
            if (!isolated.empty())
                append(assigned, isolated);
        }
        if (assigned.size() > 1)
            assigned = union_ex(assigned);
        pass_masks_by_extruder[extruder_idx] = std::move(assigned);
    }

    return pass_masks_by_extruder;
}

// --- Task 30: whole-object segmentation merge ---

// Rasterize wall_filament assignments from LayerRegion into a per-layer segmentation channel map.
// Only layers with mixed-filament assignments are populated.
static std::vector<std::vector<ExPolygons>> whole_object_local_z_segmentation_by_mixed_wall(const PrintObject &print_object)
{
    std::vector<std::vector<ExPolygons>> segmentation;

    const Print *print = print_object.print();
    if (print == nullptr || print_object.layer_count() == 0)
        return segmentation;

    const size_t num_physical = print->config().filament_colour.size();
    const size_t num_total    = print->mixed_filament_manager().total_filaments(num_physical);
    if (num_total <= num_physical)
        return segmentation;

    segmentation.assign(print_object.layer_count(), std::vector<ExPolygons>(num_total + 1));
    const MixedFilamentManager &mixed_mgr = print->mixed_filament_manager();
    size_t mixed_region_layers = 0;
    size_t mixed_region_count  = 0;

    for (size_t layer_id = 0; layer_id < print_object.layer_count(); ++layer_id) {
        const Layer &layer = *print_object.get_layer(int(layer_id));
        bool layer_has_mixed_region = false;
        for (int region_id = 0; region_id < layer.region_count(); ++region_id) {
            const LayerRegion *layerm = layer.get_region(region_id);
            if (layerm == nullptr || layerm->slices.empty())
                continue;

            const unsigned int filament_id = unsigned(std::max(0, layerm->region().config().wall_filament.value));
            if (!mixed_mgr.is_mixed(filament_id, num_physical))
                continue;
            if (filament_id >= segmentation[layer_id].size())
                continue;

            ExPolygons state_masks = to_expolygons(layerm->slices.surfaces);
            if (state_masks.empty())
                continue;

            append(segmentation[layer_id][filament_id], std::move(state_masks));
            layer_has_mixed_region = true;
            ++mixed_region_count;
        }

        if (layer_has_mixed_region) {
            ++mixed_region_layers;
            for (size_t channel_idx = num_physical + 1; channel_idx < segmentation[layer_id].size(); ++channel_idx) {
                ExPolygons &state_masks = segmentation[layer_id][channel_idx];
                if (state_masks.size() > 1)
                    state_masks = union_ex(state_masks);
            }
        }
    }

    if (mixed_region_count == 0)
        return {};

    BOOST_LOG_TRIVIAL(info) << "Local-Z whole-object wall segmentation prepared"
                            << " object=" << (print_object.model_object() ? print_object.model_object()->name : std::string("<unknown>"))
                            << " mixed_region_layers=" << mixed_region_layers
                            << " mixed_region_count=" << mixed_region_count
                            << " physical_filaments=" << num_physical
                            << " total_filaments=" << num_total;
    return segmentation;
}

// Merge painted mm_segmentation with whole-object wall masks; painted areas dominate.
// Gate: dithering_local_z_whole_objects=true.
static std::vector<std::vector<ExPolygons>> local_z_planner_segmentation_with_whole_object_mixed_wall(
    const PrintObject                          &print_object,
    const std::vector<std::vector<ExPolygons>> &paint_segmentation)
{
    const Print *print = print_object.print();
    if (print == nullptr || paint_segmentation.empty())
        return paint_segmentation;

    std::vector<std::vector<ExPolygons>> augmented = whole_object_local_z_segmentation_by_mixed_wall(print_object);
    if (augmented.empty())
        return paint_segmentation;

    const size_t num_physical             = print->config().filament_colour.size();
    const MixedFilamentManager &mixed_mgr = print->mixed_filament_manager();
    size_t overlay_layers              = 0;
    size_t overlay_mixed_channels      = 0;
    size_t physical_override_layers    = 0;

    for (size_t layer_id = 0; layer_id < augmented.size() && layer_id < paint_segmentation.size(); ++layer_id) {
        if (augmented[layer_id].size() < paint_segmentation[layer_id].size())
            augmented[layer_id].resize(paint_segmentation[layer_id].size());

        ExPolygons painted_overrides;
        for (size_t channel_idx = 1; channel_idx < paint_segmentation[layer_id].size(); ++channel_idx) {
            const ExPolygons &state_masks = paint_segmentation[layer_id][channel_idx];
            if (!state_masks.empty())
                append(painted_overrides, state_masks);
        }
        if (painted_overrides.size() > 1)
            painted_overrides = union_ex(painted_overrides);

        bool layer_has_overlay = false;
        if (!painted_overrides.empty()) {
            bool clipped_for_physical_override = false;
            for (size_t channel_idx = num_physical + 1; channel_idx < augmented[layer_id].size(); ++channel_idx) {
                ExPolygons &state_masks = augmented[layer_id][channel_idx];
                if (state_masks.empty())
                    continue;
                const ExPolygons clipped_masks = diff_ex(state_masks, painted_overrides);
                if (clipped_masks.size() != state_masks.size())
                    clipped_for_physical_override = true;
                state_masks = clipped_masks;
            }
            if (clipped_for_physical_override)
                ++physical_override_layers;
            layer_has_overlay = true;
        }

        for (size_t channel_idx = 1; channel_idx < paint_segmentation[layer_id].size(); ++channel_idx) {
            const ExPolygons &state_masks = paint_segmentation[layer_id][channel_idx];
            if (state_masks.empty())
                continue;

            const unsigned int state_id = segmentation_channel_filament_id(channel_idx);
            if (channel_idx >= augmented[layer_id].size())
                augmented[layer_id].resize(channel_idx + 1);

            append(augmented[layer_id][channel_idx], state_masks);
            layer_has_overlay = true;
            if (mixed_mgr.is_mixed(state_id, num_physical))
                ++overlay_mixed_channels;
        }

        for (size_t channel_idx = num_physical + 1; channel_idx < augmented[layer_id].size(); ++channel_idx) {
            ExPolygons &state_masks = augmented[layer_id][channel_idx];
            if (state_masks.size() > 1)
                state_masks = union_ex(state_masks);
        }
        if (layer_has_overlay)
            ++overlay_layers;
    }

    if (overlay_layers > 0) {
        BOOST_LOG_TRIVIAL(info) << "Local-Z planner merged whole-object mixed wall masks with painted overrides"
                                << " object=" << (print_object.model_object() ? print_object.model_object()->name : std::string("<unknown>"))
                                << " overlay_layers=" << overlay_layers
                                << " overlay_mixed_channels=" << overlay_mixed_channels
                                << " physical_override_layers=" << physical_override_layers;
    }
    return augmented;
}

// --- main plan builder ---

// Returns a pointillism sequence for mf (currently disabled / returns empty).
static std::vector<unsigned int> pointillism_sequence_for_row_lz(const MixedFilament &mf, size_t num_physical)
{
    (void)mf; (void)num_physical;
    return {};
}

template<typename ThrowOnCancel>
static void build_local_z_plan(PrintObject &print_object, const std::vector<std::vector<ExPolygons>> &segmentation, ThrowOnCancel throw_on_cancel)
{
    print_object.clear_local_z_plan();

    const Print *print = print_object.print();
    const std::string object_name = print_object.model_object() ? print_object.model_object()->name : std::string("<unknown>");
    if (print == nullptr || print_object.layer_count() == 0 || segmentation.size() != print_object.layer_count()) {
        BOOST_LOG_TRIVIAL(debug) << "Local-Z plan skipped: invalid preconditions"
                                 << " object=" << object_name;
        return;
    }

    const DynamicPrintConfig &full_cfg  = print->full_print_config();
    const PrintConfig        &print_cfg = print->config();
    const bool local_z_mode = bool_from_full_config(full_cfg, "dithering_local_z_mode", print_cfg.dithering_local_z_mode.value);
    const bool local_z_whole_objects =
        bool_from_full_config(full_cfg, "dithering_local_z_whole_objects", print_cfg.dithering_local_z_whole_objects.value);
    const bool local_z_direct_multicolor =
        bool_from_full_config(full_cfg, "dithering_local_z_direct_multicolor",
                              print_cfg.dithering_local_z_direct_multicolor.value);
    if (!local_z_mode) {
        BOOST_LOG_TRIVIAL(debug) << "Local-Z plan skipped: mode disabled"
                                 << " object=" << object_name;
        return;
    }
    coordf_t mixed_lower = float_from_full_config(full_cfg, "mixed_filament_height_lower_bound",
                                                  coordf_t(print_cfg.mixed_filament_height_lower_bound.value));
    coordf_t mixed_upper = float_from_full_config(full_cfg, "mixed_filament_height_upper_bound",
                                                  coordf_t(print_cfg.mixed_filament_height_upper_bound.value));
    coordf_t preferred_a = float_from_full_config(full_cfg, "mixed_color_layer_height_a",
                                                  coordf_t(print_cfg.mixed_color_layer_height_a.value));
    coordf_t preferred_b = float_from_full_config(full_cfg, "mixed_color_layer_height_b",
                                                  coordf_t(print_cfg.mixed_color_layer_height_b.value));
    mixed_lower = std::max<coordf_t>(0.01f, mixed_lower);
    mixed_upper = std::max<coordf_t>(mixed_lower, mixed_upper);
    preferred_a = std::max<coordf_t>(0.f, preferred_a);
    preferred_b = std::max<coordf_t>(0.f, preferred_b);

    const size_t num_physical = print_cfg.filament_colour.size();
    if (num_physical == 0) {
        BOOST_LOG_TRIVIAL(warning) << "Local-Z plan skipped: no physical filaments"
                                   << " object=" << object_name;
        return;
    }

    const MixedFilamentManager &mixed_mgr = print->mixed_filament_manager();
    const auto                 &mixed_rows = mixed_mgr.mixed_filaments();

    // Direct multicolor rows (3+ components): stubs for Task 31 — treat as pair cadence.
    std::vector<std::vector<unsigned int>> row_direct_component_ids(mixed_rows.size());
    std::vector<std::vector<int>>          row_direct_component_weights(mixed_rows.size());
    std::vector<std::vector<double>>       row_direct_component_error_mm(mixed_rows.size());
    std::vector<uint8_t>                   row_uses_direct_multicolor_solver(mixed_rows.size(), uint8_t(0));
    if (local_z_direct_multicolor && preferred_a <= EPSILON && preferred_b <= EPSILON) {
        for (size_t row_idx = 0; row_idx < mixed_rows.size(); ++row_idx) {
            if (local_z_direct_multicolor_row(mixed_rows[row_idx],
                                              num_physical,
                                              &row_direct_component_ids[row_idx],
                                              &row_direct_component_weights[row_idx])) {
                row_uses_direct_multicolor_solver[row_idx] = uint8_t(1);
                row_direct_component_error_mm[row_idx].assign(row_direct_component_ids[row_idx].size(), 0.0);
            }
        }
    }

    // Pointillism rows (same-layer stripe): skip Local-Z if active.
    std::vector<uint8_t> pointillism_row_eligible(mixed_rows.size(), uint8_t(0));
    for (size_t row_idx = 0; row_idx < mixed_rows.size(); ++row_idx) {
        const std::vector<unsigned int> sequence = pointillism_sequence_for_row_lz(mixed_rows[row_idx], num_physical);
        if (unique_extruder_count(sequence, num_physical) >= 2)
            pointillism_row_eligible[row_idx] = uint8_t(1);
    }

    size_t pointillism_rows = 0;
    if (!pointillism_row_eligible.empty()) {
        std::vector<uint8_t> pointillism_row_active(pointillism_row_eligible.size(), uint8_t(0));
        for (size_t layer_id = 0; layer_id < segmentation.size(); ++layer_id) {
            const auto &layer_segmentation = segmentation[layer_id];
            for (size_t channel_idx = 1; channel_idx < layer_segmentation.size(); ++channel_idx) {
                if (layer_segmentation[channel_idx].empty())
                    continue;
                const unsigned int state_id = segmentation_channel_filament_id(channel_idx);
                if (!mixed_mgr.is_mixed(state_id, num_physical))
                    continue;
                const int mixed_idx = mixed_mgr.mixed_index_from_filament_id(state_id, num_physical);
                if (mixed_idx < 0 || size_t(mixed_idx) >= pointillism_row_eligible.size())
                    continue;
                const size_t row_idx = size_t(mixed_idx);
                if (pointillism_row_eligible[row_idx] == 0 || pointillism_row_active[row_idx] != 0)
                    continue;
                pointillism_row_active[row_idx] = uint8_t(1);
                ++pointillism_rows;
            }
        }
    }
    if (pointillism_rows > 0) {
        BOOST_LOG_TRIVIAL(warning) << "Local-Z plan skipped: interleaved stripe mixed pattern active"
                                   << " object=" << object_name
                                   << " interleaved_rows=" << pointillism_rows;
        return;
    }

    // Build pair-cadence cycles for each non-direct-multicolor row.
    std::vector<std::vector<LocalZActivePair>> row_pair_cycles(mixed_rows.size());
    std::vector<uint8_t>                       row_uses_layer_cycle_pair(mixed_rows.size(), uint8_t(0));
    for (size_t row_idx = 0; row_idx < mixed_rows.size(); ++row_idx) {
        if (row_uses_direct_multicolor_solver[row_idx] != 0)
            continue;
        row_pair_cycles[row_idx] = build_local_z_pair_cycle_for_row(mixed_rows[row_idx], num_physical);
        if (!row_pair_cycles[row_idx].empty())
            row_uses_layer_cycle_pair[row_idx] = uint8_t(1);
    }

    BOOST_LOG_TRIVIAL(debug) << "Local-Z plan start"
                             << " object=" << object_name
                             << " layers=" << print_object.layer_count()
                             << " mixed_lower=" << mixed_lower
                             << " mixed_upper=" << mixed_upper
                             << " preferred_a=" << preferred_a
                             << " preferred_b=" << preferred_b
                             << " direct_multicolor=" << (local_z_direct_multicolor ? 1 : 0)
                             << " physical_filaments=" << num_physical;

    std::vector<LocalZInterval> intervals;
    std::vector<SubLayerPlan>   plans;
    intervals.reserve(print_object.layer_count());

    size_t mixed_intervals              = 0;
    size_t split_intervals              = 0;
    size_t non_split_mixed_intervals    = 0;
    size_t total_generated_sublayer_cnt = 0;
    size_t total_mixed_state_layers     = 0;
    size_t forced_height_resolve_calls  = 0;
    size_t forced_height_resolve_invalid_target  = 0;
    size_t split_passes_total                    = 0;
    size_t split_passes_with_painted_masks       = 0;
    size_t split_intervals_without_painted_masks = 0;
    size_t strict_ab_assignments                 = 0;
    size_t alternating_height_intervals          = 0;
    size_t shared_multi_row_fallback_intervals   = 0;
    constexpr size_t LOCAL_Z_MAX_ISOLATED_ACTIVE_ROWS       = 2;
    constexpr size_t LOCAL_Z_MAX_ISOLATED_MASK_COMPONENTS   = 24;
    constexpr size_t LOCAL_Z_MAX_ISOLATED_MASK_VERTICES     = 1200;
    constexpr bool   LOCAL_Z_SHARED_FALLBACK_ENABLED        = false;

    std::vector<int> row_cadence_index(mixed_rows.size(), 0);
    std::vector<int> row_layer_cycle_index(mixed_rows.size(), 0);
    std::vector<uint8_t> row_active_prev_layer(mixed_rows.size(), uint8_t(0));

    for (size_t layer_id = 0; layer_id < print_object.layer_count(); ++layer_id) {
        throw_on_cancel();

        const Layer &layer = *print_object.get_layer(int(layer_id));
        LocalZInterval interval;
        interval.layer_id           = layer_id;
        interval.z_lo               = layer.print_z - layer.height;
        interval.z_hi               = layer.print_z;
        interval.base_height        = layer.height;
        interval.sublayer_height    = layer.height;
        interval.first_sublayer_idx = plans.size();

        ExPolygons mixed_masks;
        size_t     mixed_state_count = 0;
        std::vector<uint8_t> row_active_this_layer(mixed_rows.size(), uint8_t(0));
        size_t     dominant_mixed_idx    = size_t(-1);
        double     dominant_mixed_area   = -1.0;
        double     dominant_gradient_h_a = 0.0;
        double     dominant_gradient_h_b = 0.0;
        bool       dominant_gradient_valid = false;

        for (size_t channel_idx = 0; channel_idx < segmentation[layer_id].size(); ++channel_idx) {
            const ExPolygons &state_masks = segmentation[layer_id][channel_idx];
            if (state_masks.empty())
                continue;
            const unsigned int state_id = segmentation_channel_filament_id(channel_idx);
            if (!mixed_mgr.is_mixed(state_id, num_physical))
                continue;
            const int mixed_idx = mixed_mgr.mixed_index_from_filament_id(state_id, num_physical);
            if (mixed_idx < 0 || size_t(mixed_idx) >= mixed_rows.size())
                continue;
            const MixedFilament &mf = mixed_rows[size_t(mixed_idx)];
            if (!local_z_eligible_mixed_row(mf))
                continue;

            interval.has_mixed_paint = true;
            row_active_this_layer[size_t(mixed_idx)] = uint8_t(1);
            ++mixed_state_count;
            append(mixed_masks, state_masks);

            const double mixed_area = std::abs(area(state_masks));
            if (mixed_area > dominant_mixed_area) {
                dominant_mixed_area = mixed_area;
                dominant_mixed_idx  = size_t(mixed_idx);
            }
        }

        for (size_t row_idx = 0; row_idx < row_active_this_layer.size(); ++row_idx) {
            if (row_active_this_layer[row_idx] != 0 && row_active_prev_layer[row_idx] == 0) {
                if (row_uses_direct_multicolor_solver[row_idx] != 0 &&
                    row_direct_component_error_mm[row_idx].size() == row_direct_component_ids[row_idx].size())
                    std::fill(row_direct_component_error_mm[row_idx].begin(), row_direct_component_error_mm[row_idx].end(), 0.0);

                const bool can_sync_to_dominant =
                    local_z_whole_objects &&
                    dominant_mixed_idx < mixed_rows.size() &&
                    dominant_mixed_idx != row_idx &&
                    row_active_this_layer[dominant_mixed_idx] != 0;
                if (can_sync_to_dominant) {
                    row_cadence_index[row_idx]     = row_cadence_index[dominant_mixed_idx];
                    row_layer_cycle_index[row_idx] = row_layer_cycle_index[dominant_mixed_idx];
                } else {
                    row_cadence_index[row_idx]     = 0;
                    row_layer_cycle_index[row_idx] = 0;
                }
            }
        }

        std::vector<LocalZActivePair> row_active_pairs(mixed_rows.size());
        for (size_t row_idx = 0; row_idx < row_active_this_layer.size(); ++row_idx) {
            if (row_active_this_layer[row_idx] == 0 || !local_z_eligible_mixed_row(mixed_rows[row_idx]))
                continue;
            if (row_uses_direct_multicolor_solver[row_idx] != 0)
                continue;

            const int cadence_index = row_uses_layer_cycle_pair[row_idx] != 0
                ? row_layer_cycle_index[row_idx]
                : row_cadence_index[row_idx];
            row_active_pairs[row_idx] =
                derive_local_z_active_pair(mixed_rows[row_idx], row_pair_cycles[row_idx], num_physical, cadence_index);
        }

        if (dominant_mixed_idx < mixed_rows.size()) {
            const LocalZActivePair &dominant_pair = row_active_pairs[dominant_mixed_idx];
            const int dominant_mix_b_percent =
                dominant_pair.valid_pair(num_physical) ? dominant_pair.mix_b_percent : mixed_rows[dominant_mixed_idx].mix_b_percent;
            if (row_uses_direct_multicolor_solver[dominant_mixed_idx] == 0) {
                compute_local_z_gradient_component_heights(dominant_mix_b_percent, mixed_lower, mixed_upper,
                                                           dominant_gradient_h_a, dominant_gradient_h_b);
                dominant_gradient_valid = true;
            }
        }

        total_mixed_state_layers += mixed_state_count;
        if (!mixed_masks.empty())
            mixed_masks = union_ex(mixed_masks);
        if (interval.has_mixed_paint)
            ++mixed_intervals;

        const ExPolygons layer_masks = collect_layer_region_slices(layer);
        ExPolygons       base_masks  = layer_masks;
        if (interval.has_mixed_paint && !base_masks.empty() && !mixed_masks.empty()) {
            base_masks = diff_ex(base_masks, mixed_masks);
            if (!base_masks.empty()) {
                const Polygons filtered = opening(to_polygons(base_masks), scaled<float>(5. * EPSILON), scaled<float>(5. * EPSILON));
                base_masks = union_ex(filtered);
            }
        }

        const size_t active_mixed_rows = size_t(std::count(row_active_this_layer.begin(), row_active_this_layer.end(), uint8_t(1)));
        std::vector<ExPolygons> row_state_masks(mixed_rows.size());
        std::vector<unsigned int> row_state_ids(mixed_rows.size(), 0);
        std::vector<ExPolygons> fixed_state_masks_by_extruder(num_physical);

        for (size_t channel_idx = 0; channel_idx < segmentation[layer_id].size(); ++channel_idx) {
            const ExPolygons &state_masks = segmentation[layer_id][channel_idx];
            if (state_masks.empty())
                continue;
            const unsigned int state_id = segmentation_channel_filament_id(channel_idx);
            if (state_id >= 1 && state_id <= num_physical) {
                append(fixed_state_masks_by_extruder[state_id - 1], state_masks);
                continue;
            }
            if (!mixed_mgr.is_mixed(state_id, num_physical))
                continue;
            const int mixed_idx = mixed_mgr.mixed_index_from_filament_id(state_id, num_physical);
            if (mixed_idx < 0 || size_t(mixed_idx) >= mixed_rows.size())
                continue;
            const size_t row_idx = size_t(mixed_idx);
            if (row_active_this_layer[row_idx] == 0 || !local_z_eligible_mixed_row(mixed_rows[row_idx]))
                continue;
            row_state_ids[row_idx] = state_id;
            append(row_state_masks[row_idx], state_masks);
        }
        for (ExPolygons &masks : row_state_masks)
            if (masks.size() > 1)
                masks = union_ex(masks);
        for (ExPolygons &masks : fixed_state_masks_by_extruder)
            if (masks.size() > 1)
                masks = union_ex(masks);

        const std::vector<ExPolygons> prev_fixed_state_masks_by_extruder =
            layer_id > 0 ? collect_local_z_fixed_state_masks_by_extruder(segmentation[layer_id - 1], num_physical)
                         : std::vector<ExPolygons>(num_physical);
        const std::vector<ExPolygons> next_fixed_state_masks_by_extruder =
            layer_id + 1 < segmentation.size() ? collect_local_z_fixed_state_masks_by_extruder(segmentation[layer_id + 1], num_physical)
                                               : std::vector<ExPolygons>(num_physical);

        ExPolygons fixed_state_masks_union;
        for (const ExPolygons &masks : fixed_state_masks_by_extruder)
            if (!masks.empty())
                append(fixed_state_masks_union, masks);
        if (fixed_state_masks_union.size() > 1)
            fixed_state_masks_union = union_ex(fixed_state_masks_union);

        if (interval.has_mixed_paint && local_z_whole_objects && !fixed_state_masks_union.empty()) {
            if (!base_masks.empty()) {
                base_masks = diff_ex(base_masks, fixed_state_masks_union);
                if (!base_masks.empty()) {
                    const Polygons filtered = opening(to_polygons(base_masks), scaled<float>(5. * EPSILON), scaled<float>(5. * EPSILON));
                    base_masks = union_ex(filtered);
                }
            }
            append(mixed_masks, fixed_state_masks_union);
            if (mixed_masks.size() > 1)
                mixed_masks = union_ex(mixed_masks);
        }

        if (local_z_whole_objects && !fixed_state_masks_union.empty()) {
            constexpr double LOCAL_Z_WHOLE_OBJECT_FIXED_GUARD_MM = 0.10;
            ExPolygons fixed_state_guard_masks = offset_ex(fixed_state_masks_union, float(scale_(LOCAL_Z_WHOLE_OBJECT_FIXED_GUARD_MM)));
            if (fixed_state_guard_masks.empty())
                fixed_state_guard_masks = fixed_state_masks_union;
            else if (fixed_state_guard_masks.size() > 1)
                fixed_state_guard_masks = union_ex(fixed_state_guard_masks);

            for (ExPolygons &masks : row_state_masks) {
                if (masks.empty())
                    continue;
                masks = diff_ex(masks, fixed_state_guard_masks);
                if (masks.size() > 1)
                    masks = union_ex(masks);
            }
        }

        size_t active_row_mask_components = 0;
        size_t active_row_mask_vertices   = 0;
        for (size_t row_idx = 0; row_idx < row_state_masks.size(); ++row_idx)
            if (row_active_this_layer[row_idx] != 0) {
                active_row_mask_components += row_state_masks[row_idx].size();
                for (const ExPolygon &expoly : row_state_masks[row_idx]) {
                    active_row_mask_vertices += expoly.contour.points.size();
                    for (const Polygon &hole : expoly.holes)
                        active_row_mask_vertices += hole.points.size();
                }
            }

        std::vector<std::vector<double>> isolated_row_pass_heights(mixed_rows.size());
        bool isolated_multi_row_mode = false;
        const bool shared_multi_row_fallback =
            LOCAL_Z_SHARED_FALLBACK_ENABLED &&
            interval.has_mixed_paint &&
            preferred_a <= EPSILON &&
            preferred_b <= EPSILON &&
            active_mixed_rows > 1 &&
            (active_mixed_rows > LOCAL_Z_MAX_ISOLATED_ACTIVE_ROWS ||
             active_row_mask_components > LOCAL_Z_MAX_ISOLATED_MASK_COMPONENTS ||
             active_row_mask_vertices > LOCAL_Z_MAX_ISOLATED_MASK_VERTICES);
        if (shared_multi_row_fallback)
            ++shared_multi_row_fallback_intervals;

        if (interval.has_mixed_paint &&
            preferred_a <= EPSILON &&
            preferred_b <= EPSILON &&
            !shared_multi_row_fallback &&
            active_mixed_rows > 1) {
            size_t isolated_rows_with_split = 0;
            for (size_t row_idx = 0; row_idx < row_active_this_layer.size(); ++row_idx) {
                if (row_active_this_layer[row_idx] == 0)
                    continue;

                std::vector<double> row_passes;
                if (row_uses_direct_multicolor_solver[row_idx] != 0) {
                    row_passes = build_local_z_direct_multicolor_pass_heights(mixed_rows[row_idx],
                                                                             row_direct_component_weights[row_idx],
                                                                             interval.base_height,
                                                                             mixed_lower,
                                                                             mixed_upper,
                                                                             row_direct_component_ids[row_idx].size());
                } else {
                    double row_h_a = 0.0;
                    double row_h_b = 0.0;
                    const LocalZActivePair &active_pair = row_active_pairs[row_idx];
                    const int row_mix_b_percent =
                        active_pair.valid_pair(num_physical) ? active_pair.mix_b_percent : mixed_rows[row_idx].mix_b_percent;
                    compute_local_z_gradient_component_heights(row_mix_b_percent, mixed_lower, mixed_upper, row_h_a, row_h_b);
                    row_passes = active_pair.uses_layer_cycle_sequence
                        ? build_local_z_two_pass_heights(interval.base_height, mixed_lower, mixed_upper, row_h_a, row_h_b)
                        : build_local_z_alternating_pass_heights(interval.base_height,
                                                                 mixed_lower,
                                                                 mixed_upper,
                                                                 row_h_a,
                                                                 row_h_b);
                }
                if (row_passes.empty())
                    row_passes.emplace_back(interval.base_height);
                if (!sanitize_local_z_pass_heights(row_passes, interval.base_height, mixed_lower, mixed_upper))
                    row_passes = build_uniform_local_z_pass_heights(interval.base_height, mixed_lower, mixed_upper);
                if (row_passes.size() > 1)
                    ++isolated_rows_with_split;
                isolated_row_pass_heights[row_idx] = std::move(row_passes);
            }
            if (isolated_rows_with_split > 0) {
                isolated_multi_row_mode = true;
                ++alternating_height_intervals;
            }
        }

        std::vector<double> pass_heights;
        if (interval.has_mixed_paint && !isolated_multi_row_mode) {
            if (preferred_a <= EPSILON && preferred_b <= EPSILON) {
                if (shared_multi_row_fallback) {
                    pass_heights = build_local_z_shared_pass_heights(interval.base_height, mixed_lower, mixed_upper);
                    if (pass_heights.size() > 1)
                        ++alternating_height_intervals;
                } else if (dominant_mixed_idx < mixed_rows.size() &&
                           row_uses_direct_multicolor_solver[dominant_mixed_idx] != 0) {
                    pass_heights = build_local_z_direct_multicolor_pass_heights(mixed_rows[dominant_mixed_idx],
                                                                                row_direct_component_weights[dominant_mixed_idx],
                                                                                interval.base_height,
                                                                                mixed_lower,
                                                                                mixed_upper,
                                                                                row_direct_component_ids[dominant_mixed_idx].size());
                    if (pass_heights.size() > 1)
                        ++alternating_height_intervals;
                } else if (dominant_gradient_valid) {
                    const bool dominant_uses_pair_cycle =
                        dominant_mixed_idx < mixed_rows.size() && row_active_pairs[dominant_mixed_idx].uses_layer_cycle_sequence;
                    pass_heights = dominant_uses_pair_cycle
                        ? build_local_z_two_pass_heights(interval.base_height, mixed_lower, mixed_upper,
                                                         dominant_gradient_h_a, dominant_gradient_h_b)
                        : build_local_z_alternating_pass_heights(interval.base_height,
                                                                 mixed_lower,
                                                                 mixed_upper,
                                                                 dominant_gradient_h_a,
                                                                 dominant_gradient_h_b);
                    if (pass_heights.size() > 1)
                        ++alternating_height_intervals;
                } else {
                    pass_heights = build_uniform_local_z_pass_heights(interval.base_height, mixed_lower, mixed_upper);
                }
            } else {
                pass_heights = build_local_z_pass_heights(interval.base_height,
                                                          mixed_lower,
                                                          mixed_upper,
                                                          preferred_a,
                                                          preferred_b);
            }
        } else
            pass_heights.emplace_back(interval.base_height);

        if (interval.has_mixed_paint) {
            if (!sanitize_local_z_pass_heights(pass_heights, interval.base_height, mixed_lower, mixed_upper))
                pass_heights = build_uniform_local_z_pass_heights(interval.base_height, mixed_lower, mixed_upper);
        }

        // Stabilise pass ordering: small pass first so A/B cadence stays consistent.
        if (interval.has_mixed_paint &&
            preferred_a <= EPSILON &&
            preferred_b <= EPSILON &&
            pass_heights.size() == 2 &&
            pass_heights[0] > pass_heights[1])
            std::swap(pass_heights[0], pass_heights[1]);

        const bool split_interval = interval.has_mixed_paint && (isolated_multi_row_mode || pass_heights.size() > 1);
        const bool force_height_resolve = true;
        auto build_whole_object_fixed_plans = [&](size_t first_pass_index) {
            std::vector<SubLayerPlan> fixed_plans;
            if (!local_z_whole_objects || fixed_state_masks_union.empty() || interval.base_height <= EPSILON)
                return fixed_plans;

            const std::vector<double> fixed_z_cuts {
                interval.z_lo,
                interval.z_lo + 0.5 * interval.base_height,
                interval.z_hi
            };
            const size_t fixed_pass_count      = fixed_z_cuts.size() - 1;
            const size_t fixed_dependency_group = mixed_rows.size() + 1;
            for (size_t fixed_pass_idx = 0; fixed_pass_idx < fixed_pass_count; ++fixed_pass_idx) {
                const double z_lo = fixed_z_cuts[fixed_pass_idx];
                const double z_hi = fixed_z_cuts[fixed_pass_idx + 1];
                const double pass_height = z_hi - z_lo;
                if (pass_height <= EPSILON)
                    continue;

                const std::vector<ExPolygons> fixed_masks_for_pass =
                    build_local_z_transition_fixed_masks_for_pass(fixed_state_masks_by_extruder,
                                                                  prev_fixed_state_masks_by_extruder,
                                                                  next_fixed_state_masks_by_extruder,
                                                                  fixed_pass_idx,
                                                                  fixed_pass_count);

                SubLayerPlan fixed_plan;
                fixed_plan.layer_id         = layer_id;
                fixed_plan.pass_index       = first_pass_index + fixed_plans.size();
                fixed_plan.split_interval   = true;
                fixed_plan.z_lo             = z_lo;
                fixed_plan.z_hi             = z_hi;
                fixed_plan.print_z          = z_hi;
                fixed_plan.flow_height      = pass_height;
                fixed_plan.dependency_group = fixed_dependency_group;
                fixed_plan.dependency_order = fixed_pass_idx;
                fixed_plan.painted_masks_by_extruder.assign(num_physical, ExPolygons());
                fixed_plan.fixed_painted_masks_by_extruder.assign(num_physical, ExPolygons());

                bool plan_has_fixed_masks = false;
                for (size_t extruder_idx = 0; extruder_idx < fixed_masks_for_pass.size() &&
                                             extruder_idx < fixed_plan.fixed_painted_masks_by_extruder.size();
                     ++extruder_idx) {
                    if (fixed_masks_for_pass[extruder_idx].empty())
                        continue;
                    append(fixed_plan.fixed_painted_masks_by_extruder[extruder_idx], fixed_masks_for_pass[extruder_idx]);
                    plan_has_fixed_masks = true;
                }
                if (!plan_has_fixed_masks)
                    continue;

                for (ExPolygons &masks : fixed_plan.fixed_painted_masks_by_extruder)
                    if (masks.size() > 1)
                        masks = union_ex(masks);
                fixed_plans.emplace_back(std::move(fixed_plan));
            }
            return fixed_plans;
        };

        if (split_interval) {
            ++split_intervals;
            bool interval_has_split_painted_masks = false;

            if (isolated_multi_row_mode) {
                std::vector<SubLayerPlan> isolated_plans;
                isolated_plans.reserve(std::max<size_t>(2, active_mixed_rows * 2));

                for (size_t row_idx = 0; row_idx < row_active_this_layer.size(); ++row_idx) {
                    if (row_active_this_layer[row_idx] == 0)
                        continue;
                    const ExPolygons &state_masks = row_state_masks[row_idx];
                    if (state_masks.empty())
                        continue;

                    const std::vector<double> &row_passes_raw = isolated_row_pass_heights[row_idx];
                    const std::vector<double>  row_passes = row_passes_raw.empty()
                        ? std::vector<double>{ interval.base_height }
                        : row_passes_raw;
                    const LocalZActivePair &active_pair = row_active_pairs[row_idx];
                    const bool uses_direct_multicolor = row_uses_direct_multicolor_solver[row_idx] != 0;
                    const bool valid_pair = active_pair.valid_pair(num_physical);
                    const int  orientation_cadence_index = active_pair.uses_layer_cycle_sequence
                        ? row_layer_cycle_index[row_idx]
                        : row_cadence_index[row_idx];
                    const std::vector<unsigned int> direct_sequence = uses_direct_multicolor
                        ? build_local_z_direct_multicolor_sequence(row_direct_component_ids[row_idx],
                                                                   row_direct_component_weights[row_idx],
                                                                   row_passes,
                                                                   row_direct_component_error_mm[row_idx])
                        : std::vector<unsigned int>();

                    bool start_with_a = true;
                    if (!uses_direct_multicolor && valid_pair && preferred_a <= EPSILON && preferred_b <= EPSILON) {
                        double row_h_a = 0.0;
                        double row_h_b = 0.0;
                        compute_local_z_gradient_component_heights(active_pair.mix_b_percent, mixed_lower, mixed_upper, row_h_a, row_h_b);
                        start_with_a = choose_local_z_start_with_component_a(row_passes, row_h_a, row_h_b, orientation_cadence_index);
                    }

                    double z_cursor = interval.z_lo;
                    bool   row_used = false;
                    size_t row_dependency_order = 0;
                    for (size_t pass_i = 0; pass_i < row_passes.size(); ++pass_i) {
                        if (z_cursor >= interval.z_hi - EPSILON)
                            break;

                        const double pass_height = std::min<double>(row_passes[pass_i], interval.z_hi - z_cursor);
                        if (pass_height <= EPSILON)
                            continue;
                        const double z_next = std::min<double>(interval.z_hi, z_cursor + pass_height);

                        SubLayerPlan plan;
                        plan.layer_id       = layer_id;
                        plan.pass_index     = isolated_plans.size();
                        plan.split_interval = true;
                        plan.z_lo           = z_cursor;
                        plan.z_hi           = z_next;
                        plan.print_z        = z_next;
                        plan.flow_height    = pass_height;
                        plan.dependency_group = row_idx + 1;
                        plan.dependency_order = row_dependency_order++;
                        plan.painted_masks_by_extruder.assign(num_physical, ExPolygons());
                        plan.fixed_painted_masks_by_extruder.assign(num_physical, ExPolygons());
                        ++split_passes_total;
                        ++forced_height_resolve_calls;

                        unsigned int target_extruder = 0;
                        if (uses_direct_multicolor) {
                            if (pass_i < direct_sequence.size())
                                target_extruder = direct_sequence[pass_i];
                        } else if (valid_pair) {
                            const bool even_pass = (pass_i % 2) == 0;
                            target_extruder = even_pass
                                ? (start_with_a ? active_pair.component_a : active_pair.component_b)
                                : (start_with_a ? active_pair.component_b : active_pair.component_a);
                            ++strict_ab_assignments;
                        }
                        if (target_extruder == 0) {
                            const unsigned int state_id = row_state_ids[row_idx];
                            if (state_id != 0) {
                                const int resolve_cadence_index = active_pair.uses_layer_cycle_sequence
                                    ? row_layer_cycle_index[row_idx]
                                    : row_cadence_index[row_idx];
                                target_extruder = mixed_mgr.resolve(state_id, num_physical,
                                                                    resolve_cadence_index,
                                                                    float(plan.print_z),
                                                                    float(plan.flow_height),
                                                                    force_height_resolve);
                            }
                        }
                        if (target_extruder == 0 || target_extruder > num_physical) {
                            ++forced_height_resolve_invalid_target;
                        } else {
                            append(plan.painted_masks_by_extruder[target_extruder - 1], state_masks);
                            ++split_passes_with_painted_masks;
                            interval_has_split_painted_masks = true;
                        }
                        for (ExPolygons &masks : plan.painted_masks_by_extruder)
                            if (masks.size() > 1)
                                masks = union_ex(masks);

                        isolated_plans.emplace_back(std::move(plan));
                        row_used = true;
                        if (!uses_direct_multicolor && !active_pair.uses_layer_cycle_sequence)
                            ++row_cadence_index[row_idx];
                        z_cursor = z_next;
                    }
                    if (row_used && !uses_direct_multicolor && active_pair.uses_layer_cycle_sequence)
                        ++row_layer_cycle_index[row_idx];
                }

                if (!isolated_plans.empty()) {
                    auto sort_local_z_plans = [](std::vector<SubLayerPlan> &plans) {
                        std::sort(plans.begin(), plans.end(), [](const SubLayerPlan &lhs, const SubLayerPlan &rhs) {
                            if (std::abs(lhs.print_z - rhs.print_z) > EPSILON)
                                return lhs.print_z < rhs.print_z;
                            if (std::abs(lhs.z_lo - rhs.z_lo) > EPSILON)
                                return lhs.z_lo < rhs.z_lo;
                            return lhs.pass_index < rhs.pass_index;
                        });
                    };

                    sort_local_z_plans(isolated_plans);

                    std::vector<SubLayerPlan> fixed_plans = build_whole_object_fixed_plans(isolated_plans.size());
                    if (!fixed_plans.empty()) {
                        isolated_plans.insert(isolated_plans.end(),
                                              std::make_move_iterator(fixed_plans.begin()),
                                              std::make_move_iterator(fixed_plans.end()));
                        sort_local_z_plans(isolated_plans);
                    }

                    double min_flow_height = isolated_plans.front().flow_height;
                    double max_flow_height = isolated_plans.front().flow_height;
                    for (size_t idx = 0; idx < isolated_plans.size(); ++idx) {
                        isolated_plans[idx].pass_index = idx;
                        min_flow_height = std::min(min_flow_height, isolated_plans[idx].flow_height);
                        max_flow_height = std::max(max_flow_height, isolated_plans[idx].flow_height);
                        for (ExPolygons &masks : isolated_plans[idx].painted_masks_by_extruder)
                            if (masks.size() > 1)
                                masks = union_ex(masks);
                        for (ExPolygons &masks : isolated_plans[idx].fixed_painted_masks_by_extruder)
                            if (masks.size() > 1)
                                masks = union_ex(masks);
                        if (std::any_of(isolated_plans[idx].fixed_painted_masks_by_extruder.begin(),
                                        isolated_plans[idx].fixed_painted_masks_by_extruder.end(),
                                        [](const ExPolygons &masks) { return !masks.empty(); })) {
                            ++split_passes_with_painted_masks;
                            interval_has_split_painted_masks = true;
                        }
                    }
                    isolated_plans.back().base_masks = base_masks;
                    interval.sublayer_height = min_flow_height;
                    for (SubLayerPlan &plan : isolated_plans) {
                        plans.emplace_back(std::move(plan));
                        ++interval.sublayer_count;
                        ++total_generated_sublayer_cnt;
                    }
                }
            } else {
                // Shared pass-height path: all active rows use the same split.
                std::vector<uint8_t> start_with_component_a(mixed_rows.size(), uint8_t(1));
                std::vector<std::vector<unsigned int>> row_direct_pass_sequences(mixed_rows.size());
                size_t single_dependency_group = 0;
                size_t active_dependency_rows = 0;
                for (size_t row_idx = 0; row_idx < row_state_masks.size(); ++row_idx) {
                    if (row_state_masks[row_idx].empty() || row_state_ids[row_idx] == 0)
                        continue;
                    ++active_dependency_rows;
                    single_dependency_group = row_idx + 1;
                }
                if (active_dependency_rows != 1)
                    single_dependency_group = 0;
                if (preferred_a <= EPSILON && preferred_b <= EPSILON) {
                    for (size_t row_idx = 0; row_idx < row_active_this_layer.size(); ++row_idx) {
                        if (row_active_this_layer[row_idx] == 0 || !local_z_eligible_mixed_row(mixed_rows[row_idx]))
                            continue;
                        if (row_uses_direct_multicolor_solver[row_idx] != 0) {
                            row_direct_pass_sequences[row_idx] =
                                build_local_z_direct_multicolor_sequence(row_direct_component_ids[row_idx],
                                                                         row_direct_component_weights[row_idx],
                                                                         pass_heights,
                                                                         row_direct_component_error_mm[row_idx]);
                            continue;
                        }

                        const LocalZActivePair &active_pair = row_active_pairs[row_idx];
                        if (!active_pair.valid_pair(num_physical))
                            continue;

                        double row_h_a = 0.0;
                        double row_h_b = 0.0;
                        const int orientation_cadence_index = active_pair.uses_layer_cycle_sequence
                            ? row_layer_cycle_index[row_idx]
                            : row_cadence_index[row_idx];
                        compute_local_z_gradient_component_heights(active_pair.mix_b_percent, mixed_lower, mixed_upper, row_h_a, row_h_b);
                        start_with_component_a[row_idx] =
                            choose_local_z_start_with_component_a(pass_heights, row_h_a, row_h_b, orientation_cadence_index)
                                ? uint8_t(1) : uint8_t(0);
                    }
                }

                double z_cursor = interval.z_lo;
                size_t pass_idx = 0;
                interval.sublayer_height = *std::min_element(pass_heights.begin(), pass_heights.end());
                std::vector<uint8_t> row_seen_sequence_in_interval(mixed_rows.size(), uint8_t(0));

                for (const double pass_height_nominal : pass_heights) {
                    if (z_cursor >= interval.z_hi - EPSILON)
                        break;
                    const double pass_height = std::min<double>(pass_height_nominal, interval.z_hi - z_cursor);
                    const double z_next      = std::min<double>(interval.z_hi, z_cursor + pass_height);

                    SubLayerPlan plan;
                    plan.layer_id       = layer_id;
                    plan.pass_index     = pass_idx;
                    plan.split_interval = true;
                    plan.z_lo           = z_cursor;
                    plan.z_hi           = z_next;
                    plan.print_z        = z_next;
                    plan.flow_height    = pass_height;
                    plan.dependency_group = single_dependency_group;
                    plan.dependency_order = pass_idx;
                    plan.painted_masks_by_extruder.assign(num_physical, ExPolygons());
                    plan.fixed_painted_masks_by_extruder.assign(num_physical, ExPolygons());
                    ++split_passes_total;
                    bool pass_has_painted_masks = false;
                    std::vector<uint8_t> row_seen_in_pass(mixed_rows.size(), uint8_t(0));

                    for (size_t row_idx = 0; row_idx < row_state_masks.size(); ++row_idx) {
                        const ExPolygons &state_masks = row_state_masks[row_idx];
                        if (state_masks.empty())
                            continue;
                        const unsigned int state_id = row_state_ids[row_idx];
                        if (state_id == 0)
                            continue;
                        const MixedFilament &mf = mixed_rows[row_idx];
                        if (!local_z_eligible_mixed_row(mf))
                            continue;
                        const LocalZActivePair &active_pair = row_active_pairs[row_idx];
                        const bool uses_direct_multicolor = row_uses_direct_multicolor_solver[row_idx] != 0;
                        row_seen_in_pass[row_idx] = uint8_t(1);
                        if (!uses_direct_multicolor && active_pair.uses_layer_cycle_sequence)
                            row_seen_sequence_in_interval[row_idx] = uint8_t(1);
                        ++forced_height_resolve_calls;

                        unsigned int target_extruder = 0;
                        if (uses_direct_multicolor) {
                            if (pass_idx < row_direct_pass_sequences[row_idx].size())
                                target_extruder = row_direct_pass_sequences[row_idx][pass_idx];
                        } else if (active_pair.valid_pair(num_physical)) {
                            const bool start_a   = start_with_component_a[row_idx] != 0;
                            const bool even_pass = (pass_idx % 2) == 0;
                            target_extruder = even_pass
                                ? (start_a ? active_pair.component_a : active_pair.component_b)
                                : (start_a ? active_pair.component_b : active_pair.component_a);
                            ++strict_ab_assignments;
                        }
                        if (target_extruder == 0) {
                            const int resolve_cadence_index = active_pair.uses_layer_cycle_sequence
                                ? row_layer_cycle_index[row_idx]
                                : row_cadence_index[row_idx];
                            target_extruder = mixed_mgr.resolve(state_id, num_physical,
                                                                resolve_cadence_index,
                                                                float(plan.print_z),
                                                                float(plan.flow_height),
                                                                force_height_resolve);
                        }
                        if (target_extruder == 0 || target_extruder > num_physical) {
                            ++forced_height_resolve_invalid_target;
                            continue;
                        }
                        append(plan.painted_masks_by_extruder[target_extruder - 1], state_masks);
                        pass_has_painted_masks = true;
                    }
                    for (ExPolygons &masks : plan.painted_masks_by_extruder)
                        if (masks.size() > 1)
                            masks = union_ex(masks);
                    for (ExPolygons &masks : plan.fixed_painted_masks_by_extruder)
                        if (masks.size() > 1)
                            masks = union_ex(masks);
                    if (pass_has_painted_masks) {
                        ++split_passes_with_painted_masks;
                        interval_has_split_painted_masks = true;
                    }

                    if (z_next >= interval.z_hi - EPSILON)
                        plan.base_masks = base_masks;

                    plans.emplace_back(std::move(plan));
                    ++interval.sublayer_count;
                    ++total_generated_sublayer_cnt;
                    ++pass_idx;
                    for (size_t mixed_idx = 0; mixed_idx < row_seen_in_pass.size(); ++mixed_idx)
                        if (row_seen_in_pass[mixed_idx] != 0 &&
                            row_uses_layer_cycle_pair[mixed_idx] == 0 &&
                            row_uses_direct_multicolor_solver[mixed_idx] == 0)
                            ++row_cadence_index[mixed_idx];
                    z_cursor = z_next;
                }
                std::vector<SubLayerPlan> fixed_plans = build_whole_object_fixed_plans(pass_idx);
                for (SubLayerPlan &fixed_plan : fixed_plans) {
                    interval.sublayer_height = std::min(interval.sublayer_height, fixed_plan.flow_height);
                    plans.emplace_back(std::move(fixed_plan));
                    ++interval.sublayer_count;
                    ++total_generated_sublayer_cnt;
                    ++split_passes_with_painted_masks;
                    interval_has_split_painted_masks = true;
                }
                for (size_t row_idx = 0; row_idx < row_seen_sequence_in_interval.size(); ++row_idx)
                    if (row_seen_sequence_in_interval[row_idx] != 0 &&
                        row_uses_direct_multicolor_solver[row_idx] == 0)
                        ++row_layer_cycle_index[row_idx];
            }
            if (!interval_has_split_painted_masks)
                ++split_intervals_without_painted_masks;
        } else {
            // Non-split interval: single pass.
            if (interval.has_mixed_paint)
                ++non_split_mixed_intervals;

            SubLayerPlan plan;
            plan.layer_id       = layer_id;
            plan.pass_index     = 0;
            plan.split_interval = false;
            plan.z_lo           = interval.z_lo;
            plan.z_hi           = interval.z_hi;
            plan.print_z        = interval.z_hi;
            plan.flow_height    = interval.base_height;
            plan.dependency_order = 0;
            plan.base_masks     = base_masks;
            plan.painted_masks_by_extruder.assign(num_physical, ExPolygons());
            plan.fixed_painted_masks_by_extruder.assign(num_physical, ExPolygons());
            std::vector<uint8_t> row_seen_in_interval(mixed_rows.size(), uint8_t(0));

            for (size_t row_idx = 0; row_idx < row_state_masks.size(); ++row_idx) {
                const ExPolygons &state_masks = row_state_masks[row_idx];
                if (state_masks.empty())
                    continue;
                const unsigned int state_id = row_state_ids[row_idx];
                if (state_id == 0)
                    continue;
                const MixedFilament &mixed_row = mixed_rows[row_idx];
                if (!local_z_eligible_mixed_row(mixed_row))
                    continue;
                row_seen_in_interval[row_idx] = uint8_t(1);
                ++forced_height_resolve_calls;

                unsigned int target_extruder = 0;
                if (row_uses_direct_multicolor_solver[row_idx] != 0) {
                    const std::vector<unsigned int> direct_sequence =
                        build_local_z_direct_multicolor_sequence(row_direct_component_ids[row_idx],
                                                                 row_direct_component_weights[row_idx],
                                                                 std::vector<double>{ interval.base_height },
                                                                 row_direct_component_error_mm[row_idx]);
                    if (!direct_sequence.empty())
                        target_extruder = direct_sequence.front();
                } else {
                    const int resolve_cadence_index = row_uses_layer_cycle_pair[row_idx] != 0
                        ? row_layer_cycle_index[row_idx]
                        : row_cadence_index[row_idx];
                    target_extruder =
                        mixed_mgr.resolve(state_id, num_physical,
                                          resolve_cadence_index,
                                          float(plan.print_z),
                                          float(plan.flow_height),
                                          force_height_resolve);
                }
                if (target_extruder == 0 || target_extruder > num_physical) {
                    ++forced_height_resolve_invalid_target;
                    continue;
                }
                append(plan.painted_masks_by_extruder[target_extruder - 1], state_masks);
            }
            for (size_t extruder_idx = 0; extruder_idx < fixed_state_masks_by_extruder.size(); ++extruder_idx)
                if (!fixed_state_masks_by_extruder[extruder_idx].empty())
                    append(plan.fixed_painted_masks_by_extruder[extruder_idx], fixed_state_masks_by_extruder[extruder_idx]);
            for (ExPolygons &masks : plan.painted_masks_by_extruder)
                if (masks.size() > 1)
                    masks = union_ex(masks);
            for (ExPolygons &masks : plan.fixed_painted_masks_by_extruder)
                if (masks.size() > 1)
                    masks = union_ex(masks);

            plans.emplace_back(std::move(plan));
            interval.sublayer_count = 1;
            ++total_generated_sublayer_cnt;

            for (size_t mixed_idx = 0; mixed_idx < row_seen_in_interval.size(); ++mixed_idx)
                if (row_seen_in_interval[mixed_idx] != 0 &&
                    row_uses_direct_multicolor_solver[mixed_idx] == 0)
                    (row_uses_layer_cycle_pair[mixed_idx] != 0
                        ? row_layer_cycle_index[mixed_idx]
                        : row_cadence_index[mixed_idx])++;
        }

        if (interval.has_mixed_paint) {
            BOOST_LOG_TRIVIAL(debug) << "Local-Z interval"
                                     << " object=" << object_name
                                     << " layer_id=" << layer_id
                                     << " base_height=" << interval.base_height
                                     << " split=" << split_interval
                                     << " active_mixed_rows=" << active_mixed_rows
                                     << " mixed_states=" << mixed_state_count;
        }

        row_active_prev_layer = row_active_this_layer;
        intervals.emplace_back(std::move(interval));
    }

    if (!intervals.empty() && !plans.empty()) {
        print_object.set_local_z_plan(std::move(intervals), std::move(plans));
        BOOST_LOG_TRIVIAL(warning) << "Local-Z plan built"
                                   << " object=" << object_name
                                   << " mixed_intervals=" << mixed_intervals
                                   << " split_intervals=" << split_intervals
                                   << " non_split_mixed_intervals=" << non_split_mixed_intervals
                                   << " split_intervals_without_painted_masks=" << split_intervals_without_painted_masks
                                   << " sublayer_passes=" << total_generated_sublayer_cnt
                                   << " split_passes_total=" << split_passes_total
                                   << " split_passes_with_painted_masks=" << split_passes_with_painted_masks
                                   << " alternating_height_intervals=" << alternating_height_intervals
                                   << " shared_multi_row_fallback_intervals=" << shared_multi_row_fallback_intervals
                                   << " strict_ab_assignments=" << strict_ab_assignments
                                   << " mixed_state_layers=" << total_mixed_state_layers
                                   << " forced_height_resolve_calls=" << forced_height_resolve_calls
                                   << " forced_height_resolve_invalid_target=" << forced_height_resolve_invalid_target
                                   << " mixed_lower=" << mixed_lower
                                   << " mixed_upper=" << mixed_upper
                                   << " preferred_a=" << preferred_a
                                   << " preferred_b=" << preferred_b;
    } else {
        BOOST_LOG_TRIVIAL(warning) << "Local-Z plan empty after build"
                                   << " object=" << object_name
                                   << " intervals=" << intervals.size()
                                   << " plans=" << plans.size()
                                   << " mixed_intervals=" << mixed_intervals;
    }
}

} // namespace Slic3r
