#include "clipper/clipper_z.hpp"

#include "ClipperUtils.hpp"
#include "EdgeGrid.hpp"
#include "Layer.hpp"
#include "Print.hpp"
#include "ShortestPath.hpp"
#include "libslic3r.h"
#include "PrintConfig.hpp"
#include "MaterialType.hpp"
#include "Model.hpp"
#include <algorithm>
#include <numeric>
#include <unordered_set>
#include <tbb/parallel_for.h>

#include <boost/log/trivial.hpp>

#ifndef NDEBUG
    // #define BRIM_DEBUG_TO_SVG
#endif

#if defined(BRIM_DEBUG_TO_SVG)
    #include "SVG.hpp"
#endif

namespace Slic3r {

static void append_and_translate(ExPolygons &dst, const ExPolygons &src, const PrintInstance &instance) {
    size_t dst_idx = dst.size();
    expolygons_append(dst, src);

    Point instance_shift = instance.shift_without_plate_offset();
    for (; dst_idx < dst.size(); ++dst_idx)
        dst[dst_idx].translate(instance_shift);
}
// BBS: generate brim area by objs
static void append_and_translate(ExPolygons& dst, const ExPolygons& src,
    const PrintInstance& instance, const Print& print, std::map<ObjectID, ExPolygons>& brimAreaMap) {
    ExPolygons srcShifted = src;
    Point instance_shift = instance.shift_without_plate_offset();
    for (size_t src_idx = 0; src_idx < srcShifted.size(); ++src_idx)
        srcShifted[src_idx].translate(instance_shift);
    srcShifted = diff_ex(srcShifted, dst);
    //expolygons_append(dst, temp2);
    expolygons_append(brimAreaMap[instance.print_object->id()], std::move(srcShifted));
}

static void append_and_translate(Polygons &dst, const Polygons &src, const PrintInstance &instance) {
    size_t dst_idx = dst.size();
    polygons_append(dst, src);
    Point instance_shift = instance.shift_without_plate_offset();
    for (; dst_idx < dst.size(); ++dst_idx)
        dst[dst_idx].translate(instance_shift);
}

static float max_brim_width(const ConstPrintObjectPtrsAdaptor &objects)
{
    assert(!objects.empty());
    return float(std::accumulate(objects.begin(), objects.end(), 0.,
                                 [](double partial_result, const PrintObject *object) {
                                     return std::max(partial_result, object->config().brim_type == btNoBrim ? 0. : object->config().brim_width.value);
                                 }));
}

// Returns ExPolygons of the bottom layer of the print object after elephant foot compensation.
static ExPolygons get_print_object_bottom_layer_expolygons(const PrintObject &print_object)
{
    ExPolygons ex_polygons;
    for (LayerRegion *region : print_object.layers().front()->regions())
        Slic3r::append(ex_polygons, closing_ex(region->slices.surfaces, float(SCALED_EPSILON)));
    return ex_polygons;
}

// Returns ExPolygons of bottom layer for every print object in Print after elephant foot compensation.
static std::vector<ExPolygons> get_print_bottom_layers_expolygons(const Print &print)
{
    std::vector<ExPolygons> bottom_layers_expolygons;
    bottom_layers_expolygons.reserve(print.objects().size());
    for (const PrintObject *object : print.objects())
        bottom_layers_expolygons.emplace_back(get_print_object_bottom_layer_expolygons(*object));

    return bottom_layers_expolygons;
}

static ConstPrintObjectPtrs get_top_level_objects_with_brim(const Print &print, const std::vector<ExPolygons> &bottom_layers_expolygons)
{
    assert(print.objects().size() == bottom_layers_expolygons.size());
    Polygons             islands;
    ConstPrintObjectPtrs island_to_object;
    for(size_t print_object_idx = 0; print_object_idx < print.objects().size(); ++print_object_idx) {
        const PrintObject *object = print.objects()[print_object_idx];
        Polygons islands_object;
        islands_object.reserve(bottom_layers_expolygons[print_object_idx].size());
        for (const ExPolygon &ex_poly : bottom_layers_expolygons[print_object_idx])
            islands_object.emplace_back(ex_poly.contour);

        islands.reserve(islands.size() + object->instances().size() * islands_object.size());
        for (const PrintInstance& instance : object->instances()) {
            Point instance_shift = instance.shift_without_plate_offset();
            for (Polygon& poly : islands_object) {
                islands.emplace_back(poly);
                islands.back().translate(instance_shift);
                island_to_object.emplace_back(object);
            }
        }
    }
    assert(islands.size() == island_to_object.size());

    ClipperLib_Z::Paths islands_clip;
    islands_clip.reserve(islands.size());
    for (const Polygon &poly : islands) {
        islands_clip.emplace_back();
        ClipperLib_Z::Path &island_clip = islands_clip.back();
        island_clip.reserve(poly.points.size());
        int island_idx = int(&poly - &islands.front());
        // The Z coordinate carries index of the island used to get the pointer to the object.
        for (const Point &pt : poly.points)
            island_clip.emplace_back(pt.x(), pt.y(), island_idx + 1);
    }

    // Init Clipper
    ClipperLib_Z::Clipper clipper;
    // Assign the maximum Z from four points. This values is valid index of the island
    clipper.ZFillFunction([](const ClipperLib_Z::IntPoint &e1bot, const ClipperLib_Z::IntPoint &e1top, const ClipperLib_Z::IntPoint &e2bot,
                             const ClipperLib_Z::IntPoint &e2top, ClipperLib_Z::IntPoint &pt) {
        pt.z() = std::max(std::max(e1bot.z(), e1top.z()), std::max(e2bot.z(), e2top.z()));
    });
    // Add islands
    clipper.AddPaths(islands_clip, ClipperLib_Z::ptSubject, true);
    // Execute union operation to construct polytree
    ClipperLib_Z::PolyTree islands_polytree;
    //FIXME likely pftNonZero or ptfPositive would be better. Why are we using ptfEvenOdd for Unions?
    clipper.Execute(ClipperLib_Z::ctUnion, islands_polytree, ClipperLib_Z::pftEvenOdd, ClipperLib_Z::pftEvenOdd);

    std::unordered_set<size_t> processed_objects_idx;
    ConstPrintObjectPtrs       top_level_objects_with_brim;
    for (int i = 0; i < islands_polytree.ChildCount(); ++i) {
        for (const ClipperLib_Z::IntPoint &point : islands_polytree.Childs[i]->Contour) {
            if (point.z() != 0 && processed_objects_idx.find(island_to_object[point.z() - 1]->id().id) == processed_objects_idx.end()) {
                top_level_objects_with_brim.emplace_back(island_to_object[point.z() - 1]);
                processed_objects_idx.insert(island_to_object[point.z() - 1]->id().id);
            }
        }
    }
    return top_level_objects_with_brim;
}

static Polygons top_level_outer_brim_islands(const ConstPrintObjectPtrs &top_level_objects_with_brim, const double scaled_resolution)
{
    Polygons islands;
    for (const PrintObject *object : top_level_objects_with_brim) {
        if (!object->has_brim())
            continue;

        //FIXME how about the brim type?
        auto     brim_object_gap = float(scale_(object->config().brim_object_gap.value));
        Polygons islands_object;
        for (const ExPolygon &ex_poly : get_print_object_bottom_layer_expolygons(*object)) {
            Polygons contour_offset = offset(ex_poly.contour, brim_object_gap, ClipperLib::jtSquare);
            for (Polygon &poly : contour_offset)
                poly.douglas_peucker(scaled_resolution);

            polygons_append(islands_object, std::move(contour_offset));
        }

        if (!object->support_layers().empty()) {
            for (const Polygon& support_contour : object->support_layers().front()->support_fills.polygons_covered_by_spacing()) {
                Polygons contour_offset = offset(support_contour, brim_object_gap, ClipperLib::jtSquare);
                for (Polygon& poly : contour_offset)
                    poly.douglas_peucker(scaled_resolution);

                polygons_append(islands_object, std::move(contour_offset));
            }
        }

        for (const PrintInstance &instance : object->instances())
            append_and_translate(islands, islands_object, instance);
    }
    return islands;
}

static ExPolygons top_level_outer_brim_area(const Print                   &print,
                                            const ConstPrintObjectPtrs    &top_level_objects_with_brim,
                                            const std::vector<ExPolygons> &bottom_layers_expolygons,
                                            const float                    no_brim_offset,
                                            // BBS
                                            double& brim_width_max,
                                            std::map<ObjectID,
                                            double>& brim_width_map)
{
    const auto scaled_resolution = scaled<double>(print.config().resolution.value);

    assert(print.objects().size() == bottom_layers_expolygons.size());
    std::unordered_set<size_t> top_level_objects_idx;
    top_level_objects_idx.reserve(top_level_objects_with_brim.size());
    for (const PrintObject *object : top_level_objects_with_brim)
        top_level_objects_idx.insert(object->id().id);

    ExPolygons brim_area;
    ExPolygons no_brim_area;
    brim_width_max = 0;
    for(size_t print_object_idx = 0; print_object_idx < print.objects().size(); ++print_object_idx) {
        const PrintObject *object            = print.objects()[print_object_idx];
        const BrimType     brim_type         = object->config().brim_type.value;
        const float        brim_object_gap   = scale_(object->config().brim_object_gap.value);
        // recording the autoAssigned brimWidth and corresponding objs
        double brimWidthAuto = object->config().brim_width.value;
        double flowWidth = print.brim_flow().scaled_spacing() * SCALING_FACTOR;
        brimWidthAuto = floor(brimWidthAuto / flowWidth / 2) * flowWidth * 2;
        brim_width_map.insert(std::make_pair(object->id(), brimWidthAuto));
        brim_width_max = std::max(brim_width_max, brimWidthAuto);
        const float    brim_width        = scale_(brimWidthAuto);
        const bool         is_top_outer_brim = top_level_objects_idx.find(object->id().id) != top_level_objects_idx.end();

        ExPolygons brim_area_object;
        ExPolygons no_brim_area_object;
        for (const ExPolygon &ex_poly : bottom_layers_expolygons[print_object_idx]) {
            if ((brim_type == BrimType::btOuterOnly || brim_type == BrimType::btOuterAndInner || brim_type == BrimType::btAutoBrim) && is_top_outer_brim)
                append(brim_area_object, diff_ex(offset(ex_poly.contour, brim_width + brim_object_gap, ClipperLib::jtRound, scaled_resolution), offset(ex_poly.contour, brim_object_gap, ClipperLib::jtSquare)));

            // After 7ff76d07684858fd937ef2f5d863f105a10f798e offset and shrink don't work with CW polygons (holes), so let's make it CCW.
            Polygons ex_poly_holes_reversed = ex_poly.holes;
            polygons_reverse(ex_poly_holes_reversed);
            if (brim_type == BrimType::btOuterOnly || brim_type == BrimType::btNoBrim)
                append(no_brim_area_object, shrink_ex(ex_poly_holes_reversed, no_brim_offset, ClipperLib::jtSquare));

            if (brim_type == BrimType::btInnerOnly || brim_type == BrimType::btNoBrim)
                append(no_brim_area_object, diff_ex(offset(ex_poly.contour, no_brim_offset, ClipperLib::jtSquare), ex_poly_holes_reversed));

            if (brim_type != BrimType::btNoBrim)
                append(no_brim_area_object, offset_ex(ExPolygon(ex_poly.contour), brim_object_gap, ClipperLib::jtSquare));

            no_brim_area_object.emplace_back(ex_poly.contour);
        }

        if (!object->support_layers().empty()) {
            for (const Polygon& support_contour : object->support_layers().front()->support_fills.polygons_covered_by_spacing()) {
                if ((brim_type == BrimType::btOuterOnly || brim_type == BrimType::btOuterAndInner || brim_type == BrimType::btAutoBrim) && is_top_outer_brim)
                    append(brim_area_object, diff_ex(offset(support_contour, brim_width + brim_object_gap, ClipperLib::jtRound, scaled_resolution), offset(support_contour, brim_object_gap)));

                if (brim_type != BrimType::btNoBrim)
                    append(no_brim_area_object, offset_ex(ExPolygon(support_contour), brim_object_gap));

                no_brim_area_object.emplace_back(support_contour);
            }
        }

        for (const PrintInstance &instance : object->instances()) {
            append_and_translate(brim_area, brim_area_object, instance);
            append_and_translate(no_brim_area, no_brim_area_object, instance);
        }
    }

    return diff_ex(brim_area, no_brim_area);
}

// BBS: the brims of different objs will not overlapped with each other, and are stored by objs and by extruders
static ExPolygons top_level_outer_brim_area(const Print& print, const ConstPrintObjectPtrs& top_level_objects_with_brim,
    const float no_brim_offset, double& brim_width_max, std::map<ObjectID, double>& brim_width_map,
    std::map<ObjectID, ExPolygons>& brimAreaMap,
    std::map<ObjectID, ExPolygons>& supportBrimAreaMap, std::vector<std::pair<ObjectID, unsigned int>>& objPrintVec)
{
    std::unordered_set<size_t> top_level_objects_idx;
    top_level_objects_idx.reserve(top_level_objects_with_brim.size());
    for (const PrintObject* object : top_level_objects_with_brim)
        top_level_objects_idx.insert(object->id().id);

    unsigned int support_material_extruder = 1;
    if (print.has_support_material()) {
        assert(top_level_objects_with_brim.front()->config().support_filament >= 0);
        if (top_level_objects_with_brim.front()->config().support_filament > 0)
            support_material_extruder = top_level_objects_with_brim.front()->config().support_filament;
    }

    ExPolygons brim_area;
    ExPolygons no_brim_area;
    brim_width_max = 0;
    struct brimWritten {
        bool obj;
        bool sup;
    };
    std::map<ObjectID, brimWritten> brimToWrite;
    for (const auto& objectWithExtruder : objPrintVec)
        brimToWrite.insert({ objectWithExtruder.first, {true,true} });

    for (unsigned int extruderNo : print.extruders()) {
        ++extruderNo;
        for (const auto &objectWithExtruder : objPrintVec) {
            const PrintObject* object = print.get_object(objectWithExtruder.first);
            const BrimType brim_type = object->config().brim_type.value;
            const float    brim_offset = scale_(object->config().brim_object_gap.value);
            // recording the autoAssigned brimWidth and corresponding objs
            double brimWidthAuto = object->config().brim_width.value;
            double flowWidth = print.brim_flow().scaled_spacing() * SCALING_FACTOR;
            brimWidthAuto = floor(brimWidthAuto / flowWidth / 2) * flowWidth * 2;
            brim_width_map.insert(std::make_pair(object->id(), brimWidthAuto));
            brim_width_max = std::max(brim_width_max, brimWidthAuto);
            const float    brim_width = scale_(brimWidthAuto);
            const bool     is_top_outer_brim = top_level_objects_idx.find(object->id().id) != top_level_objects_idx.end();

            ExPolygons nullBrim;
            brimAreaMap.insert(std::make_pair(object->id(), nullBrim));
            ExPolygons brim_area_object;
            ExPolygons brim_area_support;
            ExPolygons no_brim_area_object;
            ExPolygons no_brim_area_support;
            if (objectWithExtruder.second == extruderNo && brimToWrite.at(object->id()).obj) {
                for (const ExPolygon& ex_poly : object->layers().front()->lslices) {
                    if ((brim_type == BrimType::btOuterOnly || brim_type == BrimType::btOuterAndInner || brim_type == BrimType::btAutoBrim) && is_top_outer_brim) {
                        append(brim_area_object, diff_ex(offset_ex(ex_poly.contour, brim_width + brim_offset, jtRound, SCALED_RESOLUTION),
                            offset_ex(ex_poly.contour, brim_offset)));
                    }
                    if (brim_type == BrimType::btOuterOnly || brim_type == BrimType::btNoBrim)
                        append(no_brim_area_object, offset_ex(ex_poly.holes, -no_brim_offset));

                    if (brim_type == BrimType::btInnerOnly || brim_type == BrimType::btNoBrim)
                        append(no_brim_area_object, diff_ex(offset(ex_poly.contour, no_brim_offset), ex_poly.holes));

                    if (brim_type != BrimType::btNoBrim)
                        append(no_brim_area_object, offset_ex(ExPolygon(ex_poly.contour), brim_offset));

                    no_brim_area_object.emplace_back(ex_poly.contour);
                }
                brimToWrite.at(object->id()).obj = false;
                for (const PrintInstance& instance : object->instances()) {
                    if (!brim_area_object.empty())
                        append_and_translate(brim_area, brim_area_object, instance, print, brimAreaMap);
                    append_and_translate(no_brim_area, no_brim_area_object, instance);
                }
                if (brimAreaMap.find(object->id()) != brimAreaMap.end())
                    expolygons_append(brim_area, brimAreaMap[object->id()]);
            }
            if (support_material_extruder == extruderNo && brimToWrite.at(object->id()).sup) {
                if (!object->support_layers().empty()) {
                    for (const Polygon& support_contour : object->support_layers().front()->support_fills.polygons_covered_by_spacing()) {
                        //BBS: no brim offset for supports
                        if ((brim_type == BrimType::btOuterOnly || brim_type == BrimType::btOuterAndInner || brim_type == BrimType::btAutoBrim) && is_top_outer_brim)
                            append(brim_area_support, diff_ex(offset(support_contour, brim_width, jtRound, SCALED_RESOLUTION), offset(support_contour, 0)));

                        if (brim_type != BrimType::btNoBrim)
                            append(no_brim_area_support, offset_ex(support_contour, 0));

                        no_brim_area_support.emplace_back(support_contour);
                    }
                }

                brimToWrite.at(object->id()).sup = false;
                for (const PrintInstance& instance : object->instances()) {
                    if (!brim_area_support.empty())
                        append_and_translate(brim_area, brim_area_support, instance, print, supportBrimAreaMap);
                    append_and_translate(no_brim_area, no_brim_area_support, instance);
                }
                if (supportBrimAreaMap.find(object->id()) != supportBrimAreaMap.end())
                    expolygons_append(brim_area, supportBrimAreaMap[object->id()]);
            }
        }
    }
    for (const PrintObject* object : print.objects()) {
        if (brimAreaMap.find(object->id()) != brimAreaMap.end())
            brimAreaMap[object->id()] = diff_ex(brimAreaMap[object->id()], no_brim_area);
        if (supportBrimAreaMap.find(object->id()) != supportBrimAreaMap.end())
            supportBrimAreaMap[object->id()] = diff_ex(supportBrimAreaMap[object->id()], no_brim_area);
    }
    return diff_ex(std::move(brim_area), no_brim_area);
}
static ExPolygons inner_brim_area(const Print                   &print,
                                  const ConstPrintObjectPtrs    &top_level_objects_with_brim,
                                  const std::vector<ExPolygons> &bottom_layers_expolygons,
                                  const float                    no_brim_offset)
{
    assert(print.objects().size() == bottom_layers_expolygons.size());
    std::unordered_set<size_t> top_level_objects_idx;
    top_level_objects_idx.reserve(top_level_objects_with_brim.size());
    for (const PrintObject *object : top_level_objects_with_brim)
        top_level_objects_idx.insert(object->id().id);

    ExPolygons brim_area;
    ExPolygons no_brim_area;
    Polygons   holes;
    for(size_t print_object_idx = 0; print_object_idx < print.objects().size(); ++print_object_idx) {
        const PrintObject *object          = print.objects()[print_object_idx];
        const BrimType     brim_type       = object->config().brim_type.value;
        const float        brim_object_gap = scale_(object->config().brim_object_gap.value);
        double flowWidth = print.brim_flow().scaled_spacing() * SCALING_FACTOR;
        const float    brim_width = scale_(floor(object->config().brim_width.value / flowWidth / 2) * flowWidth * 2);
        const bool         top_outer_brim  = top_level_objects_idx.find(object->id().id) != top_level_objects_idx.end();

        ExPolygons brim_area_object;
        ExPolygons no_brim_area_object;
        Polygons   holes_object;
        for (const ExPolygon &ex_poly : bottom_layers_expolygons[print_object_idx]) {
            if (brim_type == BrimType::btOuterOnly || brim_type == BrimType::btOuterAndInner || brim_type == BrimType::btAutoBrim) {
                if (top_outer_brim)
                    no_brim_area_object.emplace_back(ex_poly);
                else
                    append(brim_area_object, diff_ex(offset(ex_poly.contour, brim_width + brim_object_gap, ClipperLib::jtSquare), offset(ex_poly.contour, brim_object_gap, ClipperLib::jtSquare)));
            }

            // After 7ff76d07684858fd937ef2f5d863f105a10f798e offset and shrink don't work with CW polygons (holes), so let's make it CCW.
            Polygons ex_poly_holes_reversed = ex_poly.holes;
            polygons_reverse(ex_poly_holes_reversed);
            if (brim_type == BrimType::btInnerOnly || brim_type == BrimType::btOuterAndInner || brim_type == BrimType::btAutoBrim)
                append(brim_area_object, diff_ex(shrink_ex(ex_poly_holes_reversed, brim_object_gap, ClipperLib::jtSquare), shrink_ex(ex_poly_holes_reversed, brim_width + brim_object_gap, ClipperLib::jtSquare)));

            if (brim_type == BrimType::btInnerOnly || brim_type == BrimType::btNoBrim)
                append(no_brim_area_object, diff_ex(offset(ex_poly.contour, no_brim_offset, ClipperLib::jtSquare), ex_poly_holes_reversed));

            if (brim_type == BrimType::btOuterOnly || brim_type == BrimType::btNoBrim)
                append(no_brim_area_object, diff_ex(ExPolygon(ex_poly.contour), shrink_ex(ex_poly_holes_reversed, no_brim_offset, ClipperLib::jtSquare)));

            append(holes_object, ex_poly_holes_reversed);
        }
        append(no_brim_area_object, offset_ex(bottom_layers_expolygons[print_object_idx], brim_object_gap, ClipperLib::jtSquare));

        for (const PrintInstance &instance : object->instances()) {
            append_and_translate(brim_area, brim_area_object, instance);
            append_and_translate(no_brim_area, no_brim_area_object, instance);
            append_and_translate(holes, holes_object, instance);
        }
    }

    return diff_ex(intersection_ex(to_polygons(std::move(brim_area)), holes), no_brim_area);
}

// BBS: the brims of different objs will not overlapped with each other, and are stored by objs and by extruders
static ExPolygons inner_brim_area(const Print& print, const ConstPrintObjectPtrs& top_level_objects_with_brim,
    const float no_brim_offset, std::map<ObjectID, ExPolygons>& brimAreaMap,
    std::map<ObjectID, ExPolygons>& supportBrimAreaMap,
    std::vector<std::pair<ObjectID, unsigned int>>& objPrintVec)
{
    std::unordered_set<size_t> top_level_objects_idx;
    top_level_objects_idx.reserve(top_level_objects_with_brim.size());
    for (const PrintObject* object : top_level_objects_with_brim)
        top_level_objects_idx.insert(object->id().id);

    unsigned int support_material_extruder = 1;
    if (print.has_support_material()) {
        assert(top_level_objects_with_brim.front()->config().support_filament >= 0);
        if (top_level_objects_with_brim.front()->config().support_filament > 0)
            support_material_extruder = top_level_objects_with_brim.front()->config().support_filament;
    }

    ExPolygons brim_area;
    ExPolygons no_brim_area;
    Polygons   holes;
    Polygon    bedShape(get_bed_shape(print.config()));
    holes.emplace_back(get_bed_shape(print.config()));
    std::map<ObjectID, ExPolygons> innerBrimAreaMap;
    std::map<ObjectID, ExPolygons> innerSupportBrimAreaMap;

    struct brimWritten {
        bool obj;
        bool sup;
    };
    std::map<ObjectID, brimWritten> brimToWrite;
    for (const auto& objectWithExtruder : objPrintVec)
        brimToWrite.insert({ objectWithExtruder.first, {true,true} });


    for (unsigned int extruderNo : print.extruders()) {
        ++extruderNo;
        for (const auto& objectWithExtruder : objPrintVec) {
            const PrintObject* object = print.get_object(objectWithExtruder.first);
            const BrimType brim_type = object->config().brim_type.value;
            const float    brim_offset = scale_(object->config().brim_object_gap.value);
            double flowWidth = print.brim_flow().scaled_spacing() * SCALING_FACTOR;
            const float    brim_width = scale_(floor(object->config().brim_width.value / flowWidth / 2) * flowWidth * 2);
            const bool     top_outer_brim = top_level_objects_idx.find(object->id().id) != top_level_objects_idx.end();

            ExPolygons brim_area_object;
            ExPolygons no_brim_area_object;
            ExPolygons brim_area_support;
            ExPolygons no_brim_area_support;
            Polygons   holes_object;
            Polygons   holes_support;
            if (objectWithExtruder.second == extruderNo && brimToWrite.at(object->id()).obj) {
                for (const ExPolygon& ex_poly : object->layers().front()->lslices) {
                    if (brim_type == BrimType::btOuterOnly || brim_type == BrimType::btOuterAndInner || brim_type == BrimType::btAutoBrim) {
                         if (top_outer_brim)
                            no_brim_area_object.emplace_back(ex_poly);
                        else
                            append(brim_area_object, diff_ex(offset_ex(ex_poly.contour, brim_width + brim_offset, jtRound, SCALED_RESOLUTION), offset_ex(ex_poly.contour, brim_offset)));
                    }
                    if (brim_type == BrimType::btInnerOnly || brim_type == BrimType::btOuterAndInner)
                        append(brim_area_object, diff_ex(offset_ex(ex_poly.holes, -brim_offset), offset_ex(ex_poly.holes, -brim_width - brim_offset)));
                    if (brim_type == BrimType::btInnerOnly || brim_type == BrimType::btNoBrim)
                        append(no_brim_area_object, diff_ex(offset(ex_poly.contour, no_brim_offset), ex_poly.holes));
                    if (brim_type == BrimType::btOuterOnly || brim_type == BrimType::btNoBrim)
                        append(no_brim_area_object, offset_ex(ex_poly.holes, -no_brim_offset));
                    append(holes_object, ex_poly.holes);
                }
                append(no_brim_area_object, offset_ex(object->layers().front()->lslices, brim_offset));
                brimToWrite.at(object->id()).obj = false;
                for (const PrintInstance& instance : object->instances()) {
                    if (!brim_area_object.empty())
                        append_and_translate(brim_area, brim_area_object, instance, print, innerBrimAreaMap);
                    append_and_translate(no_brim_area, no_brim_area_object, instance);
                    append_and_translate(holes, holes_object, instance);
                }
                if (innerBrimAreaMap.find(object->id()) != innerBrimAreaMap.end())
                    expolygons_append(brim_area, innerBrimAreaMap[object->id()]);
            }
            if (support_material_extruder == extruderNo && brimToWrite.at(object->id()).sup) {
                if (!object->support_layers().empty()) {
                    for (const Polygon& support_contour : object->support_layers().front()->support_fills.polygons_covered_by_spacing()) {
                        if (brim_type == BrimType::btOuterOnly || brim_type == BrimType::btOuterAndInner || brim_type == BrimType::btAutoBrim) {
                            if (!top_outer_brim)
                                append(brim_area_support, diff_ex(offset_ex(support_contour, brim_width + brim_offset, jtRound, SCALED_RESOLUTION), offset_ex(support_contour, brim_offset)));
                        }
                        if (brim_type != BrimType::btNoBrim)
                            append(no_brim_area_support, offset_ex(support_contour, 0));
                        no_brim_area_support.emplace_back(support_contour);
                    }
                }
            }
            brimToWrite.at(object->id()).sup = false;
            for (const PrintInstance& instance : object->instances()) {
                if (!brim_area_support.empty())
                    append_and_translate(brim_area, brim_area_support, instance, print, innerSupportBrimAreaMap);
                append_and_translate(no_brim_area, no_brim_area_support, instance);
                append_and_translate(holes, holes_support, instance);
            }
            if (innerSupportBrimAreaMap.find(object->id()) != innerSupportBrimAreaMap.end())
                expolygons_append(brim_area, innerSupportBrimAreaMap[object->id()]);
        }
    }
    for (const PrintObject* object : print.objects()) {
        if (innerBrimAreaMap.find(object->id()) != innerBrimAreaMap.end()) {
            innerBrimAreaMap[object->id()] = intersection_ex(to_polygons(innerBrimAreaMap[object->id()]), holes);
            append(brimAreaMap[object->id()], innerBrimAreaMap[object->id()]);
        }
        if (innerSupportBrimAreaMap.find(object->id()) != innerSupportBrimAreaMap.end()) {
            innerSupportBrimAreaMap[object->id()] = intersection_ex(to_polygons(innerSupportBrimAreaMap[object->id()]), holes);
            append(supportBrimAreaMap[object->id()], innerSupportBrimAreaMap[object->id()]);
        }
    }
    for (const PrintObject* object : print.objects()) {
        if (brimAreaMap.find(object->id()) != brimAreaMap.end())
            brimAreaMap[object->id()] = diff_ex(brimAreaMap[object->id()], no_brim_area);
        if (supportBrimAreaMap.find(object->id()) != supportBrimAreaMap.end())
            supportBrimAreaMap[object->id()] = diff_ex(supportBrimAreaMap[object->id()], no_brim_area);
    }
    brim_area = intersection_ex(to_polygons(brim_area), holes);
    append(no_brim_area, brim_area);
    return no_brim_area;
}

//BBS maximum temperature difference from print object class
double getTemperatureFromExtruder(const PrintObject* printObject) {
    auto print = printObject->print();
    std::vector<size_t> extrudersFirstLayer;
    auto firstLayerRegions = printObject->layers().front()->regions();
    if (!firstLayerRegions.empty()) {
        for (const LayerRegion* regionPtr : firstLayerRegions) {
            if (regionPtr->has_extrusions())
                extrudersFirstLayer.push_back(regionPtr->region().extruder(frExternalPerimeter));
        }
    }

    const PrintConfig& config = print->config();
    int curr_bed_type = config.option("curr_bed_type")->getInt();
    const ConfigOptionInts* bed_temp_1st_layer_opt = config.option<ConfigOptionInts>(get_bed_temp_1st_layer_key((BedType)curr_bed_type));

    double maxDeltaTemp = 0;
    for (auto extruderID : extrudersFirstLayer) {
        int bedTemp = bed_temp_1st_layer_opt->get_at(extruderID - 1);
        if (bedTemp > maxDeltaTemp)
            maxDeltaTemp = bedTemp;
    }

    return maxDeltaTemp;
}
//BBS adhesion coefficients from print object class
double getadhesionCoeff(const PrintObject* printObject)
{
    auto& insts = printObject->instances();
    auto objectVolumes = insts[0].model_instance->get_object()->volumes;

    auto print = printObject->print();
    std::vector<size_t> extrudersFirstLayer;
    auto firstLayerRegions = printObject->layers().front()->regions();
    if (!firstLayerRegions.empty()) {
        for (const LayerRegion* regionPtr : firstLayerRegions) {
            if (regionPtr->has_extrusions())
                extrudersFirstLayer.push_back(regionPtr->region().extruder(frExternalPerimeter));
        }
    }
    double adhesionCoeff = 1;
    for (const ModelVolume* modelVolume : objectVolumes) {
        for (auto iter = extrudersFirstLayer.begin(); iter != extrudersFirstLayer.end(); iter++) {
            if (modelVolume->extruder_id() == *iter) {
                if (Model::extruderParamsMap.find(modelVolume->extruder_id()) != Model::extruderParamsMap.end()) {
                    std::string filament_type = Model::extruderParamsMap.at(modelVolume->extruder_id()).materialName;
                    double adhesion_coefficient = 1.0; // Default value
                    MaterialType::get_adhesion_coefficient(filament_type, adhesion_coefficient);
                    adhesionCoeff = adhesion_coefficient;
                }
            }
        }
    }

    return adhesionCoeff;
   /*
   def->enum_values.push_back("PLA");
   def->enum_values.push_back("PET");
   def->enum_values.push_back("ABS");
   def->enum_values.push_back("ASA");
   def->enum_values.push_back("TPU");//BBS
   def->enum_values.push_back("FLEX");
   def->enum_values.push_back("HIPS");
   def->enum_values.push_back("EDGE");
   def->enum_values.push_back("NGEN");
   def->enum_values.push_back("NYLON");
   def->enum_values.push_back("PVA");
   def->enum_values.push_back("PC");
   def->enum_values.push_back("PP");
   def->enum_values.push_back("PEI");
   def->enum_values.push_back("PEEK");
   def->enum_values.push_back("PEKK");
   def->enum_values.push_back("POM");
   def->enum_values.push_back("PSU");
   def->enum_values.push_back("PVDF");
   def->enum_values.push_back("SCAFF");
   */
}

// BBS: second moment of area of a polygon
bool compSecondMoment(Polygon poly, Vec2d& sm)
{
    if (poly.is_clockwise())
        poly.make_counter_clockwise();

    sm = Vec2d(0., 0.);
    if (poly.points.size() >= 3) {
        Vec2d p1 = poly.points.back().cast<double>();
        for (const Point& p : poly.points) {
            Vec2d p2 = p.cast<double>();
            double a = cross2(p1, p2);

            sm += Vec2d((p1.y() * p1.y() + p1.y() * p2.y() + p2.y() * p2.y()), (p1.x() * p1.x() + p1.x() * p2.x() + p2.x() * p2.x())) * a / 12;
            p1 = p2;
        }
        return true;
    }
    return false;
}
// BBS: properties of an expolygon
struct ExPolyProp
{
    double aera = 0;
    Vec2d  centroid;
    Vec2d  secondMomentOfAreaRespectToCentroid;

};
// BBS: second moment of area of an expolyon
bool compSecondMoment(const ExPolygon& expoly, ExPolyProp& expolyProp)
{
    double aera = expoly.contour.area();
    Vec2d cent = expoly.contour.centroid().cast<double>() * aera;
    Vec2d sm;
    if (!compSecondMoment(expoly.contour, sm))
        return false;

    for (auto& hole : expoly.holes) {
        double a = hole.area();
        aera += hole.area();
        cent += hole.centroid().cast<double>() * a;
        Vec2d smh;
        if (compSecondMoment(hole, smh))
            sm += -smh;
    }

    cent = cent / aera;
    sm = sm - Vec2d(cent.y() * cent.y(), cent.x() * cent.x()) * aera;
    expolyProp.aera = aera;
    expolyProp.centroid = cent;
    expolyProp.secondMomentOfAreaRespectToCentroid = sm;
    return true;
}

// BBS: second moment of area of expolygons
bool compSecondMoment(const ExPolygons& expolys, double& smExpolysX, double& smExpolysY)
{
    if (expolys.empty()) return false;
    std::vector<ExPolyProp> props;
    for (const ExPolygon& expoly : expolys) {
        ExPolyProp prop;
        if (compSecondMoment(expoly, prop))
            props.push_back(prop);
    }
    if (props.empty())
        return false;
    double totalArea = 0.;
    Vec2d staticMoment(0., 0.);
    for (const ExPolyProp& prop : props) {
        totalArea += prop.aera;
        staticMoment += prop.centroid * prop.aera;
    }
    double totalCentroidX = staticMoment.x() / totalArea;
    double totalCentroidY = staticMoment.y() / totalArea;

    smExpolysX = 0;
    smExpolysY = 0;
    for (const ExPolyProp& prop : props) {
        double deltaX = prop.centroid.x() - totalCentroidX;
        double deltaY = prop.centroid.y() - totalCentroidY;
        smExpolysX += prop.secondMomentOfAreaRespectToCentroid.x() + prop.aera * deltaY * deltaY;
        smExpolysY += prop.secondMomentOfAreaRespectToCentroid.y() + prop.aera * deltaX * deltaX;
    }

    return true;
}



//BBS: config brimwidth by volumes
double configBrimWidthByVolumes(double deltaT, double adhesion, double maxSpeed, const ModelVolume* modelVolumePtr, const ExPolygons& expolys)
{
    // height of a volume
    double height = 0;
    if (modelVolumePtr->is_model_part()) {
        auto rawBoundingbox = modelVolumePtr->mesh().transformed_bounding_box(modelVolumePtr->get_matrix());
        auto bbox = modelVolumePtr->get_object()->instances.front()->transform_bounding_box(rawBoundingbox);
        auto bbox_size = bbox.size();
        height = bbox_size(2);
    }

    // sencond moment of the expolygons of the first layer of the volume
    double Ixx = -1.e30, Iyy = -1.e30;
    if (!expolys.empty()) {
        if (!compSecondMoment(expolys, Ixx, Iyy))
            Ixx = Iyy = -1.e30;
    }
    Ixx = Ixx * SCALING_FACTOR * SCALING_FACTOR * SCALING_FACTOR * SCALING_FACTOR;
    Iyy = Iyy * SCALING_FACTOR * SCALING_FACTOR * SCALING_FACTOR * SCALING_FACTOR;

    // bounding box of the expolygons of the first layer of the volume
    BoundingBox bbox2;
    for (const auto& expoly : expolys)
        bbox2.merge(get_extents(expoly.contour));
    const double& bboxX = bbox2.size()(0);
    const double& bboxY = bbox2.size()(1);
    double thermalLength = sqrt(bboxX * bboxX + bboxY * bboxY) * SCALING_FACTOR;
    double thermalLengthRef = Model::getThermalLength(modelVolumePtr);

    double height_to_area = std::max(height / Ixx * (bbox2.size()(1) * SCALING_FACTOR), height / Iyy * (bbox2.size()(0) * SCALING_FACTOR));
    double brim_width = adhesion * std::min(std::min(std::max(height_to_area * maxSpeed / 24, thermalLength * 8. / thermalLengthRef * std::min(height, 30.) / 30.), 18.), 1.5 * thermalLength);
    // small brims are omitted
    if (brim_width < 5 && brim_width < 1.5 * thermalLength)
        brim_width = 0;
    // large brims are omitted
    if (brim_width > 18) brim_width = 18.;

    return brim_width;
}

//BBS: config brimwidth by group of volumes
double configBrimWidthByVolumeGroups(double adhesion, double maxSpeed, const std::vector<ModelVolume*> modelVolumePtrs, const ExPolygons& expolys, double &groupHeight)
{
    // height of a group of volumes
    double height = 0;
    BoundingBoxf3 mergedBbx;
    for (const auto& modelVolumePtr : modelVolumePtrs) {
        if (modelVolumePtr->is_model_part()) {
            Slic3r::Transform3d t;
            if (modelVolumePtr->get_object()->instances.size() > 0)
                t = modelVolumePtr->get_object()->instances.front()->get_matrix() * modelVolumePtr->get_matrix();
            else
                t = modelVolumePtr->get_matrix();
            auto bbox = modelVolumePtr->mesh().transformed_bounding_box(t);
            mergedBbx.merge(bbox);
        }
    }
    auto bbox_size = mergedBbx.size();
    height = bbox_size(2);
    groupHeight = height;
    // second moment of the expolygons of the first layer of the volume group
    double Ixx = -1.e30, Iyy = -1.e30;
    if (!expolys.empty()) {
        if (!compSecondMoment(expolys, Ixx, Iyy))
            Ixx = Iyy = -1.e30;
    }
    Ixx = Ixx * SCALING_FACTOR * SCALING_FACTOR * SCALING_FACTOR * SCALING_FACTOR;
    Iyy = Iyy * SCALING_FACTOR * SCALING_FACTOR * SCALING_FACTOR * SCALING_FACTOR;

    // bounding box of the expolygons of the first layer of the volume
    BoundingBox bbox2;
    for (const auto& expoly : expolys)
        bbox2.merge(get_extents(expoly.contour));
    const double& bboxX = bbox2.size()(0);
    const double& bboxY = bbox2.size()(1);
    double thermalLength = sqrt(bboxX * bboxX + bboxY * bboxY) * SCALING_FACTOR;
    double thermalLengthRef = Model::getThermalLength(modelVolumePtrs);

    double height_to_area = std::max(height / Ixx * (bbox2.size()(1) * SCALING_FACTOR), height / Iyy * (bbox2.size()(0) * SCALING_FACTOR)) * height / 1920;
    double brim_width = adhesion * std::min(std::min(std::max(height_to_area * maxSpeed, thermalLength * 8. / thermalLengthRef * std::min(height, 30.) / 30.), 18.), 1.5 * thermalLength);
    // small brims are omitted
    if (brim_width < 5 && brim_width < 1.5 * thermalLength)
        brim_width = 0;
    // large brims are omitted
    if (brim_width > 18) brim_width = 18.;

    return brim_width;
}

// Generate ears
// Ported from SuperSlicer: https://github.com/supermerill/SuperSlicer/blob/45d0532845b63cd5cefe7de7dc4ef0e0ed7e030a/src/libslic3r/Brim.cpp#L1116
static ExPolygons make_brim_ears_auto(const ExPolygons& obj_expoly, coord_t size_ear, coord_t ear_detection_length,
                                      coordf_t brim_ears_max_angle, bool is_outer_brim) {
    ExPolygons mouse_ears_ex;
    if (size_ear <= 0) {
        return mouse_ears_ex;
    }
    // Detect places to put ears
    const coordf_t angle_threshold = (180 - brim_ears_max_angle) * PI / 180.0;
    Points pt_ears;
    for (const ExPolygon &poly : obj_expoly) {
        Polygon decimated_polygon = poly.contour;
        if (ear_detection_length > 0) {
            // decimate polygon
            Points points = poly.contour.points;
            points.push_back(points.front());
            points = MultiPoint::_douglas_peucker(points, ear_detection_length);
            if (points.size() > 4) { // don't decimate if it's going to be below 4 points, as it's surely enough to fill everything anyway
                points.erase(points.end() - 1);
                decimated_polygon.points = points;
            }
        }

        append(pt_ears, is_outer_brim ? decimated_polygon.convex_points(angle_threshold)
                                      : decimated_polygon.concave_points(angle_threshold));
    }

    // Then add ears
    // create ear pattern
    Polygon point_round;
    for (size_t i = 0; i < POLY_SIDE_COUNT; i++) {
        double angle = (2.0 * PI * i) / POLY_SIDE_COUNT;
        point_round.points.emplace_back(size_ear * cos(angle), size_ear * sin(angle));
    }

    // create ears
    for (Point &pt : pt_ears) {
        mouse_ears_ex.emplace_back();
        mouse_ears_ex.back().contour = point_round;
        mouse_ears_ex.back().contour.translate(pt);
    }

    return mouse_ears_ex;
}

static ExPolygons make_brim_ears(const PrintObject* object, const double& flowWidth, float brim_offset, Flow &flow, bool is_outer_brim)
{
    ExPolygons mouse_ears_ex;
    BrimPoints brim_ear_points = object->model_object()->brim_points;
    if (brim_ear_points.size() <= 0) {
        return mouse_ears_ex;
    }
    const Geometry::Transformation& trsf = object->model_object()->instances[0]->get_transformation();
    Transform3d model_trsf = trsf.get_matrix_no_offset();
    const Point &center_offset = object->center_offset();
    model_trsf = model_trsf.pretranslate(Vec3d(- unscale<double>(center_offset.x()), - unscale<double>(center_offset.y()), 0));
    for (auto &pt : brim_ear_points) {
        Vec3f world_pos = pt.transform(trsf.get_matrix());
        if ( world_pos.z() > 0) continue;
        Polygon point_round;
        float brim_width = floor(scale_(pt.head_front_radius) / flowWidth / 2) * flowWidth * 2;
        if (is_outer_brim) {
            double flowWidthScale = flowWidth / SCALING_FACTOR;
            brim_width = floor(brim_width / flowWidthScale / 2) * flowWidthScale * 2;
        }
        coord_t size_ear = (brim_width - brim_offset - flow.scaled_spacing());
        for (size_t i = 0; i < POLY_SIDE_COUNT; i++) {
            double angle = (2.0 * PI * i) / POLY_SIDE_COUNT;
            point_round.points.emplace_back(size_ear * cos(angle), size_ear * sin(angle));
        }
        mouse_ears_ex.emplace_back();
        mouse_ears_ex.back().contour = point_round;
        Vec3f pos = pt.transform(model_trsf);
        int32_t pt_x = scale_(pos.x());
        int32_t pt_y = scale_(pos.y());
        mouse_ears_ex.back().contour.translate(Point(pt_x, pt_y));
    }
    return mouse_ears_ex;
}

//BBS: create all brims
static ExPolygons outer_inner_brim_area(const Print& print,
    const float no_brim_offset, std::map<ObjectID, ExPolygons>& brimAreaMap,
    std::map<ObjectID, ExPolygons>& supportBrimAreaMap,
    std::vector<std::pair<ObjectID, unsigned int>>& objPrintVec,
    std::vector<unsigned int>& printExtruders)
{
    unsigned int support_material_extruder = printExtruders.front() + 1;
    Flow flow = print.brim_flow();

    ExPolygons brim_area;
    ExPolygons no_brim_area;
    Polygons   holes;

    struct brimWritten {
        bool obj;
        bool sup;
    };
    std::map<ObjectID, brimWritten> brimToWrite;
    for (const auto& objectWithExtruder : objPrintVec)
        brimToWrite.insert({ objectWithExtruder.first, {true,true} });

    ExPolygons objectIslands;
    for (unsigned int extruderNo : printExtruders) {
        ++extruderNo;
        for (const auto& objectWithExtruder : objPrintVec) {
            const PrintObject* object = print.get_object(objectWithExtruder.first);
            const BrimType     brim_type = object->config().brim_type.value;
            float              brim_offset = scale_(object->config().brim_object_gap.value);
            double             flowWidth = print.brim_flow().scaled_spacing() * SCALING_FACTOR;
            float              brim_width = scale_(floor(object->config().brim_width.value / flowWidth / 2) * flowWidth * 2);
            const float        scaled_flow_width = print.brim_flow().scaled_spacing();
            const float        scaled_additional_brim_width = scale_(floor(5 / flowWidth / 2) * flowWidth * 2);
            const float        scaled_half_min_adh_length = scale_(1.1);
            bool               has_brim_auto = object->config().brim_type == btAutoBrim;
            const bool         use_auto_brim_ears = object->config().brim_type == btEar;
            const bool         use_brim_ears = object->config().brim_type == btPainted;
            const bool         has_inner_brim = brim_type == btInnerOnly || brim_type == btOuterAndInner || use_auto_brim_ears || use_brim_ears;
            const bool         has_outer_brim = brim_type == btOuterOnly || brim_type == btOuterAndInner || brim_type == btAutoBrim || use_auto_brim_ears || use_brim_ears;
            coord_t            ear_detection_length = scale_(object->config().brim_ears_detection_length.value);
            coordf_t           brim_ears_max_angle = object->config().brim_ears_max_angle.value;

            ExPolygons         brim_area_object;
            ExPolygons         no_brim_area_object;
            ExPolygons         brim_area_support;
            ExPolygons         no_brim_area_support;
            Polygons           holes_object;
            Polygons           holes_support;
            if (objectWithExtruder.second == extruderNo && brimToWrite.at(object->id()).obj) {
                double             adhesion = getadhesionCoeff(object);
                double             maxSpeed = Model::findMaxSpeed(object->model_object());
                // BBS: brims are generated by volume groups
                for (const auto& volumeGroup : object->firstLayerObjGroups()) {
                    // find volumePtrs included in this group
                    std::vector<ModelVolume*> groupVolumePtrs;
                    for (auto& volumeID : volumeGroup.volume_ids) {
                        ModelVolume* currentModelVolumePtr = nullptr;
                        //BBS: support shared object logic
                        const PrintObject* shared_object = object->get_shared_object();
                        if (!shared_object)
                            shared_object = object;
                        for (auto volumePtr : shared_object->model_object()->volumes) {
                            if (volumePtr->id() == volumeID) {
                                currentModelVolumePtr = volumePtr;
                                break;
                            }
                        }
                        if (currentModelVolumePtr != nullptr) groupVolumePtrs.push_back(currentModelVolumePtr);
                    }
                    if (groupVolumePtrs.empty()) continue;
                    double groupHeight = 0.;
                    // config brim width in auto-brim mode
                    if (has_brim_auto) {
                        double brimWidthRaw = configBrimWidthByVolumeGroups(adhesion, maxSpeed, groupVolumePtrs, volumeGroup.slices, groupHeight);
                        brim_width = scale_(floor(brimWidthRaw / flowWidth / 2) * flowWidth * 2);
                    }
                    for (const ExPolygon& ex_poly : volumeGroup.slices) {
                        // BBS: additional brim width will be added if part's adhesion area is too small and brim is not generated
                        float brim_width_mod;
                        if (brim_width < scale_(5.) && has_brim_auto && groupHeight > 10.) {
                            brim_width_mod = ex_poly.area() / ex_poly.contour.length() < scaled_half_min_adh_length
                                && brim_width < scaled_flow_width ? brim_width + scaled_additional_brim_width : brim_width;
                        }
                        else {
                            brim_width_mod = brim_width;
                        }
                        //BBS: brim width should be limited to the 1.5*boundingboxSize of a single polygon.
                        if (has_brim_auto) {
                            BoundingBox bbox2 = ex_poly.contour.bounding_box();
                            brim_width_mod = std::min(brim_width_mod, float(std::max(bbox2.size()(0), bbox2.size()(1))));
                        }
                        brim_width_mod = floor(brim_width_mod / scaled_flow_width / 2) * scaled_flow_width * 2;

                        Polygons ex_poly_holes_reversed = ex_poly.holes;
                        polygons_reverse(ex_poly_holes_reversed);

                        if (has_outer_brim) {
                            // BBS: inner and outer boundary are offset from the same polygon incase of round off error.
                            auto innerExpoly = offset_ex(ex_poly.contour, brim_offset, jtRound, SCALED_RESOLUTION);
                            ExPolygons outerExpoly;
                            if (use_brim_ears) {
                                outerExpoly = make_brim_ears(object, flowWidth, brim_offset, flow, true);
                                //outerExpoly = offset_ex(outerExpoly, brim_width_mod, jtRound, SCALED_RESOLUTION);
                            } else if (use_auto_brim_ears) {
                                coord_t size_ear = (brim_width_mod - brim_offset - flow.scaled_spacing());
                                outerExpoly = make_brim_ears_auto(innerExpoly, size_ear, ear_detection_length, brim_ears_max_angle, true);
                            }else {
                                outerExpoly = offset_ex(innerExpoly, brim_width_mod, jtRound, SCALED_RESOLUTION);
                            }
                            append(brim_area_object, diff_ex(outerExpoly, innerExpoly));
                        }
                        if (has_inner_brim) {
                            ExPolygons outerExpoly;
                            auto innerExpoly = offset_ex(ex_poly_holes_reversed, -brim_width - brim_offset);
                            if (use_brim_ears) {
                                outerExpoly = make_brim_ears(object, flowWidth, brim_offset, flow, false);
                            } else if (use_auto_brim_ears) {
                                coord_t size_ear = (brim_width - brim_offset - flow.scaled_spacing());
                                outerExpoly = make_brim_ears_auto(offset_ex(ex_poly_holes_reversed, -brim_offset), size_ear, ear_detection_length, brim_ears_max_angle, false);
                            }else {
                                outerExpoly = offset_ex(ex_poly_holes_reversed, -brim_offset);
                            }
                            append(brim_area_object, intersection_ex(diff_ex(outerExpoly, innerExpoly), ex_poly_holes_reversed));
                        }
                        if (!has_inner_brim) {
                            // BBS: brim should be apart from holes
                            append(no_brim_area_object, diff_ex(ex_poly_holes_reversed, offset_ex(ex_poly_holes_reversed, -no_brim_offset)));
                        }
                        if (!has_outer_brim)
                            append(no_brim_area_object, diff_ex(offset(ex_poly.contour, no_brim_offset), ex_poly_holes_reversed));
                        append(holes_object, ex_poly_holes_reversed);
                    }
                }
                auto objectIsland = offset_ex(object->layers().front()->lslices, brim_offset, jtRound, SCALED_RESOLUTION);
                append(no_brim_area_object, objectIsland);

                brimToWrite.at(object->id()).obj = false;
                for (const PrintInstance& instance : object->instances()) {
                    if (!brim_area_object.empty())
                        append_and_translate(brim_area, brim_area_object, instance, print, brimAreaMap);
                    append_and_translate(no_brim_area, no_brim_area_object, instance);
                    append_and_translate(holes, holes_object, instance);
                    append_and_translate(objectIslands, objectIsland, instance);

                }
                if (brimAreaMap.find(object->id()) != brimAreaMap.end())
                    expolygons_append(brim_area, brimAreaMap[object->id()]);
            }
            support_material_extruder = object->config().support_filament;
            if (support_material_extruder == 0 && object->has_support_material()) {
                if (print.config().print_sequence == PrintSequence::ByObject)
                    support_material_extruder = objectWithExtruder.second;
                else
                    support_material_extruder = printExtruders.front() + 1;
            }
            if (support_material_extruder == extruderNo && brimToWrite.at(object->id()).sup) {
                if (!object->support_layers().empty() && object->support_layers().front()->support_type==stInnerNormal) {
                    for (const Polygon& support_contour : object->support_layers().front()->support_fills.polygons_covered_by_spacing()) {
                        // Brim will not be generated for supports
                        /*
                        if (has_outer_brim) {
                            append(brim_area_support, diff_ex(offset_ex(support_contour, brim_width + brim_offset, jtRound, SCALED_RESOLUTION), offset_ex(support_contour, brim_offset)));
                        }
                        if (has_inner_brim || has_outer_brim)
                            append(no_brim_area_support, offset_ex(support_contour, 0));
                        */
                        no_brim_area_support.emplace_back(support_contour);
                    }
                }
                // BBS
                if (!object->support_layers().empty() && object->support_layers().front()->support_type == stInnerTree) {
                    for (const ExPolygon &ex_poly : object->support_layers().front()->lslices) {
                        // BBS: additional brim width will be added if adhesion area is too small without brim
                        float brim_width_mod = ex_poly.area() / ex_poly.contour.length() < scaled_half_min_adh_length
                            && brim_width < scaled_flow_width ? brim_width + scaled_additional_brim_width : brim_width;
                        brim_width_mod = floor(brim_width_mod / scaled_flow_width / 2) * scaled_flow_width * 2;
                        // Brim will not be generated for supports
                        /*
                        if (has_outer_brim) {
                            append(brim_area_support, diff_ex(offset_ex(ex_poly.contour, brim_width_mod + brim_offset, jtRound, SCALED_RESOLUTION), offset_ex(ex_poly.contour, brim_offset)));
                        }
                        if (has_inner_brim)
                            append(brim_area_support, diff_ex(offset_ex(ex_poly.holes, -brim_offset), offset_ex(ex_poly.holes, -brim_width - brim_offset)));
                        */
                        if (!has_outer_brim)
                            append(no_brim_area_support, diff_ex(offset(ex_poly.contour, no_brim_offset), ex_poly.holes));
                        if (!has_inner_brim && !has_outer_brim)
                            append(no_brim_area_support, offset_ex(ex_poly.holes, -no_brim_offset));
                        append(holes_support, ex_poly.holes);
                        if (has_inner_brim || has_outer_brim)
                            append(no_brim_area_support, offset_ex(ex_poly.contour, 0));
                        no_brim_area_support.emplace_back(ex_poly.contour);
                    }
                }
                brimToWrite.at(object->id()).sup = false;
                for (const PrintInstance& instance : object->instances()) {
                    if (!brim_area_support.empty())
                        append_and_translate(brim_area, brim_area_support, instance, print, supportBrimAreaMap);
                    append_and_translate(no_brim_area, no_brim_area_support, instance);
                    append_and_translate(holes, holes_support, instance);
                }
                if (supportBrimAreaMap.find(object->id()) != supportBrimAreaMap.end())
                    expolygons_append(brim_area, supportBrimAreaMap[object->id()]);
            }
        }
    }

    int  extruder_nums = print.config().nozzle_diameter.values.size();
    std::vector<Polygons> extruder_unprintable_area = print.get_extruder_printable_polygons();
    // Orca: if per-extruder print area is not specified, use the whole bed as printable area for all extruders
    if (extruder_unprintable_area.empty()) {
        extruder_unprintable_area.resize(extruder_nums, Polygons{Model::getBedPolygon()});
    }
    std::vector<int> filament_map = print.get_filament_maps();

    if (print.has_wipe_tower() && !print.get_fake_wipe_tower().outer_wall.empty()) {
        ExPolygons expolyFromLines{};
        for (auto polyline : print.get_fake_wipe_tower().outer_wall.begin()->second) {
            polyline.remove_duplicate_points();
            expolyFromLines.emplace_back(polyline.points);
            expolyFromLines.back().translate(Point(scale_(print.get_fake_wipe_tower().pos[0]), scale_(print.get_fake_wipe_tower().pos[1])));
        }
        expolygons_append(no_brim_area, expolyFromLines);
    }

    for (const PrintObject* object : print.objects()) {
        ExPolygons extruder_no_brim_area = no_brim_area;
        auto iter = std::find_if(objPrintVec.begin(), objPrintVec.end(), [object](const std::pair<ObjectID, unsigned int>& item) {
            return item.first == object->id();
        });

        if (iter != objPrintVec.end()) {
            int extruder_id = filament_map[iter->second - 1] - 1;
            auto bedPoly = extruder_unprintable_area[extruder_id];
            auto bedExPoly   = diff_ex((offset(bedPoly, scale_(30.), jtRound, SCALED_RESOLUTION)), {bedPoly});
            if (!bedExPoly.empty()) {
                extruder_no_brim_area.push_back(bedExPoly.front());
            }
            //extruder_no_brim_area = offset2_ex(extruder_no_brim_area, scaled_flow_width, -scaled_flow_width); // connect scattered small areas to prevent generating very small brims

        }

        if (brimAreaMap.find(object->id()) != brimAreaMap.end()) {
            brimAreaMap[object->id()] = diff_ex(brimAreaMap[object->id()], extruder_no_brim_area);
        }

        if (supportBrimAreaMap.find(object->id()) != supportBrimAreaMap.end())
            supportBrimAreaMap[object->id()] = diff_ex(supportBrimAreaMap[object->id()], extruder_no_brim_area);
    }

    brim_area.clear();
    for (const PrintObject* object : print.objects()) {
        // BBS: brim should be contacted to at least one object's island or brim area
        if (brimAreaMap.find(object->id()) != brimAreaMap.end()) {
            // find other objects' brim area
            ExPolygons otherExPolys;
            for (const PrintObject* otherObject : print.objects()) {
                if ((otherObject->id() != object->id()) && (brimAreaMap.find(otherObject->id()) != brimAreaMap.end())) {
                    expolygons_append(otherExPolys, brimAreaMap[otherObject->id()]);
                }
            }

            auto tempArea = brimAreaMap[object->id()];
            brimAreaMap[object->id()].clear();

            for (int ia = 0; ia != tempArea.size(); ++ia) {
                // find this object's other brim area
                ExPolygons otherExPoly;
                for (int iao = 0; iao != tempArea.size(); ++iao)
                    if (iao != ia) otherExPoly.push_back(tempArea[iao]);

                auto offsetedTa = offset_ex(tempArea[ia], print.brim_flow().scaled_spacing() * 2, jtRound, SCALED_RESOLUTION);
                if (!intersection_ex(offsetedTa, objectIslands).empty() ||
                    !intersection_ex(offsetedTa, otherExPoly).empty() ||
                    !intersection_ex(offsetedTa, otherExPolys).empty())
                    brimAreaMap[object->id()].push_back(tempArea[ia]);
            }
            expolygons_append(brim_area, brimAreaMap[object->id()]);
        }
    }
    return brim_area;
}
// Flip orientation of open polylines to minimize travel distance.
static void optimize_polylines_by_reversing(Polylines *polylines)
{
    for (size_t poly_idx = 1; poly_idx < polylines->size(); ++poly_idx) {
        const Polyline &prev = (*polylines)[poly_idx - 1];
        Polyline &      next = (*polylines)[poly_idx];

        if (!next.is_closed()) {
            double dist_to_start = (next.first_point() - prev.last_point()).cast<double>().norm();
            double dist_to_end   = (next.last_point() - prev.last_point()).cast<double>().norm();

            if (dist_to_end < dist_to_start)
                next.reverse();
        }
    }
}

static Polylines connect_brim_lines(Polylines &&polylines, const Polygons &brim_area, float max_connection_length)
{
    if (polylines.empty())
        return {};

    BoundingBox bbox = get_extents(polylines);
    bbox.merge(get_extents(brim_area));

    EdgeGrid::Grid grid(bbox.inflated(SCALED_EPSILON));
    grid.create(brim_area, polylines, coord_t(scale_(10.)));

    struct Visitor
    {
        explicit Visitor(const EdgeGrid::Grid &grid) : grid(grid) {}

        bool operator()(coord_t iy, coord_t ix)
        {
            // Called with a row and colum of the grid cell, which is intersected by a line.
            auto cell_data_range = grid.cell_data_range(iy, ix);
            this->intersect      = false;
            for (auto it_contour_and_segment = cell_data_range.first; it_contour_and_segment != cell_data_range.second; ++it_contour_and_segment) {
                // End points of the line segment and their vector.
                auto segment = grid.segment(*it_contour_and_segment);
                if (Geometry::segments_intersect(segment.first, segment.second, brim_line.a, brim_line.b)) {
                    this->intersect = true;
                    return false;
                }
            }
            // Continue traversing the grid along the edge.
            return true;
        }

        const EdgeGrid::Grid &grid;
        Line                  brim_line;
        bool                  intersect = false;

    } visitor(grid);

    // Connect successive polylines if they are open, their ends are closer than max_connection_length.
    // Remove empty polylines.
    {
        // Skip initial empty lines.
        size_t poly_idx = 0;
        for (; poly_idx < polylines.size() && polylines[poly_idx].empty(); ++ poly_idx) ;
        size_t end = ++ poly_idx;
        double max_connection_length2 = Slic3r::sqr(max_connection_length);
        for (; poly_idx < polylines.size(); ++poly_idx) {
            Polyline &next = polylines[poly_idx];
            if (! next.empty()) {
                Polyline &prev = polylines[end - 1];
                bool   connect = false;
                if (! prev.is_closed() && ! next.is_closed()) {
                    double dist2 = (prev.last_point() - next.first_point()).cast<double>().squaredNorm();
                    if (dist2 <= max_connection_length2) {
                        visitor.brim_line.a = prev.last_point();
                        visitor.brim_line.b = next.first_point();
                        // Shrink the connection line to avoid collisions with the brim centerlines.
                        visitor.brim_line.extend(-SCALED_EPSILON);
                        grid.visit_cells_intersecting_line(visitor.brim_line.a, visitor.brim_line.b, visitor);
                        connect = ! visitor.intersect;
                    }
                }
                if (connect) {
                    append(prev.points, std::move(next.points));
                } else {
                    if (end < poly_idx)
                        polylines[end] = std::move(next);
                    ++ end;
                }
            }
        }
        if (end < polylines.size())
            polylines.erase(polylines.begin() + int(end), polylines.end());
    }

    return std::move(polylines);
}

// BBS: this function is used to generate brim for inner island inside holes
// Collect island + brim area to be minused when generating inner brim for holes
static void make_inner_island_brim(const Print& print, const ConstPrintObjectPtrs& top_level_objects_with_brim,
    ExtrusionEntityCollection &brim, ExPolygons &islands_area_ex)
{
    const auto scaled_resolution = scaled<double>(print.config().resolution.value);

    auto save_polygon_if_is_inner_island = [scaled_resolution](const Polygons& holes_area, Polygon& contour, std::map<size_t, Polygons>& hole_island_pair) {
        for (size_t i = 0; i < holes_area.size(); i++) {
            Polygons contour_polys;
            contour_polys.push_back(contour);
            if (diff_ex(contour_polys, { holes_area[i] }).empty()) {
                // BBS: this is an inner island inside holes_area[i], save
                contour.douglas_peucker(scaled_resolution);
                hole_island_pair[i].push_back(contour);
                break;
            }
        }
    };

    Flow flow = print.brim_flow();
    for (const PrintObject* object : top_level_objects_with_brim) {
        const BrimType brim_type = object->config().brim_type.value;
        // BBS: don't need to handle this object if hasn't enabled outer_brim
        if (brim_type == BrimType::btNoBrim)
            continue;

        //BBS: 1 collect holes area which is used to limit the brim of inner island
        Polygons holes_area;
        for (const ExPolygon& ex_poly : object->layers().front()->lslices)
            polygons_append(holes_area, ex_poly.holes);


        //BBS: 2 get the island polygons inside holes, saved as map
        std::map<size_t, Polygons> hole_island_pair;
        for (const ExPolygon& ex_poly : object->layers().front()->lslices) {
            Polygon counter = ex_poly.contour;
            save_polygon_if_is_inner_island(holes_area, counter, hole_island_pair);
        }

        if (!object->support_layers().empty()) {
            for (const Polygon& support_contour : object->support_layers().front()->support_fills.polygons_covered_by_spacing()) {
                Polygon counter = support_contour;
                save_polygon_if_is_inner_island(holes_area, counter, hole_island_pair);
            }
        }

        //BBS: 3 generate loops, only save part of loop which inside hole
        const float    brim_offset = scale_(object->config().brim_object_gap.value);
        const float    brim_width = scale_(object->config().brim_width.value);
        if (brim_type == BrimType::btInnerOnly) {
            // If brim_type is btInnerOnly, we actually doesn't generate loops for inner island.
            // Only update islands_area_ex and return
            for (auto it = hole_island_pair.begin(); it != hole_island_pair.end(); it++) {
                ExPolygons islands_area_ex_object = intersection_ex(offset(it->second, brim_offset), offset(holes_area[it->first], -brim_offset));
                for (const PrintInstance& instance : object->instances())
                    append_and_translate(islands_area_ex, islands_area_ex_object, instance);
            }
        }
        else {
            size_t         num_loops = size_t(floor(brim_width / float(flow.scaled_spacing())));
            for (auto it = hole_island_pair.begin(); it != hole_island_pair.end(); it++) {
                Polygons        loops;
                Polygons inner_islands = offset(it->second, brim_offset);
                Polygons brimable_area = offset(holes_area[it->first], -brim_offset);   //offset to keep away from hole
                Polygons contour = inner_islands;
                for (size_t i = 0; i < num_loops; ++i) {
                    contour = offset(contour, float(flow.scaled_spacing()), jtSquare);
                    for (Polygon& poly : contour)
                        poly.douglas_peucker(scaled_resolution);
                    polygons_append(loops, offset(contour, -0.5f * float(flow.scaled_spacing())));
                }
                // BBS: to be checked.
                //loops = union_pt_chained_outside_in(loops, false);
                loops = union_pt_chained_outside_in(loops);

                std::vector<Polylines> loops_pl_by_levels;
                {
                    Polylines              loops_pl = to_polylines(loops);
                    loops_pl_by_levels.assign(loops_pl.size(), Polylines());
                    tbb::parallel_for(tbb::blocked_range<size_t>(0, loops_pl.size()),
                        [&loops_pl_by_levels, &loops_pl, &brimable_area](const tbb::blocked_range<size_t>& range) {
                            for (size_t i = range.begin(); i < range.end(); ++i) {
                                loops_pl_by_levels[i] = chain_polylines(intersection_pl({ std::move(loops_pl[i]) }, brimable_area));
                            }
                        });
                }

                // BBS: Reduce down to the ordered list of polylines.
                Polylines all_loops_object;
                for (Polylines& polylines : loops_pl_by_levels)
                    append(all_loops_object, std::move(polylines));
                loops_pl_by_levels.clear();

                optimize_polylines_by_reversing(&all_loops_object);
                all_loops_object = connect_brim_lines(std::move(all_loops_object), offset(inner_islands, float(SCALED_EPSILON)), float(flow.scaled_spacing()) * 2.f);

                Polylines final_loops;
                for (const PrintInstance& instance : object->instances()) {
                    size_t dst_idx = final_loops.size();
                    final_loops.insert(final_loops.end(), all_loops_object.begin(), all_loops_object.end());
                    Point instance_shift = instance.shift_without_plate_offset();
                    for (; dst_idx < final_loops.size(); ++dst_idx)
                        final_loops[dst_idx].translate(instance_shift);

                }
                extrusion_entities_append_loops_and_paths(brim.entities, std::move(final_loops),
                    erBrim, float(flow.mm3_per_mm()), float(flow.width()),
                    float(print.skirt_first_layer_height()));

                //BBS: save all inner island and inner island brim area here, which is necesary if generate inner brim for holes
                //Inner brim of holes must not occupy this area
                ExPolygons islands_area_ex_object = intersection_ex(contour, brimable_area);
                for (const PrintInstance& instance : object->instances())
                    append_and_translate(islands_area_ex, islands_area_ex_object, instance);
            }
        }
    }
}

//BBS: the brim are generated one by one, and sorted by objs/supports and extruders
static void make_inner_island_brim(const Print& print, const ConstPrintObjectPtrs& top_level_objects_with_brim,
    std::map<ObjectID, ExPolygons>& innerbrimAreaMap,
    std::map<ObjectID, ExPolygons>& innerSupportBrimAreaMap,
    ExPolygons& islands_area_ex, ExPolygons& NobrimArea,
    std::vector<std::pair<ObjectID, unsigned int>>& objPrintVec)
{
    auto save_polygon_if_is_inner_island = [](const Polygons& holes_area, Polygon& counter, std::map<size_t, Polygons>& hole_island_pair) {
        for (size_t i = 0; i < holes_area.size(); i++) {
            if (diff_ex(Polygons{ counter }, { holes_area[i] }).empty()) {
                // BBS: this is an inner island inside holes_area[i], save
                counter.douglas_peucker(SCALED_RESOLUTION);
                hole_island_pair[i].push_back(counter);
                break;
            }
        }
    };

    unsigned int support_material_extruder = 1;
    if (print.has_support_material()) {
        assert(top_level_objects_with_brim.front()->config().support_filament >= 0);
        if (top_level_objects_with_brim.front()->config().support_filament > 0)
            support_material_extruder = top_level_objects_with_brim.front()->config().support_filament;
    }

    std::unordered_set<size_t> top_level_objects_idx;
    top_level_objects_idx.reserve(top_level_objects_with_brim.size());
    for (const PrintObject* object : top_level_objects_with_brim)
        top_level_objects_idx.insert(object->id().id);

    struct brimWritten {
        bool obj;
        bool sup;
    };
    std::map<ObjectID, brimWritten> brimToWrite;
    for (const auto& objectWithExtruder : objPrintVec)
        if (top_level_objects_idx.find(objectWithExtruder.first.id) != top_level_objects_idx.end())
            brimToWrite.insert({ objectWithExtruder.first, {true,true} });

    Flow flow = print.brim_flow();
    for (unsigned int extruderNo : print.extruders()) {
        ++extruderNo;
        for (const auto& objectWithExtruder : objPrintVec) {
            if (top_level_objects_idx.find(objectWithExtruder.first.id) != top_level_objects_idx.end()) {
                const PrintObject* object = print.get_object(objectWithExtruder.first);
                const BrimType brim_type = object->config().brim_type.value;
                // BBS: don't need to handle this object if hasn't enabled outer_brim
                if (brim_type == BrimType::btNoBrim)
                    continue;

                //BBS: 1 collect holes area which is used to limit the brim of inner island
                Polygons holes_area;
                for (const ExPolygon& ex_poly : object->layers().front()->lslices)
                    polygons_append(holes_area, ex_poly.holes);


                //BBS: 2 get the island polygons inside holes, saved as map
                std::map<size_t, Polygons> hole_island_pair;
                for (const ExPolygon& ex_poly : object->layers().front()->lslices) {
                    Polygon counter = ex_poly.contour;
                    save_polygon_if_is_inner_island(holes_area, counter, hole_island_pair);
                }
                std::map<size_t, Polygons> hole_island_pair_supports;
                if (!object->support_layers().empty()) {
                    for (const Polygon& support_contour : object->support_layers().front()->support_fills.polygons_covered_by_spacing()) {
                        Polygon counter = support_contour;
                        save_polygon_if_is_inner_island(holes_area, counter, hole_island_pair_supports);
                    }
                }

                //BBS: 3 generate loops, only save part of loop which inside hole
                const float    brim_offset = scale_(object->config().brim_object_gap.value);
                const float    brim_width = floor(scale_(object->config().brim_width.value) / 2 / flow.scaled_spacing()) * 2 * flow.scaled_spacing();
                if (objectWithExtruder.second == extruderNo && brimToWrite.at(object->id()).obj) {
                    if (brim_type == BrimType::btInnerOnly) {
                        // If brim_type is btInnerOnly, we actually doesn't generate loops for inner island.
                        // Only update islands_area_ex and return
                        for (auto it = hole_island_pair.begin(); it != hole_island_pair.end(); it++) {
                            ExPolygons islands_area_ex_object = intersection_ex(offset(it->second, brim_offset), offset(holes_area[it->first], -brim_offset));
                            for (const PrintInstance& instance : object->instances())
                                append_and_translate(islands_area_ex, islands_area_ex_object, instance);
                        }
                        brimToWrite.at(object->id()).obj = false;
                    }
                    else {
                        for (auto it = hole_island_pair.begin(); it != hole_island_pair.end(); it++) {
                            Polygons        loops;
                            Polygons inner_islands = offset(it->second, brim_offset);
                            Polygons brimable_area = offset(holes_area[it->first], -brim_offset);   //offset to keep away from hole
                            Polygons contour = offset(inner_islands, brim_offset + brim_width, jtRound, SCALED_RESOLUTION);
                            for (Polygon& poly : contour)
                                poly.douglas_peucker(SCALED_RESOLUTION);


                            //BBS: save all inner island and inner island brim area here, which is necesary if generate inner brim for holes
                            //Inner brim of holes must not occupy this area
                            ExPolygons islands_area_ex_object = intersection_ex(contour, brimable_area);
                            ExPolygons inner_islands_exp = offset_ex(inner_islands, 0.);
                            islands_area_ex_object = diff_ex(islands_area_ex_object, inner_islands_exp);
                            for (const PrintInstance& instance : object->instances())
                                append_and_translate(islands_area_ex, islands_area_ex_object, instance, print, innerbrimAreaMap);
                        }
                        brimToWrite.at(object->id()).obj = false;
                    }
                    if (innerbrimAreaMap.find(object->id()) != innerbrimAreaMap.end())
                        expolygons_append(islands_area_ex, innerbrimAreaMap[object->id()]);
                }


                if (support_material_extruder == extruderNo && brimToWrite.at(object->id()).sup) {
                    if (brim_type == BrimType::btInnerOnly) {
                        // If brim_type is btInnerOnly, we actually doesn't generate loops for inner island.
                        // Only update islands_area_ex and return
                        for (auto it = hole_island_pair_supports.begin(); it != hole_island_pair_supports.end(); it++) {
                            ExPolygons islands_area_ex_support = intersection_ex(offset(it->second, 0), offset(holes_area[it->first], 0));
                            for (const PrintInstance& instance : object->instances())
                                append_and_translate(islands_area_ex, islands_area_ex_support, instance);
                        }
                        brimToWrite.at(object->id()).sup = false;
                    }
                    else {
                        for (auto it = hole_island_pair_supports.begin(); it != hole_island_pair_supports.end(); it++) {
                            Polygons        loops;
                            Polygons inner_islands = offset(it->second, 0);
                            Polygons brimable_area = offset(holes_area[it->first], -float(flow.scaled_spacing()));   //offset to keep away from hole
                            Polygons contour = offset(inner_islands, brim_width, jtRound, SCALED_RESOLUTION);
                            for (Polygon& poly : contour)
                                poly.douglas_peucker(SCALED_RESOLUTION);


                            //BBS: save all inner island and inner island brim area here, which is necesary if generate inner brim for holes
                            //Inner brim of holes must not occupy this area
                            ExPolygons islands_area_ex_support = intersection_ex(contour, brimable_area);
                            ExPolygons inner_islands_exp = offset_ex(inner_islands, 0.);
                            islands_area_ex_support = diff_ex(islands_area_ex_support, inner_islands_exp);
                            for (const PrintInstance& instance : object->instances())
                                append_and_translate(islands_area_ex, islands_area_ex_support, instance, print, innerSupportBrimAreaMap);

                        }
                        brimToWrite.at(object->id()).sup = false;
                    }
                    if (innerSupportBrimAreaMap.find(object->id()) != innerSupportBrimAreaMap.end())
                        expolygons_append(islands_area_ex, innerSupportBrimAreaMap[object->id()]);
                }
            }
        }
    }
    islands_area_ex = diff_ex(islands_area_ex, NobrimArea);
    for (const PrintObject* object : print.objects()) {
        if (innerbrimAreaMap.find(object->id()) != innerbrimAreaMap.end())
            innerbrimAreaMap[object->id()] = diff_ex(innerbrimAreaMap[object->id()], NobrimArea);
        if (innerSupportBrimAreaMap.find(object->id()) != innerSupportBrimAreaMap.end())
            innerSupportBrimAreaMap[object->id()] = diff_ex(innerSupportBrimAreaMap[object->id()], NobrimArea);
    }
}
static void make_inner_brim(const Print                   &print,
                            const ConstPrintObjectPtrs    &top_level_objects_with_brim,
                            const std::vector<ExPolygons> &bottom_layers_expolygons,
                            ExtrusionEntityCollection     &brim)
{
    assert(print.objects().size() == bottom_layers_expolygons.size());
    const auto scaled_resolution = scaled<double>(print.config().resolution.value);

    //BBS: generate brim for inner island first
    ExPolygons inner_islands_ex;
    make_inner_island_brim(print, top_level_objects_with_brim, brim, inner_islands_ex);

#ifdef INNER_ISLAND_BRIM_DEBUG_TO_SVG
    static int irun = 0;
    BoundingBox bbox_svg;
    bbox_svg.merge(get_extents(inner_islands_ex));
    {
        std::stringstream stri;
        stri << "inner_island_and_brim_area_" << irun << ".svg";
        SVG svg(stri.str(), bbox_svg);
        svg.draw(to_polylines(inner_islands_ex), "blue");
        svg.Close();
    }
    ++ irun;
#endif

    Flow       flow = print.brim_flow();
    ExPolygons islands_ex = inner_brim_area(print, top_level_objects_with_brim, bottom_layers_expolygons, float(flow.scaled_spacing()));
    //BBS: brim of hole must not overlap with inner island and inner island brim
    if (!inner_islands_ex.empty()) {
        islands_ex = diff_ex(islands_ex, inner_islands_ex);
    }

    Polygons   loops;
    islands_ex      = offset_ex(islands_ex, -0.5f * float(flow.scaled_spacing()));// jtSquare seems not working when expandign the holes
    for (size_t i = 0; !islands_ex.empty(); ++i) {
        for (ExPolygon &poly_ex : islands_ex)
            poly_ex.douglas_peucker(scaled_resolution);
        polygons_append(loops, to_polygons(islands_ex));// jtSquare seems not working when expandign the holes
        islands_ex = offset_ex(islands_ex, -1.3f * float(flow.scaled_spacing()));
        islands_ex = offset_ex(islands_ex, .3f * float(flow.scaled_spacing()));
    }

    loops = union_pt_chained_outside_in(loops);
    std::reverse(loops.begin(), loops.end());
    extrusion_entities_append_loops(brim.entities, std::move(loops), erBrim, float(flow.mm3_per_mm()),
                                    float(flow.width()), float(print.skirt_first_layer_height()));
}

// BBS: generate inner brim by objs
static void make_inner_brim(const Print& print, const ConstPrintObjectPtrs& top_level_objects_with_brim,
    std::map<ObjectID, ExPolygons>& brimAreaMap, std::map<ObjectID, ExPolygons>& supportBrimAreaMap,
    std::vector<std::pair<ObjectID, unsigned int>>& objPrintVec)
{
    //BBS: generate brim for inner island first


#ifdef INNER_ISLAND_BRIM_DEBUG_TO_SVG
    static int irun = 0;
    BoundingBox bbox_svg;
    bbox_svg.merge(get_extents(inner_islands_ex));
    {
        std::stringstream stri;
        stri << "inner_island_and_brim_area_" << irun << ".svg";
        SVG svg(stri.str(), bbox_svg);
        svg.draw(to_polylines(inner_islands_ex), "blue");
        svg.Close();
    }
    ++irun;
#endif

    Flow       flow = print.brim_flow();
    ExPolygons NoBrim = inner_brim_area(print, top_level_objects_with_brim,
        float(flow.scaled_spacing()), brimAreaMap, supportBrimAreaMap, objPrintVec);

    ExPolygons inner_islands_ex;
    std::map<ObjectID, ExPolygons> innerBrimAreaMap;
    std::map<ObjectID, ExPolygons> innerSupportBrimAreaMap;
    /*make_inner_island_brim(print, top_level_objects_with_brim, innerBrimAreaMap, innerSupportBrimAreaMap,
        inner_islands_ex, NoBrim, objPrintVec);*/

    //BBS: brim of hole must not overlap with inner island and inner island brim
    if (!inner_islands_ex.empty()) {
        if (brimAreaMap.size() > 0) {
            for (auto iter = brimAreaMap.begin(); iter != brimAreaMap.end(); ++iter) {
                if (!iter->second.empty()) {
                    iter->second = diff_ex(iter->second, inner_islands_ex);
                };
            }
        }
        if (supportBrimAreaMap.size() > 0) {
            for (auto iter = supportBrimAreaMap.begin(); iter != supportBrimAreaMap.end(); ++iter) {
                if (!iter->second.empty()) {
                    iter->second = diff_ex(iter->second, inner_islands_ex);
                };
            }
        }
        for (const PrintObject* object : print.objects()) {
            if (innerBrimAreaMap.find(object->id()) != innerBrimAreaMap.end()) {
                append(brimAreaMap[object->id()], innerBrimAreaMap[object->id()]);
            }
            if (innerSupportBrimAreaMap.find(object->id()) != innerSupportBrimAreaMap.end()) {
                append(supportBrimAreaMap[object->id()], innerSupportBrimAreaMap[object->id()]);
            }
        }
    }
}


//BBS: generate out brim by offseting ExPolygons 'islands_area_ex'
Polygons tryExPolygonOffset(const ExPolygons islandAreaEx, const Print& print)
{
    const auto scaled_resolution = scaled<double>(print.config().resolution.value);
    Polygons   loops;
    ExPolygons islands_ex;
    Flow       flow = print.brim_flow();

    double resolution = 0.0125 / SCALING_FACTOR;
    islands_ex = islandAreaEx;
    for (ExPolygon& poly_ex : islands_ex)
        poly_ex.douglas_peucker(resolution);
    islands_ex = offset_ex(std::move(islands_ex), -0.5f * float(flow.scaled_spacing()), jtRound, resolution);
    for (size_t i = 0; !islands_ex.empty(); ++i) {
        for (ExPolygon& poly_ex : islands_ex)
            poly_ex.douglas_peucker(resolution);
        polygons_append(loops, to_polygons(islands_ex));
        islands_ex = offset_ex(std::move(islands_ex), -1.3f*float(flow.scaled_spacing()), jtRound, resolution);
        for (ExPolygon& poly_ex : islands_ex)
            poly_ex.douglas_peucker(resolution);
        islands_ex = offset_ex(std::move(islands_ex), 0.3f*float(flow.scaled_spacing()), jtRound, resolution);
    }
    return loops;
}
//BBS: a function creates the ExtrusionEntityCollection from the brim area defined by ExPolygons
ExtrusionEntityCollection makeBrimInfill(const ExPolygons& singleBrimArea, const Print& print, const Polygons& islands_area) {
    Polygons        loops = tryExPolygonOffset(singleBrimArea, print);
    Flow  flow = print.brim_flow();
    loops = union_pt_chained_outside_in(loops);

    std::vector<Polylines> loops_pl_by_levels;
    {
        Polylines              loops_pl = to_polylines(loops);
        loops_pl_by_levels.assign(loops_pl.size(), Polylines());
        tbb::parallel_for(tbb::blocked_range<size_t>(0, loops_pl.size()),
            [&loops_pl_by_levels, &loops_pl /*, &islands_area*/](const tbb::blocked_range<size_t>& range) {
                for (size_t i = range.begin(); i < range.end(); ++i) {
                    loops_pl_by_levels[i] = chain_polylines({ std::move(loops_pl[i]) });
                    //loops_pl_by_levels[i] = chain_polylines(intersection_pl({ std::move(loops_pl[i]) }, islands_area));
                }
            });
    }

    // output
    ExtrusionEntityCollection brim;
    // Reduce down to the ordered list of polylines.
    Polylines all_loops;
    for (Polylines& polylines : loops_pl_by_levels)
        append(all_loops, std::move(polylines));
    loops_pl_by_levels.clear();

    // Flip orientation of open polylines to minimize travel distance.
    optimize_polylines_by_reversing(&all_loops);
    all_loops = connect_brim_lines(std::move(all_loops), offset(singleBrimArea, float(SCALED_EPSILON)), float(flow.scaled_spacing()) * 2.f);

    //BBS: finally apply the plate offset which may very large
    auto plate_offset = print.get_plate_origin();
    Point scaled_plate_offset = Point(scaled(plate_offset.x()), scaled(plate_offset.y()));
    for (Polyline& one_loop : all_loops)
        one_loop.translate(scaled_plate_offset);

    extrusion_entities_append_loops_and_paths(brim.entities, std::move(all_loops), erBrim, float(flow.mm3_per_mm()), float(flow.width()), float(print.skirt_first_layer_height()));
    return brim;
}

//BBS: an overload of the orignal brim generator that generates the brim by obj and by extruders
void make_brim(const Print& print, PrintTryCancel try_cancel, Polygons& islands_area,
    std::map<ObjectID, ExtrusionEntityCollection>& brimMap,
    std::map<ObjectID, ExtrusionEntityCollection>& supportBrimMap,
    std::vector<std::pair<ObjectID, unsigned int>> &objPrintVec,
    std::vector<unsigned int>& printExtruders)
{

    double brim_width_max = 0;
    std::map<ObjectID, double> brim_width_map;
    std::map<ObjectID, ExPolygons> brimAreaMap;
    std::map<ObjectID, ExPolygons> supportBrimAreaMap;
    Flow                 flow = print.brim_flow();
    const auto           scaled_resolution = scaled<double>(print.config().resolution.value);
    ExPolygons           islands_area_ex = outer_inner_brim_area(print,
        float(flow.scaled_spacing()), brimAreaMap, supportBrimAreaMap, objPrintVec, printExtruders);

    // BBS: Find boundingbox of the first layer
    for (const ObjectID printObjID : print.print_object_ids()) {
        BoundingBox bbx;
        PrintObject* object = const_cast<PrintObject*>(print.get_object(printObjID));
        for (const ExPolygon& ex_poly : object->layers().front()->lslices)
            for (const PrintInstance& instance : object->instances()) {
                auto ex_poly_translated = ex_poly;
                ex_poly_translated.translate(instance.shift_without_plate_offset());
                bbx.merge(get_extents(ex_poly_translated.contour));
            }
        if (!object->support_layers().empty())
        for (const Polygon& support_contour : object->support_layers().front()->support_fills.polygons_covered_by_spacing())
            for (const PrintInstance& instance : object->instances()) {
                auto ex_poly_translated = support_contour;
                ex_poly_translated.translate(instance.shift_without_plate_offset());
                bbx.merge(get_extents(ex_poly_translated));
            }
        if (supportBrimAreaMap.find(printObjID) != supportBrimAreaMap.end()) {
            for (const ExPolygon& ex_poly : supportBrimAreaMap.at(printObjID))
                bbx.merge(get_extents(ex_poly.contour));
        }
        if (brimAreaMap.find(printObjID) != brimAreaMap.end()) {
            for (const ExPolygon& ex_poly : brimAreaMap.at(printObjID))
                bbx.merge(get_extents(ex_poly.contour));
        }
        object->firstLayerObjectBrimBoundingBox = bbx;
    }

    islands_area = to_polygons(islands_area_ex);

    // BBS: plate offset is applied
    const Vec3d plate_offset = print.get_plate_origin();
    Point plate_shift = Point(scaled(plate_offset.x()), scaled(plate_offset.y()));
    for (size_t iia = 0; iia < islands_area.size(); ++iia)
        islands_area[iia].translate(plate_shift);

    for (auto iter = brimAreaMap.begin(); iter != brimAreaMap.end(); ++iter) {
        if (!iter->second.empty()) {
            brimMap.insert(std::make_pair(iter->first, makeBrimInfill(iter->second, print, islands_area)));
        };
    }
    for (auto iter = supportBrimAreaMap.begin(); iter != supportBrimAreaMap.end(); ++iter) {
        if (!iter->second.empty()) {
            supportBrimMap.insert(std::make_pair(iter->first, makeBrimInfill(iter->second, print, islands_area)));
        };
    }

    size_t          num_loops = size_t(floor(brim_width_max / flow.spacing()));
    BOOST_LOG_TRIVIAL(debug) << "brim_width_max, num_loops: " << brim_width_max << ", " << num_loops;
}

} // namespace Slic3r
