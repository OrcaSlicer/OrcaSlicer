//ORCA: Tangential sacrificial bridging for counterbore holes
#include "TangentialHoleBridging.hpp"
#include "Print.hpp"
#include "Layer.hpp"
#include "ClipperUtils.hpp"
#include "BridgeDetector.hpp"
#include "Flow.hpp"

namespace Slic3r {

void TangentialHoleBridging::apply(PrintObject* object)
{
    // check if ANY region has chbTangential enabled
    bool has_tangential = false;
    for (size_t region_id = 0; region_id < object->num_printing_regions(); ++region_id) {
        if (object->printing_region(region_id).config().counterbore_hole_bridging == chbTangential) {
            has_tangential = true;
            break;
        }
    }
    if (!has_tangential || object->layer_count() < 3)
        return;

    const double nozzle_diameter = object->print()->config().nozzle_diameter.get_at(0);
    const coord_t margin = scale_(nozzle_diameter * 2.0); // anchor margin

    for (size_t i = 2; i < object->layer_count(); ++i) {
        Layer* layer_n = object->get_layer(i);
        Layer* layer_n1 = object->get_layer(i - 1);
        Layer* layer_n2 = object->get_layer(i - 2);

        for (size_t region_id = 0; region_id < object->num_printing_regions(); ++region_id) {
            if (object->printing_region(region_id).config().counterbore_hole_bridging != chbTangential) {
                continue;
            }

            LayerRegion* region_n = layer_n->get_region(region_id);
            if (!region_n || region_n->slices.empty()) continue;

            // Find unsupported areas specifically for this region
            ExPolygons unsupported = diff_ex(region_n->slices.surfaces, layer_n1->lslices, ApplySafetyOffset::Yes);
            if (unsupported.empty()) continue;

            const coord_t max_lip = scale_(8.0);

            for (const ExPolygon& poly_unsupp : unsupported) {
                // A counterbore overhang must have a hole in the middle (it's a ring or a bridge with holes)
                if (poly_unsupp.holes.empty()) {
                    continue; 
                }

                BoundingBox bbox = get_extents(poly_unsupp);

                // We must ensure the struts will anchor to something in N-1 and N-2.
                // ORCA: Expand contour to ensure anchoring into the solid walls
                Polygons contour_bigger = offset(poly_unsupp.contour, margin);
                Polygons anchors = intersection(contour_bigger, to_polygons(layer_n1->lslices));
                
                if (anchors.empty()) {
                    continue;
                }

                Polygons raw_struts_n1;
                Polygons raw_struts_n2;

                // ORCA: Detect the bridging direction for the entire unsupported area.
                // This ensures all holes in the same bridge use a consistent and optimal angle.
                BridgeDetector bd(poly_unsupp, layer_n1->lslices, scale_(nozzle_diameter));
                bd.detect_angle();
                double bridge_angle = bd.angle;
                
                // Snap to 90-degree increments to avoid noisy angles on axis-aligned models
                double snap_tol = Geometry::deg2rad(3.0);
                if (std::abs(bridge_angle) < snap_tol || std::abs(bridge_angle - PI) < snap_tol || std::abs(bridge_angle - 2.0*PI) < snap_tol) bridge_angle = 0.0;
                else if (std::abs(bridge_angle - PI/2.0) < snap_tol || std::abs(bridge_angle - 1.5*PI) < snap_tol) bridge_angle = PI/2.0;

                // ORCA: Process ALL holes instead of just the largest one
                for (const Polygon& hole : poly_unsupp.holes) {
                    BoundingBox inner_bbox = get_extents(hole);

                    coord_t dist_left   = inner_bbox.min.x() - bbox.min.x();
                    coord_t dist_right  = bbox.max.x() - inner_bbox.max.x();
                    coord_t dist_bottom = inner_bbox.min.y() - bbox.min.y();
                    coord_t dist_top    = bbox.max.y() - inner_bbox.max.y();

                    bool is_small_lip = (dist_left <= max_lip && dist_right <= max_lip && dist_bottom <= max_lip && dist_top <= max_lip);

                    int wall_loops = object->printing_region(region_id).config().wall_loops.value;
                    if (wall_loops <= 0) wall_loops = 2;
                    
                    // ORCA: Ensure even number of wall loops for normal holes to improve anchor points
                    if (!is_small_lip && wall_loops % 2 != 0) {
                        wall_loops += 1;
                    }
                    
                    // ORCA: Compute strut width properly using actual configured wall line widths.
                    // The bridging strut has empty space on BOTH its left and right sides (the small hole and the rest of the large hole gap).
                    // Therefore, it will be traced with outer loops on both sides. We need width for 2 outer walls + (wall_loops - 2) inner walls.
                    const auto &reg_config = object->printing_region(region_id).config();
                    Flow outer_flow = Flow::new_from_config_width(frExternalPerimeter, reg_config.outer_wall_line_width, nozzle_diameter, layer_n1->height);
                    Flow inner_flow = Flow::new_from_config_width(frPerimeter, reg_config.inner_wall_line_width, nozzle_diameter, layer_n1->height);
                    
                    coord_t strut_w = outer_flow.scaled_width();
                    if (wall_loops > 1) {
                        strut_w = 2 * outer_flow.scaled_width() + inner_flow.scaled_spacing() * (wall_loops - 2);
                    }

                    if (!is_small_lip) {
                        // LARGE HOLE: Generate rotated '#' struts aligned with the bridge angle
                        Point center = inner_bbox.center();
                        Polygon rotated_hole = hole;
                        rotated_hole.rotate(-bridge_angle, center);
                        BoundingBox rot_inner_bbox = get_extents(rotated_hole);
                        
                        Polygon rotated_contour = poly_unsupp.contour;
                        rotated_contour.rotate(-bridge_angle, center);
                        BoundingBox rot_bbox = get_extents(rotated_contour);

                        coord_t r_left_x   = rot_bbox.min.x() - margin;
                        coord_t r_right_x  = rot_bbox.max.x() + margin;
                        coord_t r_bottom_y = rot_bbox.min.y() - margin;
                        coord_t r_top_y    = rot_bbox.max.y() + margin;

                        // Strut Left (vertical in rotated space) -> Layer N-2
                        Polygon s_left;
                        s_left.points = {
                            Point(rot_inner_bbox.min.x() - strut_w, r_bottom_y),
                            Point(rot_inner_bbox.min.x(),           r_bottom_y),
                            Point(rot_inner_bbox.min.x(),           r_top_y),
                            Point(rot_inner_bbox.min.x() - strut_w, r_top_y)
                        };
                        s_left.rotate(bridge_angle, center);
                        
                        // Strut Right (vertical in rotated space) -> Layer N-2
                        Polygon s_right;
                        s_right.points = {
                            Point(rot_inner_bbox.max.x(),           r_bottom_y),
                            Point(rot_inner_bbox.max.x() + strut_w, r_bottom_y),
                            Point(rot_inner_bbox.max.x() + strut_w, r_top_y),
                            Point(rot_inner_bbox.max.x(),           r_top_y)
                        };
                        s_right.rotate(bridge_angle, center);
                        
                        // Strut Bottom (horizontal in rotated space) -> normally Layer N-1
                        Polygon s_bottom;
                        s_bottom.points = {
                            Point(r_left_x,  rot_inner_bbox.min.y() - strut_w),
                            Point(r_right_x, rot_inner_bbox.min.y() - strut_w),
                            Point(r_right_x, rot_inner_bbox.min.y()),
                            Point(r_left_x,  rot_inner_bbox.min.y())
                        };
                        s_bottom.rotate(bridge_angle, center);

                        // Strut Top (horizontal in rotated space) -> normally Layer N-1
                        Polygon s_top;
                        s_top.points = {
                            Point(r_left_x,  rot_inner_bbox.max.y()),
                            Point(r_right_x, rot_inner_bbox.max.y()),
                            Point(r_right_x, rot_inner_bbox.max.y() + strut_w),
                            Point(r_left_x,  rot_inner_bbox.max.y() + strut_w)
                        };
                        s_top.rotate(bridge_angle, center);

                        Polygons s_A = {s_left, s_right};
                        Polygons s_B = {s_bottom, s_top};

                        Polygons anchors_n2 = to_polygons(layer_n2->lslices);
                        double area_A = 0;
                        for (const Polygon& p : intersection(s_A, anchors_n2)) {
                            area_A += std::abs(p.area());
                        }
                        
                        double area_B = 0;
                        for (const Polygon& p : intersection(s_B, anchors_n2)) {
                            area_B += std::abs(p.area());
                        }

                        // ORCA: If the default first layer (A) has significantly less anchoring 
                        // than the second layer (B), swap them. This ensures the first printed 
                        // struts are securely anchored to the walls (especially useful for custom modifiers).
                        if (area_A < area_B * 0.5) {
                            for (const Polygon& p : s_B) raw_struts_n2.push_back(p);
                            for (const Polygon& p : s_A) raw_struts_n1.push_back(p);
                        } else {
                            for (const Polygon& p : s_A) raw_struts_n2.push_back(p);
                            for (const Polygon& p : s_B) raw_struts_n1.push_back(p);
                        }
                    } else {
                        // SMALL LIP: Original axis-aligned behavior for solid filling
                        coord_t l_x = bbox.min.x() - margin;
                        coord_t r_x = bbox.max.x() + margin;
                        coord_t b_y = bbox.min.y() - margin;
                        coord_t t_y = bbox.max.y() + margin;

                        Polygon s_left;
                        s_left.points = { Point(l_x, b_y), Point(inner_bbox.min.x(), b_y), Point(inner_bbox.min.x(), t_y), Point(l_x, t_y) };
                        Polygon s_right;
                        s_right.points = { Point(inner_bbox.max.x(), b_y), Point(r_x, b_y), Point(r_x, t_y), Point(inner_bbox.max.x(), t_y) };
                        raw_struts_n2.push_back(s_left);
                        raw_struts_n2.push_back(s_right);

                        Polygon s_bottom;
                        s_bottom.points = { Point(l_x, b_y), Point(r_x, b_y), Point(r_x, inner_bbox.min.y()), Point(l_x, inner_bbox.min.y()) };
                        Polygon s_top;
                        s_top.points = { Point(l_x, inner_bbox.max.y()), Point(r_x, inner_bbox.max.y()), Point(r_x, t_y), Point(l_x, t_y) };
                        raw_struts_n1.push_back(s_bottom);
                        raw_struts_n1.push_back(s_top);
                    }
                }

                Polygons final_struts_n1 = intersection(raw_struts_n1, contour_bigger);
                Polygons final_struts_n2 = intersection(raw_struts_n2, contour_bigger);

                Polygons all_holes_small_lips;
                for (const Polygon& hole : poly_unsupp.holes) {
                    BoundingBox h_bbox = get_extents(hole);
                    coord_t d_l = h_bbox.min.x() - bbox.min.x();
                    coord_t d_r = bbox.max.x() - h_bbox.max.x();
                    coord_t d_b = h_bbox.min.y() - bbox.min.y();
                    coord_t d_t = bbox.max.y() - h_bbox.max.y();
                    if (d_l <= max_lip && d_r <= max_lip && d_b <= max_lip && d_t <= max_lip) {
                        all_holes_small_lips.push_back(hole);
                    }
                }

                if (!all_holes_small_lips.empty()) {
                    final_struts_n1 = diff(final_struts_n1, all_holes_small_lips);
                    final_struts_n2 = diff(final_struts_n2, all_holes_small_lips);
                }

                if (final_struts_n1.empty() && final_struts_n2.empty()) continue;

                layer_n2->lslices = union_ex(layer_n2->lslices, final_struts_n2);
                layer_n1->lslices = union_ex(layer_n1->lslices, final_struts_n1);
                
                if (!layer_n1->regions().empty() && !final_struts_n1.empty()) {
                    for (size_t r = 0; r < layer_n1->regions().size(); ++r) {
                        if (r == region_id) continue;
                        LayerRegion* other_region = layer_n1->get_region(r);
                        if (!other_region || other_region->slices.empty()) continue;
                        other_region->slices.set(diff_ex(other_region->slices.surfaces, final_struts_n1), stInternal);
                    }
                    LayerRegion* target_region = layer_n1->get_region(region_id);
                    if (target_region) {
                        Polygons p1 = to_polygons(target_region->slices.surfaces);
                        for (const Polygon& p : final_struts_n1) p1.push_back(p);
                        target_region->slices.set(union_ex(p1), stInternal);
                    }
                }
                
                if (!layer_n2->regions().empty() && !final_struts_n2.empty()) {
                    for (size_t r = 0; r < layer_n2->regions().size(); ++r) {
                        if (r == region_id) continue;
                        LayerRegion* other_region = layer_n2->get_region(r);
                        if (!other_region || other_region->slices.empty()) continue;
                        other_region->slices.set(diff_ex(other_region->slices.surfaces, final_struts_n2), stInternal);
                    }
                    LayerRegion* target_region = layer_n2->get_region(region_id);
                    if (target_region) {
                        Polygons p2 = to_polygons(target_region->slices.surfaces);
                        for (const Polygon& p : final_struts_n2) p2.push_back(p);
                        target_region->slices.set(union_ex(p2), stInternal);
                    }
                }
            }
        }
    }
}

} // namespace Slic3r