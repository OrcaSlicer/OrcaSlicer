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
#include <cstdint>
#include <limits>
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

//ORCA: Brim can follow the post-EFC outline when enabled.
static bool use_brim_efc_outline(const PrintObject &object)
{
    return object.config().brim_use_efc_outline.value
        && object.config().elefant_foot_compensation.value > 0.
        && object.config().elefant_foot_compensation_layers.value > 0
        && object.config().raft_layers.value == 0;
}

//ORCA: Helper for snapping painted ears to the EFC outline.
static bool closest_point_on_expolygons(const ExPolygons &polygons, const Point &from, Point &closest_out)
{
    double min_dist2 = std::numeric_limits<double>::max();
    bool found = false;

    for (const ExPolygon &poly : polygons) {
        for (int i = 0; i < poly.num_contours(); ++i) {
            const Point *candidate = poly.contour_or_hole(i).closest_point(from);
            if (candidate == nullptr)
                continue;
            const int64_t dx = int64_t(candidate->x()) - int64_t(from.x());
            const int64_t dy = int64_t(candidate->y()) - int64_t(from.y());
            const double dist2 = double(dx * dx + dy * dy);
            if (dist2 < min_dist2) {
                min_dist2 = dist2;
                closest_out = *candidate;
                found = true;
            }
        }
    }
    return found;
}

//ORCA: Helper for matching painted ears to their original island before EFC snapping.
static int find_containing_expolygon_index(const ExPolygons &polygons, const Point &from)
{
    for (size_t idx = 0; idx < polygons.size(); ++idx) {
        if (polygons[idx].contains(from))
            return int(idx);
    }
    return -1;
}

//ORCA: Keep painted ear snapping on the matching island when using EFC outline.
static bool closest_point_on_matching_island(const ExPolygons &raw_outline, const ExPolygons &efc_outline, const Point &from, Point &closest_out)
{
    const int island_idx = find_containing_expolygon_index(raw_outline, from);
    if (island_idx >= 0) {
        ExPolygons island_outline = intersection_ex(efc_outline, raw_outline[island_idx]);
        if (!island_outline.empty())
            return closest_point_on_expolygons(island_outline, from, closest_out);
    }
    return closest_point_on_expolygons(efc_outline, from, closest_out);
}
//ORCA: Use post-processed first-layer slices (including EFC) for brim outline.
// Returns ExPolygons of the bottom layer after all first-layer modifiers
// (including elephant foot compensation, if enabled) have been applied.
static ExPolygons get_print_object_bottom_layer_expolygons(const PrintObject &print_object)
{
    ExPolygons ex_polygons;
    for (LayerRegion *region : print_object.layers().front()->regions())
        Slic3r::append(ex_polygons, closing_ex(region->slices.surfaces, float(SCALED_EPSILON)));
    return ex_polygons;
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
    //ORCA: Painted ears can snap to the EFC-adjusted outline when enabled.
    const bool use_efc_outline = use_brim_efc_outline(*object);
    const ExPolygons &raw_outline = object->layers().front()->lslices;
    //ORCA: Lazily computed EFC-adjusted bottom outline.
    //Stored separately so we can avoid recomputation unless EFC snapping is used.
    ExPolygons efc_outline_storage;
    const ExPolygons* efc_outline = nullptr;

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

        //ORCA: Snap painted ears to the EFC-adjusted outline when enabled.
        if (use_efc_outline) {
            if (efc_outline == nullptr) {
                //ORCA: Compute EFC-adjusted outline lazily for painted ear snapping.
                efc_outline_storage = get_print_object_bottom_layer_expolygons(*object);
                efc_outline = &efc_outline_storage;
            }

            if (!efc_outline->empty()) {
                Point closest_point;
                //ORCA: Snap within the matching island to avoid drifting to another island.
                if (closest_point_on_matching_island(
                        raw_outline,
                        *efc_outline,
                        Point(pt_x, pt_y),
                        closest_point)) {
                    pt_x = closest_point.x();
                    pt_y = closest_point.y();
                }
            }
        }

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
            //ORCA: Select brim base slices from EFC-compensated outline when enabled.
            const bool         use_efc_outline = use_brim_efc_outline(*object);
            ExPolygons         brim_slices_storage;
            const ExPolygons*  brim_slices = nullptr;
            //ORCA: Select EFC-adjusted bottom outline when enabled.
            if (use_efc_outline)
                brim_slices_storage = get_print_object_bottom_layer_expolygons(*object);
            brim_slices = use_efc_outline ? &brim_slices_storage : &object->layers().front()->lslices;

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
                    ExPolygons volume_group_slices_efc;
                    const ExPolygons* volume_group_slices = &volumeGroup.slices;
                    if (use_efc_outline) {
                        //ORCA: When using EFC outline, restrict per-volume-group slices to the
                        // EFC-adjusted bottom footprint to keep brim width heuristics consistent.
                        volume_group_slices_efc = intersection_ex(*brim_slices, volumeGroup.slices);
                        volume_group_slices = &volume_group_slices_efc;
                    }
                    for (const ExPolygon& ex_poly : *volume_group_slices) {
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
                auto objectIsland = offset_ex(*brim_slices, brim_offset, jtRound, SCALED_RESOLUTION);
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
//BBS: generate out brim by offseting ExPolygons 'islands_area_ex'
Polygons tryExPolygonOffset(const ExPolygons& islandAreaEx, const Print& print)
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
        //ORCA: Use EFC-compensated outline for brim bounding box when enabled.
        const ExPolygons brim_slices = use_brim_efc_outline(*object) ?
            get_print_object_bottom_layer_expolygons(*object) : object->layers().front()->lslices;
        for (const ExPolygon& ex_poly : brim_slices)
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
}

} // namespace Slic3r
