#include <chrono>
#include <math.h>

#include "format.hpp"
#include "ClipperUtils.hpp"
#include "Fill/FillBase.hpp"
#include "I18N.hpp"
#include "Layer.hpp"
#include "MinimumSpanningTree.hpp"
#include "Print.hpp"
#include "ShortestPath.hpp"
#include "SupportCommon.hpp"
#include "SVG.hpp"
#include "TreeSupportCommon.hpp"
#include "TreeSupport.hpp"
#include "TreeSupport3D.hpp"
#include <libnest2d/backends/libslic3r/geometries.hpp>
#include <libnest2d/placers/nfpplacer.hpp>


#include <tbb/blocked_range.h>
#include <tbb/concurrent_unordered_set.h>
#include <tbb/concurrent_vector.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_for_each.h>

#include <boost/log/trivial.hpp>

#ifndef M_PI
#define M_PI 3.1415926535897932384626433832795
#endif
#ifndef SIGN
#define SIGN(x) (x>=0?1:-1)
#endif
#define TAU (2.0 * M_PI)
#define NO_INDEX (std::numeric_limits<unsigned int>::max())
#define USE_SUPPORT_3D 0

//#define SUPPORT_TREE_DEBUG_TO_SVG

#ifdef SUPPORT_TREE_DEBUG_TO_SVG
#include "nlohmann/json.hpp"
#endif
namespace Slic3r
{
#define unscale_(val) ((val) * SCALING_FACTOR)

inline double dot_with_unscale(const Point a, const Point b)
{
    return unscale_(a(0)) * unscale_(b(0)) + unscale_(a(1)) * unscale_(b(1));
}

inline double vsize2_with_unscale(const Point pt)
{
    return dot_with_unscale(pt, pt);
}

inline Point turn90_ccw(const Point pt)
{
    Point ret;

    ret(0) = -pt(1);
    ret(1) = pt(0);
    return ret;
}

inline Point normal(Point pt, double scale)
{
    double length = scale_(sqrt(vsize2_with_unscale(pt)));
    if (length < SCALED_EPSILON) {
        return pt;
    }

    return pt * (scale / length);
}

enum TreeSupportStage {
    STAGE_DETECT_OVERHANGS,
    STAGE_GENERATE_CONTACT_NODES,
    STAGE_DROP_DOWN_NODES,
    STAGE_DRAW_CIRCLES,
    STAGE_GENERATE_TOOLPATHS,
    STAGE_MinimumSpanningTree,
    STAGE_GET_AVOIDANCE,
    STAGE_projection_onto_ex,
    STAGE_get_collision,
    STAGE_intersection_ln,
    STAGE_total,
    NUM_STAGES
};

class TreeSupportProfiler
{
public:
    uint32_t stage_durations[NUM_STAGES] = { 0 };
    uint32_t stage_index = 0;
    boost::posix_time::ptime tic_time;
    boost::posix_time::ptime toc_time;

    TreeSupportProfiler()
    {
        for (uint32_t& item : stage_durations) {
            item = 0;
        }
    }

    void stage_start(TreeSupportStage stage)
    {
        if (stage > NUM_STAGES)
            return;

        m_stage_start_times[stage] = boost::posix_time::microsec_clock::local_time();
    }

    void stage_finish(TreeSupportStage stage)
    {
        if (stage > NUM_STAGES)
            return;

        boost::posix_time::ptime time = boost::posix_time::microsec_clock::local_time();
        stage_durations[stage] = (time - m_stage_start_times[stage]).total_milliseconds();
    }

    void tic() { tic_time = boost::posix_time::microsec_clock::local_time(); }
    uint32_t toc() {
        toc_time = boost::posix_time::microsec_clock::local_time();
        return (toc_time - tic_time).total_milliseconds();
    }
    void stage_add(TreeSupportStage stage)
    {
        if (stage > NUM_STAGES)
            return;
        stage_durations[stage] += toc();
    }

    std::string report()
    {
        std::stringstream ss;
        ss << "total overhange cost: " << stage_durations[STAGE_total]
            << "; STAGE_DETECT_OVERHANGS: " << stage_durations[STAGE_DETECT_OVERHANGS]
            << "; STAGE_GENERATE_CONTACT_NODES: " << stage_durations[STAGE_GENERATE_CONTACT_NODES]
            << "; STAGE_DROP_DOWN_NODES: " << stage_durations[STAGE_DROP_DOWN_NODES]
            << "; STAGE_DRAW_CIRCLES: " << stage_durations[STAGE_DRAW_CIRCLES]
            << "; STAGE_GENERATE_TOOLPATHS: " << stage_durations[STAGE_GENERATE_TOOLPATHS]
            << "; STAGE_MinimumSpanningTree: " << stage_durations[STAGE_MinimumSpanningTree]
            << "; STAGE_GET_AVOIDANCE: " << stage_durations[STAGE_GET_AVOIDANCE]
            << "; STAGE_projection_onto_ex: " << stage_durations[STAGE_projection_onto_ex]
            << "; STAGE_get_collision: " << stage_durations[STAGE_get_collision]
            << "; STAGE_intersection_ln: " << stage_durations[STAGE_intersection_ln];

        return ss.str();
    }
private:
    boost::posix_time::ptime m_stage_start_times[NUM_STAGES];
};
TreeSupportProfiler profiler;

Lines spanning_tree_to_lines(const std::vector<MinimumSpanningTree>& spanning_trees)
{
    Lines polylines;
    for (const MinimumSpanningTree& mst : spanning_trees) {
        std::vector<Point> points = mst.vertices();
        std::unordered_set<Point, PointHash> to_ignore;
        for (Point pt1 : points) {
            if (to_ignore.find(pt1) != to_ignore.end())
                continue;

            const std::vector<Point>& neighbours = mst.adjacent_nodes(pt1);
            if (neighbours.empty())
                continue;

            for (Point pt2 : neighbours) {
                if (to_ignore.find(pt2) != to_ignore.end())
                    continue;

                Line line(pt1, pt2);
                polylines.push_back(line);
            }

            to_ignore.insert(pt1);
        }
    }
    return polylines;
}


#ifdef SUPPORT_TREE_DEBUG_TO_SVG
static void draw_contours_and_nodes_to_svg
(
    std::string fname,
    const ExPolygons &overhangs,
    const ExPolygons &overhangs_after_offset,
    const ExPolygons &outlines_below,
    const std::vector<SupportNode*>& layer_nodes,
    const std::vector<SupportNode*>& lower_layer_nodes,
    std::vector<std::string> legends = { "overhang","avoid","outlines" },
	std::vector<std::string> colors = { "blue","red","yellow" }
)
{
    BoundingBox bbox = get_extents(overhangs);
    bbox.merge(get_extents(overhangs_after_offset));
    bbox.merge(get_extents(outlines_below));
    Points layer_pts;
    for (SupportNode* node : layer_nodes) {
        layer_pts.push_back(node->position);
    }
    bbox.merge(get_extents(layer_pts));
    bbox.inflated(scale_(1));
    bbox.max.x() = std::max(bbox.max.x(), (coord_t)scale_(10));
    bbox.max.y() = std::max(bbox.max.y(), (coord_t)scale_(10));

    SVG svg;
    svg.open(fname, bbox);
    if (!svg.is_opened())        return;

    // draw grid
    svg.draw_grid(bbox, "gray", coord_t(scale_(0.05)));

    // draw overhang areas
    svg.draw_outline(overhangs, colors[0]);
    svg.draw_outline(overhangs_after_offset, colors[1]);
    svg.draw_outline(outlines_below, colors[2]);

    // draw legend
	svg.draw_text(bbox.min + Point(scale_(0), scale_(0)), format("nPoints: %1%->%2%",layer_nodes.size(), lower_layer_nodes.size()).c_str(), "green", 2);
    svg.draw_text(bbox.min + Point(scale_(0), scale_(2)), legends[0].c_str(), colors[0].c_str(), 2);
    svg.draw_text(bbox.min + Point(scale_(0), scale_(4)), legends[1].c_str(), colors[1].c_str(), 2);
    svg.draw_text(bbox.min + Point(scale_(0), scale_(6)), legends[2].c_str(), colors[2].c_str(), 2);

    // draw layer nodes
    svg.draw(layer_pts, "green", coord_t(scale_(0.1)));
    for (SupportNode *node : layer_nodes) { svg.draw({node->overhang}, "green", 0.5); }

    // lower layer points
    layer_pts.clear();
    for (SupportNode* node : lower_layer_nodes) {
        layer_pts.push_back(node->position);
    }
    svg.draw(layer_pts, "black", coord_t(scale_(0.1)));

    //// higher layer points
    //layer_pts.clear();
    //for (SupportNode* node : layer_nodes) {
    //    if(node->parent)
    //        layer_pts.push_back(node->parent->position);
    //}
    //svg.draw(layer_pts, "blue", coord_t(scale_(0.1)));
}

static void draw_layer_mst
(const std::string &fname,
    const std::vector<MinimumSpanningTree> &spanning_trees,
    const ExPolygons& outline
)
{
    auto lines = spanning_tree_to_lines(spanning_trees);
    BoundingBox bbox = get_extents(lines);
    for (auto& poly : outline)
    {
        BoundingBox bb = poly.contour.bounding_box();
        bbox.merge(bb);
    }

    SVG svg(fname, bbox);
    if (!svg.is_opened())        return;

    svg.draw(lines, "blue", coord_t(scale_(0.05)));
    svg.draw_outline(outline, "yellow");
    for (auto &spanning_tree : spanning_trees)
        svg.draw(spanning_tree.vertices(), "black", coord_t(scale_(0.1)));
}

#endif

// Move point from inside polygon if distance>0, outside if distance<0.
// Special case: distance=0 means find the nearest point of from on the polygon contour.
// The max move distance should not excceed max_move_distance.
// @return success(true) or not(false)
static bool move_inside_expoly(const ExPolygon &polygon, Point& from, double distance = 0, double max_move_distance = std::numeric_limits<double>::max())
{
    //TODO: This is copied from the moveInside of Polygons.
    /*
    We'd like to use this function as subroutine in moveInside(Polygons...), but
    then we'd need to recompute the distance of the point to the polygon, which
    is expensive. Or we need to return the distance. We need the distance there
    to compare with the distance to other polygons.
    */
    Point ret = from;
    double bestDist2 = std::numeric_limits<double>::max();
    bool is_already_on_correct_side_of_boundary = false; // whether [from] is already on the right side of the boundary
    const Polygon &contour = polygon.contour;

    if (contour.points.size() < 2)
    {
        return false;
    }
    Point p0 = contour.points[polygon.contour.size() - 2];
    Point p1 = contour.points.back();
    // because we compare with vsize2_with_unscale here (no division by zero), we also need to compare by vsize2_with_unscale inside the loop
    // to avoid integer rounding edge cases
    bool projected_p_beyond_prev_segment = dot_with_unscale(p1 - p0, from - p0) >= vsize2_with_unscale(p1 - p0);
    for(const Point& p2 : polygon.contour.points)
    {
        // X = A + Normal(B-A) * (((B-A) dot_with_unscale (P-A)) / VSize(B-A));
        //   = A +       (B-A) *  ((B-A) dot_with_unscale (P-A)) / VSize2(B-A);
        // X = P projected on AB
        const Point& a = p1;
        const Point& b = p2;
        const Point& p = from;
        Point ab = b - a;
        Point ap = p - a;
        double ab_length2 = vsize2_with_unscale(ab);
        if(ab_length2 <= 0) //A = B, i.e. the input polygon had two adjacent points on top of each other.
        {
            p1 = p2; //Skip only one of the points.
            continue;
        }
        double dot_prod = dot_with_unscale(ab, ap);
        if (dot_prod <= 0) // x is projected to before ab
        {
            if (projected_p_beyond_prev_segment)
            { //  case which looks like:   > .
                projected_p_beyond_prev_segment = false;
                Point& x = p1;

                double dist2 = vsize2_with_unscale(x - p);
                if (dist2 < bestDist2)
                {
                    bestDist2 = dist2;
                    if (distance == 0)
                    {
                        ret = x;
                    }
                    else
                    {
                        // TODO: check whether it needs scale_()
                        Point inward_dir = turn90_ccw(normal(ab, 10.0) + normal(p1 - p0, 10.0)); // inward direction irrespective of sign of [distance]
                        // MM2INT(10.0) to retain precision for the eventual normalization
                        ret = x + normal(inward_dir, scale_(distance));
                        is_already_on_correct_side_of_boundary = dot_with_unscale(inward_dir, p - x) * distance >= 0;
                    }
                }
            }
            else
            {
                projected_p_beyond_prev_segment = false;
                p0 = p1;
                p1 = p2;
                continue;
            }
        }
        else if (dot_prod >= ab_length2) // x is projected to beyond ab
        {
            projected_p_beyond_prev_segment = true;
            p0 = p1;
            p1 = p2;
            continue;
        }
        else
        { // x is projected to a point properly on the line segment (not onto a vertex). The case which looks like | .
            projected_p_beyond_prev_segment = false;
            Point x = a + ab * (dot_prod / ab_length2);

            double dist2 = vsize2_with_unscale(p - x);
            if (dist2 < bestDist2)
            {
                bestDist2 = dist2;
                if (distance == 0)
                {
                    ret = x;
                }
                else
                {
                    Point inward_dir = turn90_ccw(normal(ab, scale_(distance))); // inward or outward depending on the sign of [distance]
                    ret = x + inward_dir;
                    is_already_on_correct_side_of_boundary = dot_with_unscale(inward_dir, p - x) >= 0;
                }
            }
        }

        p0 = p1;
        p1 = p2;
    }

    if (is_already_on_correct_side_of_boundary) // when the best point is already inside and we're moving inside, or when the best point is already outside and we're moving outside
    {
        // BBS. Remove this condition.
        if (bestDist2 < distance * distance)
        {
            from = ret;
        }
        return true;
    }
    else if (bestDist2 < max_move_distance * max_move_distance)
    {
        from = ret;
        return true;
    }
    return false;
}

/*
 * Implementation assumes moving inside, but moving outside should just as well be possible.
 */
static bool move_inside_expolys(const ExPolygons& polygons, Point& from, double distance, double max_move_distance)
{
    Point from0 = from;
    Point ret = from;
    std::vector<Point> valid_pts;
    double bestDist2 = std::numeric_limits<double>::max();
    unsigned int bestPoly = NO_INDEX;
    bool is_already_on_correct_side_of_boundary = false; // whether [from] is already on the right side of the boundary
    Point inward_dir;
    for (unsigned int poly_idx = 0; poly_idx < polygons.size(); poly_idx++)
    {
        const ExPolygon poly = polygons[poly_idx];
        if (poly.contour.size() < 2)
            continue;
        Point p0 = poly.contour[poly.contour.size()-2];
        Point p1 = poly.contour.points.back();
        // because we compare with vsize2_with_unscale here (no division by zero), we also need to compare by vsize2_with_unscale inside the loop
        // to avoid integer rounding edge cases
        bool projected_p_beyond_prev_segment = dot_with_unscale(p1 - p0, from - p0) >= vsize2_with_unscale(p1 - p0);
        for(const Point& p2 : poly.contour.points)
        {
            // X = A + Normal(B-A) * (((B-A) dot_with_unscale (P-A)) / VSize(B-A));
            //   = A +       (B-A) *  ((B-A) dot_with_unscale (P-A)) / VSize2(B-A);
            // X = P projected on AB
            Point a = p1;
            Point b = p2;
            Point p = from;
            Point ab = b - a;
            Point ap = p - a;
            double ab_length2 = vsize2_with_unscale(ab);
            if(ab_length2 <= 0) //A = B, i.e. the input polygon had two adjacent points on top of each other.
            {
                p1 = p2; //Skip only one of the points.
                continue;
            }
            double dot_prod = dot_with_unscale(ab, ap);
            if (dot_prod <= 0) // x is projected to before ab
            {
                if (projected_p_beyond_prev_segment)
                { //  case which looks like:   > .
                    projected_p_beyond_prev_segment = false;
                    Point& x = p1;

                    double dist2 = vsize2_with_unscale(x - p);
                    if (dist2 < bestDist2)
                    {
                        bestDist2 = dist2;
                        bestPoly = poly_idx;
                        if (distance == 0) { ret = x; }
                        else
                        {
                            inward_dir = turn90_ccw(normal(ab, 10.0) + normal(p1 - p0, 10.0)); // inward direction irrespective of sign of [distance]
                            // MM2INT(10.0) to retain precision for the eventual normalization
                            ret = x + normal(inward_dir, scale_(distance));
                            is_already_on_correct_side_of_boundary = dot_with_unscale(inward_dir, p - x) * distance >= 0;
                            if (is_already_on_correct_side_of_boundary && dist2 < distance * distance)
                                valid_pts.push_back(ret-from0);
                        }
                    }
                }
                else
                {
                    projected_p_beyond_prev_segment = false;
                    p0 = p1;
                    p1 = p2;
                    continue;
                }
            }
            else if (dot_prod >= ab_length2) // x is projected to beyond ab
            {
                projected_p_beyond_prev_segment = true;
                p0 = p1;
                p1 = p2;
                continue;
            }
            else
            { // x is projected to a point properly on the line segment (not onto a vertex). The case which looks like | .
                projected_p_beyond_prev_segment = false;
                Point x = a + ab * (dot_prod / ab_length2);

                double dist2 = vsize2_with_unscale(p - x);
                if (dist2 < bestDist2)
                {
                    bestDist2 = dist2;
                    bestPoly = poly_idx;
                    if (distance == 0) { ret = x; }
                    else
                    {
                        inward_dir = turn90_ccw(normal(ab, scale_(distance))); // inward or outward depending on the sign of [distance]
                        ret = x + inward_dir;
                        is_already_on_correct_side_of_boundary = dot_with_unscale(inward_dir, p - x) >= 0;
                        if (is_already_on_correct_side_of_boundary && dist2<distance*distance)
                            valid_pts.push_back(ret-from0);
                    }
                }
            }
            p0 = p1;
            p1 = p2;
        }
    }

    //if (valid_pts.size() > 1) {
    //    std::sort(valid_pts.begin(), valid_pts.end());
    //    Point v_combine = valid_pts[0] + valid_pts[1];
    //    if(vsize2_with_unscale(v_combine)<distance*distance)
    //        v_combine = normal(v_combine, scale_(distance));
    //    ret = v_combine + from0;
    //}

    if (is_already_on_correct_side_of_boundary) // when the best point is already inside and we're moving inside, or when the best point is already outside and we're moving outside
    {
        if (bestDist2 < distance * distance)
        {
            from = ret;
        }
        return true;
    }
    else if (bestDist2 < max_move_distance * max_move_distance)
    {
        from = ret;
        return true;
    }
    return false;
}

static Point find_closest_ex(Point from, const ExPolygons& polygons)
{
    Point closest_pt;
    double min_dist2 = std::numeric_limits<double>::max();

    for (const ExPolygon &poly : polygons) {
        for (int i = 0; i < poly.num_contours(); i++) {
            const Point* candidate = poly.contour_or_hole(i).closest_point(from);
            double dist2 = vsize2_with_unscale(*candidate - from);
            if (dist2 < min_dist2) {
                closest_pt = *candidate;
                min_dist2 = dist2;
            }
        }
    }

    return closest_pt;
}

static bool move_outside_expolys(const ExPolygons& polygons, Point& from, double distance, double max_move_distance)
{
    return move_inside_expolys(polygons, from, -distance, -max_move_distance);
}

static bool is_inside_ex(const ExPolygon &polygon, const Point &pt)
{
    if (!get_extents(polygon).contains(pt))
        return false;

    return polygon.contains(pt);
}

static bool is_inside_ex(const ExPolygons &polygons, const Point &pt)
{
    for (const ExPolygon &poly : polygons) {
        if (is_inside_ex(poly, pt))
            return true;
    }

    return false;
}

// use project_onto which is more accurate but more expensive
static bool move_out_expolys(const ExPolygons& polygons, Point& from, double distance, double max_move_distance)
{
    Point from0 = from;
    ExPolygons polys_dilated = union_ex(offset_ex(polygons, scale_(distance)));
    Point pt = projection_onto(polys_dilated, from);// find_closest_ex(from, polys_dilated);
    Point outward_dir = pt - from;
    Point pt_max = from + normal(outward_dir, scale_(max_move_distance));
    double dist2 = vsize2_with_unscale(outward_dir);
    if (dist2 > SQ(max_move_distance))
        pt = pt_max;
    // case 5: already outside and far enough, no need to move
    if (!is_inside_ex(polys_dilated, from))
        return true;
    else if (!is_inside_ex(polygons, from)) {
        // case 4: already outside but not far enough
        from = pt;
        return true;
    }
    else {
        bool pt_max_in_poly = is_inside_ex(polygons, pt_max);
        if (!pt_max_in_poly) {
            from = pt_max;
            return true;
        }
        else {
            return false;
        }
    }
}

static Point bounding_box_middle(const BoundingBox &bbox)
{
    return (bbox.max + bbox.min) / 2;
}

TreeSupport::TreeSupport(PrintObject& object, const SlicingParameters &slicing_params)
    : m_object(&object), m_slicing_params(slicing_params), m_support_params(object), m_object_config(&object.config())
{
    m_print_config = &m_object->print()->config();
    m_raft_layers = slicing_params.base_raft_layers + slicing_params.interface_raft_layers;
    support_type = m_object_config->support_type;

    SupportMaterialPattern support_pattern  = m_object_config->support_base_pattern;
    if (m_support_params.support_style == smsTreeHybrid && support_pattern == smpDefault)
        support_pattern = smpRectilinear;

    if(support_pattern == smpLightning)
        m_support_params.base_fill_pattern = ipLightning;
    diameter_angle_scale_factor              = std::clamp<double>(m_object_config->tree_support_branch_diameter_angle * M_PI / 180., 0., 0.5 * M_PI - EPSILON);
    is_slim                                  = is_tree_slim(support_type, m_support_params.support_style);
    is_strong = is_tree(support_type) && m_support_params.support_style == smsTreeStrong;
    base_radius                              = std::max(MIN_BRANCH_RADIUS, m_object_config->tree_support_branch_diameter.value / 2);
    // by default tree support needs no infill, unless it's tree hybrid which contains normal nodes.
    with_infill                              = support_pattern != smpNone && support_pattern != smpDefault;
    m_machine_border.contour = get_bed_shape_with_excluded_area(*m_print_config);
    Vec3d plate_offset       = m_object->print()->get_plate_origin();
    // align with the centered object in current plate (may not be the 1st plate, so need to add the plate offset)
    m_machine_border.translate(Point(scale_(plate_offset(0)), scale_(plate_offset(1))) - m_object->instances().front().shift);
    top_z_distance                            = m_object_config->support_top_z_distance.value;
    if (top_z_distance > EPSILON) top_z_distance = std::max(top_z_distance, float(m_slicing_params.min_layer_height));
#ifdef SUPPORT_TREE_DEBUG_TO_SVG
    SVG svg(debug_out_path("machine_boarder.svg"), m_object->bounding_box());
    if (svg.is_opened()) svg.draw(m_machine_border, "yellow");
#endif
}


#define SUPPORT_SURFACES_OFFSET_PARAMETERS ClipperLib::jtSquare, 0.
void TreeSupport::detect_overhangs(bool check_support_necessity/* = false*/)
{
    bool tree_support_enable = m_object_config->enable_support.value && is_tree(m_object_config->support_type.value);
    if (!tree_support_enable && !check_support_necessity) {
        BOOST_LOG_TRIVIAL(info) << "Tree support is disabled.";
        return;
    }

    // Clear and create Tree Support Layers
    m_object->clear_support_layers();
    m_object->clear_tree_support_preview_cache();

    const PrintObjectConfig& config = m_object->config();
    SupportType stype = support_type;
    const coordf_t radius_sample_resolution = g_config_tree_support_collision_resolution;
    const double nozzle_diameter = m_object->print()->config().nozzle_diameter.get_at(0);
    const coordf_t extrusion_width = config.get_abs_value("line_width", nozzle_diameter);
    const coordf_t extrusion_width_scaled = scale_(extrusion_width);
    const coordf_t max_bridge_length = scale_(config.max_bridge_length.value);
    const bool bridge_no_support = max_bridge_length > 0;
    const bool support_critical_regions_only = config.support_critical_regions_only.value;
    bool config_remove_small_overhangs = config.support_remove_small_overhang.value;
    bool config_detect_sharp_tails = g_config_support_sharp_tails;
    const int enforce_support_layers = config.enforce_support_layers.value;
    const double area_thresh_well_supported = SQ(scale_(6));
    const double length_thresh_well_supported = scale_(6);
    static const double sharp_tail_max_support_height = 16.f;
    // a region is considered well supported if the number of layers below it exceeds this threshold
    const int thresh_layers_below = 10 / config.layer_height;
    // +1 makes the threshold inclusive
    double thresh_angle = config.support_threshold_angle.value > EPSILON ? config.support_threshold_angle.value + 1 : 30;
    thresh_angle = std::min(thresh_angle, 89.); // should be smaller than 90
    const double threshold_rad = Geometry::deg2rad(thresh_angle);
    // FIXME this is a fudge constant!
    double support_tree_tip_diameter = 0.8;
    auto   enforcer_overhang_offset  = scaled<double>(support_tree_tip_diameter);

    // for small overhang removal
    struct OverhangCluster {
        std::map<int, const ExPolygon*> layer_overhangs;
        ExPolygons merged_poly;
        BoundingBox merged_bbox;
        int min_layer = 1e7;
        int max_layer = 0;
        coordf_t offset = 0;
        bool is_cantilever = false;
        bool is_sharp_tail = false;
        bool is_small_overhang = false;
        OverhangCluster(const ExPolygon* expoly, int layer_nr) {
            push_back(expoly, layer_nr);
        }
        void push_back(const ExPolygon* expoly, int layer_nr) {
            layer_overhangs.emplace(layer_nr, expoly);
            auto dilate1 = offset_ex(*expoly, offset);
            if (!dilate1.empty())
                merged_poly = union_ex(merged_poly, dilate1);
            min_layer = std::min(min_layer, layer_nr);
            max_layer = std::max(max_layer, layer_nr);
            merged_bbox.merge(get_extents(dilate1));
        }
        int height() {
            return max_layer - min_layer + 1;
        }
        bool intersects(const ExPolygon& region, int layer_nr, coordf_t offset) {
            if (layer_nr < 1) return false;
            auto it = layer_overhangs.find(layer_nr - 1);
            if (it == layer_overhangs.end()) return false;
            const ExPolygon* overhang = it->second;

            this->offset = offset;
            auto dilate1 = offset_ex(region, offset);
            BoundingBox bbox = get_extents(dilate1);
            if (!merged_bbox.overlap(bbox))
                return false;
            return overlaps({ *overhang }, dilate1);
        }
        // it's basically the combination of push_back and intersects, but saves an offset_ex
        bool push_back_if_intersects(const ExPolygon& region, int layer_nr, coordf_t offset) {
            bool is_intersect = false;
            ExPolygons dilate1;
            BoundingBox bbox;
            do {
                if (layer_nr < 1) break;
                auto it = layer_overhangs.find(layer_nr - 1);
                if (it == layer_overhangs.end()) break;
                const ExPolygon* overhang = it->second;

                this->offset = offset;
                dilate1 = offset_ex(region, offset);
                if (dilate1.empty()) break;
                bbox = get_extents(dilate1);
                if (!merged_bbox.overlap(bbox))
                    break;
                is_intersect = overlaps({ *overhang }, dilate1);
            } while (0);
            if (is_intersect) {
                layer_overhangs.emplace(layer_nr, &region);
                merged_poly = union_ex(merged_poly, dilate1);
                min_layer = std::min(min_layer, layer_nr);
                max_layer = std::max(max_layer, layer_nr);
                merged_bbox.merge(bbox);
            }
            return is_intersect;
        }
    };
    std::vector<OverhangCluster> overhangClusters;

    auto find_and_insert_cluster = [](auto &regionClusters, const ExPolygon &region, int layer_nr, coordf_t offset) {
        OverhangCluster *cluster = nullptr;
        for (int i = 0; i < regionClusters.size(); i++) {
            auto cluster_i = &regionClusters[i];
            if (cluster_i->push_back_if_intersects(region, layer_nr, offset)) {
                cluster = cluster_i;
                break;
            }
        }
        if (!cluster) {
            cluster = &regionClusters.emplace_back(&region, layer_nr);
        }
        return cluster;
    };

    if (!is_tree(stype)) return;

    max_cantilever_dist = 0;
    m_highest_overhang_layer = 0;

    // calc the extrudable expolygons of each layer
    tbb::parallel_for(tbb::blocked_range<size_t>(0, m_object->layer_count()),
        [&](const tbb::blocked_range<size_t>& range) {
            for (size_t layer_nr = range.begin(); layer_nr < range.end(); layer_nr++) {
                if (m_object->print()->canceled())
                    break;
                Layer* layer = m_object->get_layer(layer_nr);
                // Filter out areas whose diameter that is smaller than extrusion_width, but we don't want to lose any details.
                layer->lslices_extrudable = intersection_ex(layer->lslices, offset2_ex(layer->lslices, -extrusion_width_scaled / 2, extrusion_width_scaled));
                layer->loverhangs.clear();
            }
        });

    typedef std::chrono::high_resolution_clock clock_;
    typedef std::chrono::duration<double, std::ratio<1> > second_;
    std::chrono::time_point<clock_> t0{ clock_::now() };
    // main part of overhang detection can be parallel
    tbb::concurrent_vector<ExPolygons> overhangs_all_layers(m_object->layer_count());
    tbb::parallel_for(tbb::blocked_range<size_t>(0, m_object->layer_count()),
        [&](const tbb::blocked_range<size_t>& range) {
            for (size_t layer_nr = range.begin(); layer_nr < range.end(); layer_nr++) {
                if (m_object->print()->canceled())
                    break;

                if (!is_auto(stype) && layer_nr > enforce_support_layers)
                    continue;

                Layer* layer = m_object->get_layer(layer_nr);

                if (layer->lower_layer == nullptr) {
                    for (auto& slice : layer->lslices_extrudable) {
                        auto bbox_size = get_extents(slice).size();
                        if (!((bbox_size.x() > length_thresh_well_supported && bbox_size.y() > length_thresh_well_supported))
                            && g_config_support_sharp_tails) {
                            layer->sharp_tails.push_back(slice);
                            layer->sharp_tails_height.push_back(layer->height);
                        }
                    }
                    continue;
                }

                Layer* lower_layer = layer->lower_layer;
                coordf_t lower_layer_offset = layer_nr < enforce_support_layers ? -0.15 * extrusion_width : (float)lower_layer->height / tan(threshold_rad);
                //lower_layer_offset = std::min(lower_layer_offset, extrusion_width);
                coordf_t support_offset_scaled = scale_(lower_layer_offset);
                ExPolygons& curr_polys = layer->lslices_extrudable;
                ExPolygons& lower_polys = lower_layer->lslices_extrudable;

                // normal overhang
                ExPolygons lower_layer_offseted = offset_ex(lower_polys, support_offset_scaled, SUPPORT_SURFACES_OFFSET_PARAMETERS);
                overhangs_all_layers[layer_nr] = std::move(diff_ex(curr_polys, lower_layer_offseted));

                double duration{ std::chrono::duration_cast<second_>(clock_::now() - t0).count() };
                if (duration > 30 || overhangs_all_layers[layer_nr].size() > 100) {
                    BOOST_LOG_TRIVIAL(info) << "detect_overhangs takes more than 30 secs, skip cantilever and sharp tails detection: layer_nr=" << layer_nr << " duration=" << duration;
                    config_detect_sharp_tails = false;
                    config_remove_small_overhangs = false;
                    continue;
                }
                if (is_auto(stype) && config_detect_sharp_tails)
                {
                    // BBS detect sharp tail
                    for (const ExPolygon& expoly : curr_polys) {
                        bool  is_sharp_tail = false;
                        // 1. nothing below
                        // this is a sharp tail region if it's floating and non-ignorable
                        if (!overlaps(offset_ex(expoly, 0.1 * extrusion_width_scaled), lower_polys)) {
                            is_sharp_tail = !offset_ex(expoly, -0.1 * extrusion_width_scaled).empty();
                        }

                        if (is_sharp_tail) {
                            layer->sharp_tails.push_back(expoly);
                            layer->sharp_tails_height.push_back(0);

                            has_sharp_tails = true;
#ifdef SUPPORT_TREE_DEBUG_TO_SVG
							SVG::export_expolygons(debug_out_path("sharp_tail_orig_%.02f.svg", layer->print_z), { expoly });
#endif
						}
                    }
                }


                // check cantilever
                // lower_layer_offset may be very small, so we need to do max and then add 0.1
                lower_layer_offseted = offset_ex(lower_layer_offseted, scale_(std::max(extrusion_width - lower_layer_offset, 0.) + 0.1));
                for (ExPolygon& poly : overhangs_all_layers[layer_nr]) {
                    auto cluster_boundary_ex = intersection_ex(poly, lower_layer_offseted);
                    Polygons cluster_boundary = to_polygons(cluster_boundary_ex);
                    if (cluster_boundary.empty()) continue;
                    double dist_max = 0;
                    for (auto& pt : poly.contour.points) {
                        double dist_pt = std::numeric_limits<double>::max();
                        for (auto& ply : cluster_boundary) {
                            double d = ply.distance_to(pt);
                            dist_pt = std::min(dist_pt, d);
                        }
                        dist_max = std::max(dist_max, dist_pt);
                    }
                    if (dist_max > scale_(3)) {  // is cantilever if the farmost point is larger than 3mm away from base
                        max_cantilever_dist = std::max(max_cantilever_dist, dist_max);
                        layer->cantilevers.emplace_back(poly);
                        BOOST_LOG_TRIVIAL(debug) << "found a cantilever cluster. layer_nr=" << layer_nr << dist_max;
                        has_cantilever = true;
                    }
                }
            }
        }
    ); // end tbb::parallel_for

    BOOST_LOG_TRIVIAL(info) << "max_cantilever_dist=" << max_cantilever_dist;
    if (check_support_necessity)
        return;

    // check if the sharp tails should be extended higher
    if (is_auto(stype) && config_detect_sharp_tails) {
        for (size_t layer_nr = 0; layer_nr < m_object->layer_count(); layer_nr++) {
            if (m_object->print()->canceled())
                break;

            Layer* layer = m_object->get_layer(layer_nr);
            Layer* lower_layer = layer->lower_layer;
            if (!lower_layer)
                continue;

            // BBS detect sharp tail
            const ExPolygons& lower_layer_sharptails = lower_layer->sharp_tails;
            const auto& lower_layer_sharptails_height = lower_layer->sharp_tails_height;
            for (ExPolygon& expoly : layer->lslices_extrudable) {
                bool  is_sharp_tail = false;
                float accum_height = layer->height;
                do {
                    // 2. something below
                    // check whether this is above a sharp tail region.

                    // 2.1 If no sharp tail below, this is considered as common region.
                    ExPolygons supported_by_lower = intersection_ex({ expoly }, lower_layer_sharptails);
                    if (supported_by_lower.empty()) {
                        is_sharp_tail = false;
                        break;
                    }

                    // 2.2 If sharp tail below, check whether it support this region enough.
#if 0
                    // judge by area isn't reliable, failure cases include 45 degree rotated cube
                    float       supported_area = area(supported_by_lower);
                    if (supported_area > area_thresh_well_supported) {
                        is_sharp_tail = false;
                        break;
                    }
#endif
                    BoundingBox bbox = get_extents(supported_by_lower);
                    if (bbox.size().x() > length_thresh_well_supported && bbox.size().y() > length_thresh_well_supported) {
                        is_sharp_tail = false;
                        break;
                    }

                    // 2.3 check whether sharp tail exceed the max height
                    for(size_t i=0;i<lower_layer_sharptails.size();i++) {
                        if (lower_layer_sharptails[i].overlaps(expoly)) {
                            accum_height += lower_layer_sharptails_height[i];
                            break;
                        }
                    }
                    if (accum_height > sharp_tail_max_support_height) {
                        is_sharp_tail = false;
                        break;
                    }

                    // 2.4 if the area grows fast than threshold, it get connected to other part or
                    // it has a sharp slop and will be auto supported.
                    ExPolygons new_overhang_expolys = diff_ex({ expoly }, lower_layer_sharptails);
                    if ((get_extents(new_overhang_expolys).size() - get_extents(lower_layer_sharptails).size()).both_comp(Point(scale_(5), scale_(5)), ">") || !offset_ex(new_overhang_expolys, -5.0 * extrusion_width_scaled).empty()) {
                        is_sharp_tail = false;
                        break;
                    }

                    // 2.5 mark the expoly as sharptail
                    is_sharp_tail = true;
                } while (0);

                if (is_sharp_tail) {
                    layer->sharp_tails.push_back(expoly);
                    layer->sharp_tails_height.push_back( accum_height);
                }

            }
        }
    }

    // group overhang clusters
    for (size_t layer_nr = 0; layer_nr < m_object->layer_count(); layer_nr++) {
        if (m_object->print()->canceled())
            break;
        Layer* layer = m_object->get_layer(layer_nr);
        for (auto& overhang : overhangs_all_layers[layer_nr]) {
            OverhangCluster* cluster = find_and_insert_cluster(overhangClusters, overhang, layer_nr, extrusion_width_scaled);
            if (overlaps({ overhang }, layer->cantilevers))
                cluster->is_cantilever = true;
        }
    }

    auto enforcers = m_object->slice_support_enforcers();
    auto blockers  = m_object->slice_support_blockers();
    m_vertical_enforcer_points.clear();
    m_object->project_and_append_custom_facets(false, EnforcerBlockerType::ENFORCER, enforcers, &m_vertical_enforcer_points);
    m_object->project_and_append_custom_facets(false, EnforcerBlockerType::BLOCKER, blockers);

    if (is_auto(stype) && config_remove_small_overhangs) {
        // remove small overhangs
        for (auto& cluster : overhangClusters) {
            // 3. check whether the small overhang is sharp tail
            cluster.is_sharp_tail = false;
            for (size_t layer_id = cluster.min_layer; layer_id <= cluster.max_layer; layer_id++) {
                Layer* layer = m_object->get_layer(layer_id);
                if (overlaps(layer->sharp_tails, cluster.merged_poly)) {
                    cluster.is_sharp_tail = true;
                    break;
                }
            }

            if (!cluster.is_sharp_tail && !cluster.is_cantilever) {
                // 2. check overhang cluster size is smaller than 3.0 * fw_scaled
                auto erode1 = offset_ex(cluster.merged_poly, -1 * extrusion_width_scaled);
                Point bbox_sz = get_extents(erode1).size();
                if (bbox_sz.x() < 2 * extrusion_width_scaled || bbox_sz.y() < 2 * extrusion_width_scaled) {
                    cluster.is_small_overhang = true;
                }
            }

#ifdef SUPPORT_TREE_DEBUG_TO_SVG
            const Layer* layer1 = m_object->get_layer(cluster.min_layer);
            std::string fname = debug_out_path("overhangCluster_%d-%d_%.2f-%.2f_tail=%d_cantilever=%d_small=%d.svg",
                cluster.min_layer, cluster.max_layer, layer1->print_z, m_object->get_layer(cluster.max_layer)->print_z,
                cluster.is_sharp_tail, cluster.is_cantilever, cluster.is_small_overhang);
            SVG::export_expolygons(fname, {
                { layer1->lslices, {"min_layer_lslices","red",0.5} },
                { m_object->get_layer(cluster.max_layer)->lslices, {"max_layer_lslices","yellow",0.5} },
                { cluster.merged_poly,{"overhang", "blue", 0.5} },
                { cluster.is_cantilever? layer1->cantilevers: offset_ex(cluster.merged_poly, -1 * extrusion_width_scaled), {cluster.is_cantilever ? "cantilever":"erode1","green",0.5}} });
#endif
        }
    }

    for (auto& cluster : overhangClusters) {
        if (cluster.is_small_overhang) continue;
        // collect overhangs that's not small overhangs
        for (auto it = cluster.layer_overhangs.begin(); it != cluster.layer_overhangs.end(); it++) {
            int  layer_nr   = it->first;
            auto p_overhang = it->second;
            m_object->get_layer(layer_nr)->loverhangs.emplace_back(*p_overhang);
        }
    }

    int layers_with_overhangs = 0;
    int layers_with_enforcers = 0;
    for (int layer_nr = 0; layer_nr < m_object->layer_count(); layer_nr++) {
        if (m_object->print()->canceled())
            break;

        auto layer = m_object->get_layer(layer_nr);
        auto lower_layer = layer->lower_layer;

        // add support for every 1mm height for sharp tails
        ExPolygons sharp_tail_overhangs;
        if (lower_layer == nullptr)
            sharp_tail_overhangs = layer->sharp_tails;
        else {
            ExPolygons lower_layer_expanded = offset_ex(lower_layer->lslices_extrudable, SCALED_RESOLUTION);
            for (size_t i = 0; i < layer->sharp_tails_height.size();i++) {
                ExPolygons areas = diff_clipped({ layer->sharp_tails[i]}, lower_layer_expanded);
                float accum_height = layer->sharp_tails_height[i];
                if (!areas.empty() && int(accum_height * 10) % 5 == 0) {
                    append(sharp_tail_overhangs, areas);
                    has_sharp_tails = true;
#ifdef SUPPORT_TREE_DEBUG_TO_SVG
                    SVG::export_expolygons(debug_out_path("sharp_tail_%.02f.svg", layer->print_z), areas);
#endif
                }
            }
        }

        if (layer_nr < blockers.size()) {
            // Arthur: union_ is a must because after mirroring, the blocker polygons are in left-hand coordinates, ie clockwise,
            // which are not valid polygons, and will be removed by offset_ex. union_ can make these polygons right.
            ExPolygons blocker = offset_ex(union_(blockers[layer_nr]), scale_(radius_sample_resolution));
            layer->loverhangs = diff_ex(layer->loverhangs, blocker);
            layer->cantilevers = diff_ex(layer->cantilevers, blocker);
            sharp_tail_overhangs = diff_ex(sharp_tail_overhangs, blocker);
        }

        if (support_critical_regions_only && is_auto(stype)) {
            layer->loverhangs.clear();  // remove oridinary overhangs, only keep cantilevers and sharp tails (added later)
            append(layer->loverhangs, layer->cantilevers);
        }

        if (max_bridge_length > 0 && layer->loverhangs.size() > 0 && lower_layer) {
            // do not break bridge as the interface will be poor, see #4318
            bool break_bridge = false;
            m_object->remove_bridges_from_contacts(lower_layer, layer, extrusion_width_scaled, &layer->loverhangs, max_bridge_length, break_bridge);
        }

		int nDetected = layer->loverhangs.size();
        // enforcers now follow same logic as normal support. See STUDIO-3692
        if (layer_nr < enforcers.size() && lower_layer) {
            ExPolygons enforced_overhangs   = intersection_ex(diff_ex(layer->lslices_extrudable, lower_layer->lslices_extrudable), enforcers[layer_nr]);
            if (!enforced_overhangs.empty()) {
                // FIXME this is a hack to make enforcers work on steep overhangs. See STUDIO-7538.
                enforced_overhangs = diff_ex(offset_ex(enforced_overhangs, enforcer_overhang_offset), lower_layer->lslices_extrudable);
                append(layer->loverhangs, enforced_overhangs);
            }
        }
        int nEnforced = layer->loverhangs.size();

        // add sharp tail overhangs
        append(layer->loverhangs, sharp_tail_overhangs);

        // fill overhang_types
        for (size_t i = 0; i < layer->loverhangs.size(); i++)
            overhang_types.emplace(&layer->loverhangs[i], i < nDetected ? OverhangType::Detected :
                i < nEnforced ? OverhangType::Enforced : OverhangType::SharpTail);

        if (!layer->loverhangs.empty()) {
            layers_with_overhangs++;
            m_highest_overhang_layer = std::max(m_highest_overhang_layer, size_t(layer_nr));
        }
        if (nEnforced > 0) layers_with_enforcers++;
        if (!layer->cantilevers.empty()) has_cantilever = true;
    }

    BOOST_LOG_TRIVIAL(info) << "Tree support overhang detection done. " << layers_with_overhangs << " layers with overhangs. nEnforced=" << layers_with_enforcers;

#ifdef SUPPORT_TREE_DEBUG_TO_SVG
    for (const Layer* layer : m_object->layers()) {
        if (layer->loverhangs.empty() && (blockers.size()<=layer->id() || blockers[layer->id()].empty()))
            continue;

        SVG::export_expolygons(debug_out_path("overhang_areas_%d_%.2f.svg",layer->id(), layer->print_z), {
            { m_object->get_layer(layer->id())->lslices_extrudable, {"lslices_extrudable","yellow",0.5} },
            { layer->loverhangs, {"overhang","red",0.5} }
            });

        if (enforcers.size() > layer->id()) {
            SVG svg(format("SVG/enforcer_%s.svg", layer->print_z), m_object->bounding_box());
            if (svg.is_opened()) {
                svg.draw_outline(m_object->get_layer(layer->id())->lslices_extrudable, "yellow");
                svg.draw(enforcers[layer->id()], "red");
            }
        }
        if (blockers.size() > layer->id()) {
            SVG svg(format("SVG/blocker_%s.svg", layer->print_z), m_object->bounding_box());
            if (svg.is_opened()) {
                svg.draw_outline(m_object->get_layer(layer->id())->lslices_extrudable, "yellow");
                svg.draw(blockers[layer->id()], "red");
            }
        }
    }
#endif
}

// create support layers for raft and tree support
// if support is disabled, only raft layers are created
void TreeSupport::create_tree_support_layers()
{
    int layer_id = 0;
    if (m_raft_layers > 0) { //create raft layers
        coordf_t raft_print_z = 0.f;
        coordf_t raft_slice_z = 0.f;
        {
            // Do not add the raft contact layer, 1st layer should use first_print_layer_height
            coordf_t height = m_slicing_params.first_print_layer_height;
            raft_print_z += height;
            raft_slice_z = raft_print_z - height / 2;
            m_object->add_tree_support_layer(layer_id++, height, raft_print_z, raft_slice_z);
        }
        // Insert the base layers.
        for (size_t i = 1; i < m_slicing_params.base_raft_layers; i++) {
            coordf_t height = m_slicing_params.base_raft_layer_height;
            raft_print_z += height;
            raft_slice_z = raft_print_z - height / 2;
            m_object->add_tree_support_layer(layer_id++, height, raft_print_z, raft_slice_z);
        }
        // Insert the interface layers.
        for (size_t i = 0; i < m_slicing_params.interface_raft_layers; i++) {
            coordf_t height = m_slicing_params.interface_raft_layer_height;
            raft_print_z += height;
            raft_slice_z = raft_print_z - height / 2;
            m_object->add_tree_support_layer(layer_id++, height, raft_print_z, raft_slice_z);
        }

        // Layers between the raft contacts and bottom of the object.
        double dist_to_go = m_slicing_params.object_print_z_min - raft_print_z;
        auto nsteps = int(ceil(dist_to_go / m_slicing_params.max_suport_layer_height));
        double height = dist_to_go / nsteps;
        for (int i = 0; i < nsteps; ++i) {
            raft_print_z += height;
            raft_slice_z = raft_print_z - height / 2;
            m_object->add_tree_support_layer(layer_id++, height, raft_print_z, raft_slice_z);
        }
        m_raft_layers = layer_id;
    }
}

static inline BoundingBox fill_expolygon_generate_paths(
    ExtrusionEntitiesPtr    &dst,
    ExPolygon               &expolygon,
    Fill                    *filler,
    const FillParams        &fill_params,
    ExtrusionRole            role,
    const Flow              &flow)
{
    Surface surface(stInternal, std::move(expolygon));
    Polylines polylines;
    try {
        polylines = filler->fill_surface(&surface, fill_params);
    } catch (InfillFailedException &) {
    }

    BoundingBox fill_bbox;
    if (!polylines.empty()) {
        fill_bbox = polylines[0].bounding_box();
        for (auto& polyline : polylines)
            fill_bbox.merge(polyline.bounding_box());
    }

    extrusion_entities_append_paths(dst, std::move(polylines), role, flow.mm3_per_mm(), flow.width(), flow.height());

    return fill_bbox;
}

static inline std::vector<BoundingBox> fill_expolygons_generate_paths(
    ExtrusionEntitiesPtr   &dst,
    ExPolygons             &expolygons,
    Fill                   *filler,
    const FillParams       &fill_params,
    ExtrusionRole           role,
    const Flow             &flow)
{
    std::vector<BoundingBox> fill_boxes;
    for (ExPolygon& expoly : expolygons) {
        auto box = fill_expolygon_generate_paths(dst, expoly, filler, fill_params, role, flow);
        fill_boxes.emplace_back(box);
    }
    return fill_boxes;
}

static void _make_loops(ExtrusionEntitiesPtr& loops_entities, ExPolygons &support_area, ExtrusionRole role, size_t wall_count, const Flow &flow)
{
    Polygons       loops;
    std::map<ExPolygon *, int> depth_per_expoly;
    std::list<ExPolygon> expoly_list;

    for (ExPolygon &expoly : support_area) {
        expoly_list.emplace_back(std::move(expoly));
        depth_per_expoly.insert({&expoly_list.back(), 0});
    }
    if (expoly_list.empty()) return;

    while (!expoly_list.empty()) {
        polygons_append(loops, to_polygons(expoly_list.front()));

        auto first_iter = expoly_list.begin();
        auto depth_iter = depth_per_expoly.find(&expoly_list.front());
        if (depth_iter->second + 1 < wall_count) {
            //ExPolygons expolys_new = offset_ex(expoly_list.front(), -float(flow.scaled_spacing()), jtSquare);
            // shrink and then dilate to prevent overlapping and overflow
            ExPolygons expolys_new = offset2_ex({expoly_list.front()}, -1.4 * float(flow.scaled_spacing()), .4 * float(flow.scaled_spacing()));

            for (ExPolygon &expoly : expolys_new) {
                auto new_iter = expoly_list.insert(expoly_list.begin(), expoly);
                depth_per_expoly.insert({&*new_iter, depth_iter->second + 1});
            }
        }
        depth_per_expoly.erase(depth_iter);
        expoly_list.erase(first_iter);
    }

    extrusion_entities_append_loops(loops_entities, std::move(loops), role, float(flow.mm3_per_mm()), float(flow.width()), float(flow.height()));

}

static void make_perimeter_and_inner_brim(ExtrusionEntitiesPtr &dst, const ExPolygon &support_area, size_t wall_count, const Flow &flow, ExtrusionRole role)
{
    Polygons   loops;
    ExPolygons support_area_new = offset_ex(support_area, -0.5f * float(flow.scaled_spacing()), jtSquare);
    _make_loops(dst, support_area_new, role, wall_count, flow);
}

static void make_perimeter_and_infill(ExtrusionEntitiesPtr& dst, const ExPolygon& support_area, size_t wall_count, const Flow& flow, ExtrusionRole role, Fill* filler_support, double support_density, bool infill_first=true)
{
    Polygons   loops;
    ExPolygons support_area_new = offset_ex(support_area, -0.5f * float(flow.scaled_spacing()), jtSquare);

    // draw infill
    FillParams fill_params;
    fill_params.density = support_density;
    fill_params.dont_adjust = true;
    ExPolygons to_infill = offset_ex(support_area, -float(wall_count) * float(flow.scaled_spacing()), jtSquare);
    std::vector<BoundingBox> fill_boxes = fill_expolygons_generate_paths(dst, to_infill, filler_support, fill_params, role, flow);

    // allow wall_count to be zero, which means only draw infill
    if (wall_count == 0) {
        for (auto fill_bbox : fill_boxes)
        {
            // extend bounding box on x-axis
            if (cos(filler_support->angle)>=sin(filler_support->angle)) {
                fill_bbox.min[0] -= scale_(10);
                fill_bbox.max[0] += scale_(10);
            }
            else {
                fill_bbox.min[1] -= scale_(10);
                fill_bbox.max[1] += scale_(10);
            }
            support_area_new = diff_ex(support_area_new, offset_ex(to_expolygons({ fill_bbox.polygon() }), 0.5*flow.scaled_width()));
        }
        // filter out small areas
        for (auto it = support_area_new.begin(); it != support_area_new.end(); ) {
            if (offset_ex(*it, -flow.scaled_width()).empty())
                it = support_area_new.erase(it);
            else
                it++;
        }
    }

    { // draw loops
        ExtrusionEntitiesPtr loops_entities;
        _make_loops(loops_entities, support_area_new, role, wall_count, flow);

        if (infill_first)
            dst.insert(dst.end(), loops_entities.begin(), loops_entities.end());
        else { // loops first
            loops_entities.insert(loops_entities.end(), dst.begin(), dst.end());
            dst = std::move(loops_entities);
        }
    }
    dst.erase(std::remove_if(dst.begin(), dst.end(), [](ExtrusionEntity *entity) { return static_cast<ExtrusionEntityCollection *>(entity)->empty(); }), dst.end());
    if (infill_first) {
        // sort regions to reduce travel
        Points ordering_points;
        for (const auto& area : dst)
            ordering_points.push_back(area->first_point());
        std::vector<Points::size_type> order = chain_points(ordering_points);
        ExtrusionEntitiesPtr new_dst;
        new_dst.reserve(ordering_points.size());
        for (size_t i : order)
            new_dst.emplace_back(dst[i]);
        dst = new_dst;
    }
}

void TreeSupport::generate_toolpaths()
{
    const PrintObjectConfig &object_config = m_object->config();
    coordf_t support_extrusion_width = m_support_params.support_extrusion_width;
    coordf_t nozzle_diameter = m_print_config->nozzle_diameter.get_at(object_config.support_filament - 1);
    coordf_t layer_height = object_config.layer_height.value;
    const size_t wall_count = object_config.tree_support_wall_count.value;

    // Check if set to zero, use default if so.
    if (support_extrusion_width <= 0.0)
        support_extrusion_width = Flow::auto_extrusion_width(FlowRole::frSupportMaterial, (float)nozzle_diameter);

    // coconut: use same intensity settings as SupportMaterial.cpp
    auto m_support_material_interface_flow = support_material_interface_flow(m_object, float(m_slicing_params.layer_height));
    coordf_t interface_spacing = object_config.support_interface_spacing.value + m_support_material_interface_flow.spacing();
    coordf_t bottom_interface_spacing = object_config.support_bottom_interface_spacing.value + m_support_material_interface_flow.spacing();
    coordf_t interface_density = std::min(1., m_support_material_interface_flow.spacing() / interface_spacing);
    coordf_t bottom_interface_density = std::min(1., m_support_material_interface_flow.spacing() / bottom_interface_spacing);

    const coordf_t branch_radius = object_config.tree_support_branch_diameter.value / 2;
    const coordf_t branch_radius_scaled = scale_(branch_radius);

    if (m_object->support_layers().empty())
        return;

    // calculate fill areas for raft layers
    ExPolygons raft_areas;
    if (m_object->layer_count() > 0) {
        const Layer *layer = m_object->layers().front();
        for (const ExPolygon &expoly : layer->lslices) {
            raft_areas.push_back(expoly);
        }
    }

    if (m_object->support_layer_count() > m_raft_layers) {
        const SupportLayer *ts_layer = m_object->get_support_layer(m_raft_layers);
        for (const ExPolygon& expoly : ts_layer->floor_areas)
            raft_areas.push_back(expoly);
        for (const ExPolygon& expoly : ts_layer->roof_areas)
            raft_areas.push_back(expoly);
        for (const ExPolygon& expoly : ts_layer->base_areas)
            raft_areas.push_back(expoly);
    }

    raft_areas = std::move(offset_ex(raft_areas, scale_(object_config.raft_first_layer_expansion)));

    size_t layer_nr = 0;
    for (; layer_nr < m_slicing_params.base_raft_layers; layer_nr++) {
        SupportLayer *ts_layer = m_object->get_support_layer(layer_nr);
        coordf_t expand_offset = (layer_nr == 0 ? m_object_config->raft_first_layer_expansion.value : 0.);
        auto raft_areas1 = offset_ex(raft_areas, scale_(expand_offset));

        Flow support_flow = Flow(support_extrusion_width, ts_layer->height, nozzle_diameter);
        Fill* filler_raft = Fill::new_from_type(ipRectilinear);
        filler_raft->angle = layer_nr == 0 ? PI/2 : 0;
        filler_raft->spacing = support_flow.spacing();

        FillParams fill_params;
        coordf_t raft_density = std::min(1., support_flow.spacing() / (object_config.support_base_pattern_spacing.value + support_flow.spacing()));
        fill_params.density = layer_nr == 0 ? object_config.raft_first_layer_density * 0.01 : raft_density;
        fill_params.dont_adjust = true;

        // wall of first layer raft
        if (layer_nr == 0) {
            Flow flow = Flow(support_extrusion_width, ts_layer->height, nozzle_diameter);
            extrusion_entities_append_loops(ts_layer->support_fills.entities, to_polygons(raft_areas1), erSupportMaterial,
                        float(flow.mm3_per_mm()), float(flow.width()), float(flow.height()));
            raft_areas1 = offset_ex(raft_areas1, -flow.scaled_spacing() / 2.);
        }
        fill_expolygons_generate_paths(ts_layer->support_fills.entities, raft_areas1,
            filler_raft, fill_params, erSupportMaterial, support_flow);
    }

    // subtract the non-raft support bases, otherwise we'll get support base on top of raft interfaces which is not stable
    ExPolygons first_non_raft_base;
    SupportLayer* first_non_raft_layer = m_object->get_support_layer(m_raft_layers);
    if (first_non_raft_layer) {
        for (auto& area_group : first_non_raft_layer->area_groups) {
            if (area_group.type == SupportLayer::BaseType)
                first_non_raft_base.emplace_back(*area_group.area);
        }
    }
    first_non_raft_base = offset_ex(first_non_raft_base, support_extrusion_width);
    ExPolygons raft_base_areas = intersection_ex(raft_areas, first_non_raft_base);
    ExPolygons raft_interface_areas = diff_ex(raft_areas, raft_base_areas);


    // raft interfaces
    for (layer_nr = m_slicing_params.base_raft_layers;
         layer_nr < m_slicing_params.base_raft_layers + m_slicing_params.interface_raft_layers;
         layer_nr++)
    {
        SupportLayer *ts_layer = m_object->get_support_layer(layer_nr);

        Flow support_flow(support_extrusion_width, ts_layer->height, nozzle_diameter);
        Fill* filler_interface = Fill::new_from_type(ipRectilinear);
        filler_interface->angle = PI / 2;  // interface should be perpendicular to base
        filler_interface->spacing = support_flow.spacing();

        FillParams fill_params;
        fill_params.density = interface_density;
        fill_params.dont_adjust = true;

        fill_expolygons_generate_paths(ts_layer->support_fills.entities, raft_interface_areas,
            filler_interface, fill_params, erSupportMaterialInterface, support_flow);

        fill_params.density = object_config.raft_first_layer_density * 0.01;
        fill_expolygons_generate_paths(ts_layer->support_fills.entities, raft_base_areas,
            filler_interface, fill_params, erSupportMaterial, support_flow);
    }

    // layers between raft and object
    for (; layer_nr < m_raft_layers; layer_nr++) {
        SupportLayer *ts_layer = m_object->get_support_layer(layer_nr);
        Flow support_flow(support_extrusion_width, ts_layer->height, nozzle_diameter);
        Fill* filler_raft = Fill::new_from_type(ipRectilinear);
        filler_raft->angle = PI / 2;
        filler_raft->spacing = support_flow.spacing();
        for (auto& poly : first_non_raft_base)
            make_perimeter_and_infill(ts_layer->support_fills.entities, poly, std::min(size_t(1), wall_count), support_flow, erSupportMaterial, filler_raft, interface_density, false);
    }

    if (m_object->support_layer_count() <= m_raft_layers)
        return;

    BoundingBox bbox_object(Point(-scale_(1.), -scale_(1.0)), Point(scale_(1.), scale_(1.)));

    std::shared_ptr<Fill> filler_interface = std::shared_ptr<Fill>(Fill::new_from_type(m_support_params.contact_fill_pattern));
    std::shared_ptr<Fill> filler_Roof1stLayer = std::shared_ptr<Fill>(Fill::new_from_type(ipRectilinear));
    filler_interface->set_bounding_box(bbox_object);
    filler_Roof1stLayer->set_bounding_box(bbox_object);
    filler_interface->angle = Geometry::deg2rad(object_config.support_angle.value + 90.);
    filler_Roof1stLayer->angle = Geometry::deg2rad(object_config.support_angle.value + 90.);

    // generate tree support tool paths
    tbb::parallel_for(
        tbb::blocked_range<size_t>(m_raft_layers, m_object->support_layer_count()),
        [&](const tbb::blocked_range<size_t>& range)
        {
            for (size_t layer_id = range.begin(); layer_id < range.end(); layer_id++) {
                if (m_object->print()->canceled())
                    break;

                //m_object->print()->set_status(70, (boost::format(_u8L("Support: generate toolpath at layer %d")) % layer_id).str());

                SupportLayer* ts_layer = m_object->get_support_layer(layer_id);
                Flow support_flow(support_extrusion_width, ts_layer->height, nozzle_diameter);
                Flow interface_flow = support_material_interface_flow(m_object, ts_layer->height); // update flow using real support layer height
                coordf_t support_spacing         = object_config.support_base_pattern_spacing.value + support_flow.spacing();
                coordf_t support_density         = std::min(1., support_flow.spacing() / support_spacing);
                ts_layer->support_fills.no_sort = false;

                for (auto& area_group : ts_layer->area_groups) {
                    ExPolygon& poly = *area_group.area;
                    ExPolygons polys;
                    FillParams fill_params;
                    if (area_group.type != SupportLayer::BaseType) {
                        // interface
                        if (layer_id == 0) {
                            Flow flow = m_raft_layers == 0 ? m_object->print()->brim_flow() : support_flow;
                            make_perimeter_and_inner_brim(ts_layer->support_fills.entities, poly, wall_count, flow,
                                                          area_group.type == SupportLayer::RoofType ? erSupportMaterialInterface : erSupportMaterial);
                            polys = std::move(offset_ex(poly, -flow.scaled_spacing()));
                        } else if (area_group.type == SupportLayer::Roof1stLayer) {
                            polys = std::move(offset_ex(poly, 0.5*support_flow.scaled_width()));
                        }
                        else {
                            polys.push_back(poly);
                        }
                        fill_params.density = interface_density;
                        fill_params.dont_adjust = true;
                    }
                    if (area_group.type == SupportLayer::Roof1stLayer) {
                        // roof_1st_layer
                        fill_params.density = interface_density;
                        // Note: spacing means the separation between two lines as if they are tightly extruded
                        filler_Roof1stLayer->spacing = interface_flow.spacing();
                        // generate a perimeter first to support interface better
                        ExtrusionEntityCollection* temp_support_fills = new ExtrusionEntityCollection();
                        make_perimeter_and_infill(temp_support_fills->entities, poly, 1, interface_flow, erSupportMaterial,
                            filler_Roof1stLayer.get(), interface_density, false);
                        temp_support_fills->no_sort = true; // make sure loops are first
                        if (!temp_support_fills->entities.empty())
                            ts_layer->support_fills.entities.push_back(temp_support_fills);
                        else
                            delete temp_support_fills;
                    } else if (area_group.type == SupportLayer::FloorType) {
                        // floor_areas
                        fill_params.density = bottom_interface_density;
                        filler_interface->spacing = interface_flow.spacing();
                        fill_expolygons_generate_paths(ts_layer->support_fills.entities, polys,
                            filler_interface.get(), fill_params, erSupportMaterialInterface, interface_flow);
                    } else if (area_group.type == SupportLayer::RoofType) {
                        // roof_areas
                        fill_params.density       = interface_density;
                        filler_interface->spacing = interface_flow.spacing();
                        if (m_object_config->support_interface_pattern == smipGrid) {
                            filler_interface->angle = Geometry::deg2rad(object_config.support_angle.value);
                            fill_params.dont_sort = true;
                        }
                        if (m_object_config->support_interface_pattern == smipRectilinearInterlaced)
                            filler_interface->layer_id = area_group.interface_id;

                        fill_expolygons_generate_paths(ts_layer->support_fills.entities, polys, filler_interface.get(), fill_params, erSupportMaterialInterface,
                                                       interface_flow);
                    }
                    else {
                        // base_areas
                        Flow flow               = (layer_id == 0 && m_raft_layers == 0) ? m_object->print()->brim_flow() : support_flow;
                        bool need_infill = with_infill;
                        if(m_object_config->support_base_pattern==smpDefault)
                            need_infill &= area_group.need_infill;
                        std::shared_ptr<Fill> filler_support = std::shared_ptr<Fill>(Fill::new_from_type(layer_id == 0 ? ipConcentric : m_support_params.base_fill_pattern));
                        filler_support->set_bounding_box(bbox_object);
                        filler_support->spacing = support_spacing * support_density; // constant spacing to align support infill lines
                        filler_support->angle = Geometry::deg2rad(object_config.support_angle.value);

                        Polygons loops = to_polygons(poly);
                        std::unique_ptr<ExtrusionEntityCollection> base_eec = std::make_unique<ExtrusionEntityCollection>();
                        base_eec->no_sort = true;
                        ExtrusionEntitiesPtr &base_dst = base_eec->entities;
                        if (layer_id == 0) {
                            float density = float(m_object_config->raft_first_layer_density.value * 0.01);
                            fill_expolygons_with_sheath_generate_paths(base_dst, loops, filler_support.get(), density, erSupportMaterial, flow,
                                                                       m_support_params, true, false);
                        }
                        else {
                            if (need_infill && m_support_params.base_fill_pattern != ipLightning) {
                                // allow infill-only mode if support is thick enough (so min_wall_count is 0);
                                // otherwise must draw 1 wall
                                // Don't need extra walls if we have infill. Extra walls may overlap with the infills.
                                size_t min_wall_count = offset(poly, -scale_(support_spacing * 1.5)).empty() ? 1 : 0;
                                make_perimeter_and_infill(base_dst, poly, std::max(min_wall_count, wall_count), flow,
                                    erSupportMaterial, filler_support.get(), support_density, false);
                            }
                            else {
                                SupportParameters support_params = m_support_params;
                                if (area_group.need_extra_wall && object_config.tree_support_wall_count.value == 0)
                                    support_params.tree_branch_diameter_double_wall_area_scaled = 0.1;
                                tree_supports_generate_paths(base_dst, loops, flow, support_params);
                            }
                        }

                        if (m_support_params.base_fill_pattern == ipLightning) {
                            double print_z = ts_layer->print_z;
                            auto lightning_layer_mapping = printZ_to_lightninglayer.find(print_z);
                            if (lightning_layer_mapping != printZ_to_lightninglayer.end()) {
                                auto &lightning_layer = generator->getTreesForLayer(lightning_layer_mapping->second);
                                ExPolygons areas;
                                areas.emplace_back(poly);
                                areas = offset_ex(areas, -flow.scaled_spacing());
                                for (auto &area : areas) {
                                    Polylines polylines = lightning_layer.convertToLines(to_polygons(area), 0);
                                    for (auto itr = polylines.begin(); itr != polylines.end();) {
                                        if (itr->length() < scale_(1.0))
                                            itr = polylines.erase(itr);
                                        else
                                            itr++;
                                    }
                                    Polylines opt_polylines;
                                    append(opt_polylines, chain_polylines(std::move(polylines)));
                                    extrusion_entities_append_paths(base_dst, opt_polylines, erSupportMaterial,
                                        float(flow.mm3_per_mm()), float(flow.width()), float(flow.height()));
#ifdef SUPPORT_TREE_DEBUG_TO_SVG
                                    std::string name = debug_out_path("trees_polyline_%.2f.svg", ts_layer->print_z);
                                    BoundingBox bbox = get_extents(ts_layer->base_areas);
                                    SVG svg(name, bbox);
                                    if (svg.is_opened()) {
                                        svg.draw(ts_layer->base_areas, "blue");
                                        svg.draw(generator->Overhangs()[lightning_layer_mapping->second], "red");
                                        for (auto &line : opt_polylines) svg.draw(line, "yellow");
                                    }
#endif
                                }
                            }
                        }

                        if (!base_eec->empty())
                            ts_layer->support_fills.entities.push_back(base_eec.release());
                    }
                }

                // sort extrusions to reduce travel, also make sure walls go before infills
                if (ts_layer->support_fills.no_sort == false) {
                    chain_and_reorder_extrusion_entities(ts_layer->support_fills.entities);
                }
            }
        }
    );
}

void deleteDirectoryContents(const std::filesystem::path& dir)
{
    for (const auto& entry : std::filesystem::directory_iterator(dir))
        std::filesystem::remove_all(entry.path());
}


void TreeSupport::move_bounds_to_contact_nodes(std::vector<TreeSupport3D::SupportElements> &move_bounds,
                                  PrintObject                             &print_object,
                                  const TreeSupport3D::TreeSupportSettings    &config)
{
    m_ts_data = print_object.alloc_tree_support_preview_cache();
    // convert move_bounds back to Support Nodes for tree skeleton preview
    this->contact_nodes.resize(move_bounds.size());
    for (int layer_nr = move_bounds.size() - 1; layer_nr >= 0; layer_nr--) {
        TreeSupport3D::SupportElements &elements      = move_bounds[layer_nr];
        auto            &contact_nodes_layer = this->contact_nodes[layer_nr];
        for (size_t i = 0; i < elements.size(); i++) {
            auto &elem    = elements[i];
            auto &state   = elem.state;
            state.print_z = layer_z(print_object.slicing_parameters(), config, layer_nr);
            auto node = this->create_node(state.result_on_layer, state.distance_to_top, layer_nr, 0, state.to_buildplate, nullptr, state.print_z, config.layer_height);
            contact_nodes_layer.push_back(node);
            if (layer_nr < move_bounds.size() - 1 && !elem.parents.empty()) {
                node->parent = contact_nodes[layer_nr + 1][elem.parents.front()];
                for (int j : elem.parents) {
                    contact_nodes[layer_nr + 1][j]->child = node;
                    node->parents.push_back(contact_nodes[layer_nr + 1][j]);
                }
            }

            if (i == 0) { m_ts_data->layer_heights.emplace_back(state.print_z, config.layer_height, std::max(0, layer_nr - 1)); }
        }
    }
}

void TreeSupport::generate()
{
    if (!is_tree(m_object_config->support_type.value)) return;

    if (m_support_params.support_style == smsTreeOrganic) {
        generate_tree_support_3D(*m_object, this, this->throw_on_cancel);
        return;
    }

    profiler.stage_start(STAGE_total);

    // Generate overhang areas
    profiler.stage_start(STAGE_DETECT_OVERHANGS);
    m_object->print()->set_status(55, _u8L("Generating support"));
    detect_overhangs();
    profiler.stage_finish(STAGE_DETECT_OVERHANGS);

    create_tree_support_layers();
    m_ts_data = m_object->alloc_tree_support_preview_cache();
    m_ts_data->is_slim = is_slim;
    // // get the ring of outside plate
    // auto tmp= diff_ex(offset_ex(m_machine_border, scale_(100)), m_machine_border);
    // if (!tmp.empty()) m_ts_data->m_machine_border = tmp[0];

    std::vector<TreeSupport3D::SupportElements> move_bounds(m_highest_overhang_layer + 1);
    profiler.stage_start(STAGE_GENERATE_CONTACT_NODES);
    m_object->print()->set_status(56, _u8L("Support: generate contact points"));
    generate_contact_points();
    profiler.stage_finish(STAGE_GENERATE_CONTACT_NODES);

    m_ts_data->layer_heights = plan_layer_heights();

    //Drop nodes to lower layers.
    profiler.stage_start(STAGE_DROP_DOWN_NODES);
    m_object->print()->set_status(60, _u8L("Generating support"));
    drop_nodes();
    profiler.stage_finish(STAGE_DROP_DOWN_NODES);

    smooth_nodes();// , tree_support_3d_config);

    //Generate support areas.
    profiler.stage_start(STAGE_DRAW_CIRCLES);
    m_object->print()->set_status(65, _u8L("Generating support"));
    draw_circles();
    profiler.stage_finish(STAGE_DRAW_CIRCLES);



    profiler.stage_start(STAGE_GENERATE_TOOLPATHS);
    m_object->print()->set_status(70, _u8L("Generating support"));
    generate_toolpaths();
    profiler.stage_finish(STAGE_GENERATE_TOOLPATHS);

    profiler.stage_finish(STAGE_total);
    BOOST_LOG_TRIVIAL(info) << "tree support time " << profiler.report();
}

coordf_t TreeSupport::calc_branch_radius(coordf_t base_radius, size_t layers_to_top, size_t tip_layers, double diameter_angle_scale_factor)
{
    double radius;
    if (!is_slim) {
        if ((layers_to_top + 1) > tip_layers) {
            radius = base_radius + base_radius * (layers_to_top + 1) * diameter_angle_scale_factor;
        } else {
            radius = base_radius * (layers_to_top + 1) / tip_layers;
        }
    } else {
        if ((layers_to_top + 1) > tip_layers * 2) {
            radius = base_radius + base_radius * (layers_to_top + 1) * diameter_angle_scale_factor;
        } else {
            radius = base_radius * (layers_to_top + 1) / (tip_layers * 2);
        }
    }
    radius = std::clamp(radius, MIN_BRANCH_RADIUS, MAX_BRANCH_RADIUS);
    return radius;
}

coordf_t TreeSupport::calc_branch_radius(coordf_t base_radius, coordf_t mm_to_top,  double diameter_angle_scale_factor, bool use_min_distance)
{
    double radius;
    coordf_t tip_height = base_radius;// this is a 45 degree tip
    if (mm_to_top > tip_height)
    {
        radius = base_radius + (mm_to_top-tip_height) * diameter_angle_scale_factor;
    }
    else
    {
        radius = mm_to_top;// this is a 45 degree tip
    }
    radius = std::clamp(radius, MIN_BRANCH_RADIUS, MAX_BRANCH_RADIUS);
    // if have interface layers, radius should be larger
    if (m_object_config->support_interface_top_layers.value > 0)
        radius = std::max(radius, base_radius);
    return radius;
}

coordf_t TreeSupport::calc_radius(coordf_t mm_to_top)
{
    return calc_branch_radius(base_radius, mm_to_top, diameter_angle_scale_factor);
}

coordf_t TreeSupport::get_radius(const SupportNode* node)
{
    if (node->radius == 0)
        node->radius = calc_radius(node->dist_mm_to_top);
    return node->radius;
}

ExPolygons TreeSupport::get_avoidance(coordf_t radius, size_t obj_layer_nr)
{
#if USE_SUPPORT_3D
    if (m_model_volumes) {
        bool on_build_plate = m_object_config->support_on_build_plate_only.value;
        const Polygons& avoid_polys = m_model_volumes->getAvoidance(radius, obj_layer_nr, TreeSupport3D::TreeModelVolumes::AvoidanceType::FastSafe, on_build_plate, true);
        ExPolygons expolys;
        for (auto& poly : avoid_polys)
            expolys.emplace_back(std::move(poly));
        return expolys;
    }
    return ExPolygons();
#else
    return m_ts_data->get_avoidance(radius, obj_layer_nr);
#endif
}
ExPolygons TreeSupport::get_collision(coordf_t radius, size_t layer_nr)
{
#if USE_SUPPORT_3D
    if (m_model_volumes) {
        bool on_build_plate = m_object_config->support_on_build_plate_only.value;
        const Polygons& collision_polys = m_model_volumes->getCollision(radius, layer_nr, true);
        ExPolygons expolys;
        for (auto& poly : collision_polys)
            expolys.emplace_back(std::move(poly));
        return expolys;
    }
#else
    return m_ts_data->get_collision(radius, layer_nr);
#endif
    return ExPolygons();
}
Polygons TreeSupport::get_collision_polys(coordf_t radius, size_t layer_nr)
{
#if USE_SUPPORT_3D
    if (m_model_volumes) {
        bool on_build_plate = m_object_config->support_on_build_plate_only.value;
        const Polygons& collision_polys = m_model_volumes->getCollision(radius, layer_nr, true);
        return collision_polys;
    }
#else
    ExPolygons expolys = m_ts_data->get_collision(radius, layer_nr);
    Polygons polys;
    for (auto& expoly : expolys)
        for (int i = 0; i < expoly.num_contours(); i++)
            polys.emplace_back(std::move(expoly.contour_or_hole(i)));
    return polys;
#endif
    return Polygons();
}

ExPolygons avoid_object_remove_extra_small_parts(const ExPolygon &expoly, const ExPolygons& avoid_region) {
    ExPolygons expolys_out;
    if(expoly.empty()) return expolys_out;
    auto clipped_avoid_region=ClipperUtils::clip_clipper_polygons_with_subject_bbox(avoid_region, get_extents(expoly));
    auto  expolys_avoid = diff_ex(expoly, clipped_avoid_region);
    int   idx_max_area  = -1;
    float max_area      = 0;
    for (int i = 0; i < expolys_avoid.size(); ++i) {
        auto a = expolys_avoid[i].area();
        if (a > max_area) {
            max_area     = a;
            idx_max_area = i;
        }
    }
    if (idx_max_area >= 0) expolys_out.emplace_back(std::move(expolys_avoid[idx_max_area]));

    return expolys_out;
}

Polygons TreeSupport::get_trim_support_regions(
    const PrintObject& object,
    SupportLayer* support_layer_ptr,
    const coordf_t       gap_extra_above,
    const coordf_t       gap_extra_below,
    const coordf_t       gap_xy)
{
    static const double sharp_tail_xy_gap = 0.2f;
    static const double no_overlap_xy_gap = 0.2f;
    double gap_xy_scaled = scale_(gap_xy);
    SupportLayer& support_layer = *support_layer_ptr;
    auto m_print_config = object.print()->config();

    size_t idx_object_layer_overlapping = size_t(-1);

    auto is_layers_overlap = [](const SupportLayer& support_layer, const Layer& object_layer, coordf_t bridging_height = 0.f) -> bool {
        if (std::abs(support_layer.print_z - object_layer.print_z) < EPSILON)
            return true;

        coordf_t object_lh = bridging_height > EPSILON ? bridging_height : object_layer.height;
        if (support_layer.print_z < object_layer.print_z && support_layer.print_z > object_layer.print_z - object_lh)
            return true;

        if (support_layer.print_z > object_layer.print_z && support_layer.bottom_z() < object_layer.print_z - EPSILON)
            return true;

        return false;
        };

    // Find the overlapping object layers including the extra above / below gap.
    coordf_t z_threshold = support_layer.bottom_z() - gap_extra_below + EPSILON;
    idx_object_layer_overlapping = Layer::idx_higher_or_equal(
        object.layers().begin(), object.layers().end(), idx_object_layer_overlapping,
        [z_threshold](const Layer* layer) { return layer->print_z >= z_threshold; });
    // Collect all the object layers intersecting with this layer.
    Polygons polygons_trimming;
    size_t i = idx_object_layer_overlapping;
    for (; i < object.layers().size(); ++i) {
        const Layer& object_layer = *object.layers()[i];
        if (object_layer.bottom_z() > support_layer.print_z + gap_extra_above - EPSILON)
            break;

        bool is_overlap = is_layers_overlap(support_layer, object_layer);
        for (const ExPolygon& expoly : object_layer.lslices) {
            // BBS
            bool is_sharptail = !intersection_ex({ expoly }, object_layer.sharp_tails).empty();
            coordf_t trimming_offset = is_sharptail ? scale_(sharp_tail_xy_gap) :
                is_overlap ? gap_xy_scaled :
                scale_(no_overlap_xy_gap);
            polygons_append(polygons_trimming, offset({ expoly }, trimming_offset, SUPPORT_SURFACES_OFFSET_PARAMETERS));
            }
        }
    if (!m_slicing_params.soluble_interface && m_object_config->thick_bridges) {
        // Collect all bottom surfaces, which will be extruded with a bridging flow.
        for (; i < object.layers().size(); ++i) {
            const Layer& object_layer = *object.layers()[i];
            bool some_region_overlaps = false;
            for (LayerRegion* region : object_layer.regions()) {
                coordf_t bridging_height = region->region().bridging_height_avg(m_print_config);
                if (object_layer.print_z - bridging_height > support_layer.print_z + gap_extra_above - EPSILON)
                    break;
                some_region_overlaps = true;

                bool is_overlap = is_layers_overlap(support_layer, object_layer, bridging_height);
                coordf_t trimming_offset = is_overlap ? gap_xy_scaled : scale_(no_overlap_xy_gap);
                polygons_append(polygons_trimming,
                    offset(region->fill_surfaces.filter_by_type(stBottomBridge), trimming_offset, SUPPORT_SURFACES_OFFSET_PARAMETERS));
                }
            if (!some_region_overlaps)
                break;
            }
        }

    return polygons_trimming;
}

void TreeSupport::draw_circles()
{
    const PrintObjectConfig &config = m_object->config();
    const Print* print = m_object->print();
    bool has_brim = print->has_brim();
    int bottom_gap_layers = round(m_slicing_params.gap_object_support / m_slicing_params.layer_height);
    const coordf_t branch_radius = config.tree_support_branch_diameter.value / 2;
    const coordf_t branch_radius_scaled = scale_(branch_radius);
    bool on_buildplate_only = m_object_config->support_on_build_plate_only.value;
    Polygon branch_circle; //Pre-generate a circle with correct diameter so that we don't have to recompute those (co)sines every time.

    // Use square support if there are too many nodes per layer because circle support needs much longer time to compute
    // Hower circle support can be printed faster, so we prefer circle for fewer nodes case.
    const bool SQUARE_SUPPORT = avg_node_per_layer > 200;
    const int  CIRCLE_RESOLUTION = SQUARE_SUPPORT ? 4 : 100; // The number of vertices in each circle.


    for (int i = 0; i < CIRCLE_RESOLUTION; i++)
    {
        double angle;
        if (SQUARE_SUPPORT)
            angle = (double) i / CIRCLE_RESOLUTION * TAU + PI / 4.0 + nodes_angle;
        else
            angle = (double) i / CIRCLE_RESOLUTION * TAU;
        branch_circle.append(Point(cos(angle) * branch_radius_scaled, sin(angle) * branch_radius_scaled));
    }

    // Performance optimization. Only generate lslices for brim and skirt.
    size_t brim_skirt_layers = has_brim ? 1 : 0;
    const PrintConfig& print_config = print->config();
    for (const PrintObject* object : print->objects())
    {
        size_t skirt_layers = print->has_infinite_skirt() ? object->layer_count() : std::min(size_t(print_config.skirt_height.value), object->layer_count());
        brim_skirt_layers = std::max(brim_skirt_layers, skirt_layers);
    }

    // generate areas
    const coordf_t layer_height = config.layer_height.value;
    const size_t   top_interface_layers = config.support_interface_top_layers.value;
    const size_t   bottom_interface_layers = config.support_interface_bottom_layers.value < 0 ? top_interface_layers : config.support_interface_bottom_layers.value;
    const double nozzle_diameter = m_object->print()->config().nozzle_diameter.get_at(0);
    const coordf_t line_width = config.get_abs_value("support_line_width", nozzle_diameter);
    const coordf_t line_width_scaled           = scale_(line_width);
    const bool with_lightning_infill = m_support_params.base_fill_pattern == ipLightning;
    coordf_t support_extrusion_width = m_support_params.support_extrusion_width;
    const float tree_brim_width = config.tree_support_brim_width.value;

    if (m_object->support_layer_count() <= m_raft_layers)
        return;
    BOOST_LOG_TRIVIAL(info) << "draw_circles for object: " << m_object->model_object()->name;

    tbb::parallel_for(tbb::blocked_range<size_t>(0, m_ts_data->layer_heights.size()),
        [&](const tbb::blocked_range<size_t>& range)
        {
            for (size_t layer_nr = range.begin(); layer_nr < range.end(); layer_nr++)
            {
                if (print->canceled())
                    break;

                const std::vector<SupportNode*>& curr_layer_nodes = contact_nodes[layer_nr];
                SupportLayer* ts_layer = m_object->get_support_layer(layer_nr + m_raft_layers);
                assert(ts_layer != nullptr);

                // skip if current layer has no points. This fixes potential crash in get_collision (see jira BBL001-355)
                if (curr_layer_nodes.empty()) {
                    ts_layer->print_z = 0.0;
                    ts_layer->height = 0.0;
                    continue;
                }

                ts_layer->print_z = m_ts_data->layer_heights[layer_nr].print_z;
                ts_layer->height = m_ts_data->layer_heights[layer_nr].height;
                size_t obj_layer_nr = m_ts_data->layer_heights[layer_nr].obj_layer_nr;
                if (ts_layer->height < EPSILON) {
                    continue;
                }

                ExPolygons& base_areas = ts_layer->base_areas;
                ExPolygons& roof_areas = ts_layer->roof_areas;
                ExPolygons& roof_1st_layer = ts_layer->roof_1st_layer;
                ExPolygons& floor_areas = ts_layer->floor_areas;
                ExPolygons& roof_gap_areas = ts_layer->roof_gap_areas;
                coordf_t         max_layers_above_base = 0;
                coordf_t         max_layers_above_roof = 0;
                coordf_t         max_layers_above_roof1 = 0;
                int interface_id = 0;
                bool has_circle_node = false;
                bool need_extra_wall = false;
                ExPolygons collision_sharp_tails;
                ExPolygons collision_base;
                auto             get_collision = [&](bool sharp_tail) -> ExPolygons             &{
                    ExPolygons &collision = sharp_tail ? collision_sharp_tails : collision_base;
                    if (collision.empty()) {
                        collision = offset_ex(m_ts_data->m_layer_outlines[obj_layer_nr],
                                              sharp_tail ? scale_(top_z_distance) :
                                                           scale_((obj_layer_nr == 0) ? config.support_object_first_layer_gap : m_ts_data->m_xy_distance));
                        // the top layers may be too close to interface with adaptive layer heights and very small overhang angle
                        if (top_z_distance > EPSILON) {
                            float accum_height = 0;
                            for (size_t layer_id = obj_layer_nr + 1;
                                 layer_id < m_ts_data->m_layer_outlines.size() && (accum_height += m_object->get_layer(layer_id)->height) && accum_height <= top_z_distance;
                                 layer_id++) {
                                collision = union_ex(collision, offset_ex(m_ts_data->m_layer_outlines[layer_id], scale_(top_z_distance)));
                            }
                        }
                    }
                    return collision;
                };

                BOOST_LOG_TRIVIAL(debug) << "circles at layer " << layer_nr << " contact nodes size=" << curr_layer_nodes.size();
                //Draw the support areas and add the roofs appropriately to the support roof instead of normal areas.
                ts_layer->lslices.reserve(curr_layer_nodes.size());
                ExPolygons area_poly;  // the polygon node area which will be printed as normal support
                for (const SupportNode* p_node : curr_layer_nodes)
                {
                    if (print->canceled())
                        break;

                    const SupportNode& node = *p_node;
                    ExPolygons area;
                    // Generate directly from overhang polygon if one of the following is true:
                    // 1) node is a normal part of hybrid support
                    // 2) node is virtual
                    if (node.type == ePolygon || (node.distance_to_top<0 && !node.is_sharp_tail)) {
                        if (node.overhang.contour.size() > 100 || node.overhang.holes.size()>1)
                            area.emplace_back(node.overhang);
                        else {
                            area = offset_ex({ node.overhang }, scale_(m_ts_data->m_xy_distance));
                        }
                        area = diff_clipped(area, get_collision(node.is_sharp_tail && node.distance_to_top <= 0));
                        if (node.type == ePolygon) append(area_poly, area);
                    }
                    else {
                        Polygon circle(branch_circle);
                        double  scale = node.radius / branch_radius;
                        double moveX = node.movement.x() / (scale * branch_radius_scaled);
                        double moveY = node.movement.y() / (scale * branch_radius_scaled);
                        //BOOST_LOG_TRIVIAL(debug) << format("scale,moveX,moveY: %.3f,%.3f,%.3f", scale, moveX, moveY);

                        if (!SQUARE_SUPPORT && std::abs(moveX)>0.001 && std::abs(moveY)>0.001) { // draw ellipse along movement direction
                            const double vsize_inv = 0.5 / (0.01 + std::sqrt(moveX * moveX + moveY * moveY));
                            double       matrix[2*2]  = {
                                scale * (1 + moveX * moveX * vsize_inv),scale * (0 + moveX * moveY * vsize_inv),
                                scale * (0 + moveX * moveY * vsize_inv),scale * (1 + moveY * moveY * vsize_inv),
                            };
                            int i = 0;
                            for (auto vertex: branch_circle.points) {
                                vertex = Point(matrix[0] * vertex.x() + matrix[1] * vertex.y(), matrix[2] * vertex.x() + matrix[3] * vertex.y());
                                circle.points[i++] = node.position + vertex;
                            }
                        } else {
                            for (int i = 0;i< circle.points.size(); i++) {
                                circle.points[i] = circle.points[i] * scale + node.position;
                            }
                        }
                        if (obj_layer_nr == 0 && m_raft_layers == 0) {
                            double brim_width = !config.tree_support_auto_brim ? tree_brim_width : std::max(MIN_BRANCH_RADIUS_FIRST_LAYER, std::min(node.radius + node.dist_mm_to_top / (scale * branch_radius) * 0.5, MAX_BRANCH_RADIUS_FIRST_LAYER) - node.radius);
                            auto tmp=offset(circle, scale_(brim_width));
                            if(!tmp.empty())
                                circle = tmp[0];
                        }
                        area = avoid_object_remove_extra_small_parts(ExPolygon(circle), get_collision(node.is_sharp_tail && node.distance_to_top <= 0));
                        // area = diff_clipped({ ExPolygon(circle) }, get_collision(node.is_sharp_tail && node.distance_to_top <= 0));

                        if (!area.empty()) has_circle_node = true;
                        if (node.need_extra_wall) need_extra_wall = true;

                        // merge overhang to get a smoother interface surface
                        // Do not merge when buildplate_only is on, because some underneath nodes may have been deleted.
                        if (top_interface_layers > 0 && node.support_roof_layers_below > 0 && !on_buildplate_only && !node.is_sharp_tail) {
                            ExPolygons overhang_expanded;
                            if (node.overhang.contour.size() > 100 || node.overhang.holes.size()>1)
                                overhang_expanded.emplace_back(node.overhang);
                            else {
                                overhang_expanded = offset_ex({ node.overhang }, scale_(m_ts_data->m_xy_distance));
                            }
                            append(area, overhang_expanded);
                        }
                    }

                    if (obj_layer_nr>0 && node.distance_to_top < 0)
                        append(roof_gap_areas, area);
                    else if (obj_layer_nr > 0 && node.support_roof_layers_below == 1 && node.is_sharp_tail==false)
                    {
                        append(roof_1st_layer, area);
                        max_layers_above_roof1 = std::max(max_layers_above_roof1, node.dist_mm_to_top);
                    }
                    else if (obj_layer_nr > 0 && node.support_roof_layers_below > 0 && node.is_sharp_tail == false)
                    {
                        append(roof_areas, area);
                        max_layers_above_roof = std::max(max_layers_above_roof, node.dist_mm_to_top);
                        interface_id = node.obj_layer_nr % top_interface_layers;
                    }
                    else
                    {
                        append(base_areas, area);
                        max_layers_above_base = std::max(max_layers_above_base, node.dist_mm_to_top);
                    }

                }

                //m_object->print()->set_status(65, (boost::format( _u8L("Support: generate polygons at layer %d")) % layer_nr).str());

                // join roof segments
                roof_areas     = diff_clipped(offset2_ex(roof_areas, line_width_scaled, -line_width_scaled), get_collision(false));
                roof_areas     = intersection_ex(roof_areas, m_machine_border);
                roof_1st_layer = diff_clipped(offset2_ex(roof_1st_layer, line_width_scaled, -line_width_scaled), get_collision(false));

                // roof_1st_layer and roof_areas may intersect, so need to subtract roof_areas from roof_1st_layer
                roof_1st_layer = diff_ex(roof_1st_layer, ClipperUtils::clip_clipper_polygons_with_subject_bbox(roof_areas,get_extents(roof_1st_layer)));
                roof_1st_layer = intersection_ex(roof_1st_layer, m_machine_border);

                ExPolygons roofs; append(roofs, roof_1st_layer); append(roofs, roof_areas);append(roofs, roof_gap_areas);
                base_areas = diff_ex(base_areas, ClipperUtils::clip_clipper_polygons_with_subject_bbox(roofs, get_extents(base_areas)));
                base_areas = intersection_ex(base_areas, m_machine_border);

                if (SQUARE_SUPPORT) {
                    // simplify support contours
                    ExPolygons base_areas_simplified;
                    for (auto &area : base_areas) { area.simplify(scale_(line_width / 2), &base_areas_simplified); }
                    base_areas = std::move(base_areas_simplified);
                }
                //Subtract support floors. We can only compute floor_areas here instead of with roof_areas,
                // or we'll get much wider floor than necessary.
                if (bottom_interface_layers + bottom_gap_layers > 0)
                {
                    if (layer_nr >= bottom_interface_layers + bottom_gap_layers)
                    {
                        // find the lowest interface layer
                        // TODO the gap may not be exact when "independent support layer height" is enabled
                        size_t layer_nr_next = layer_nr - bottom_interface_layers;
                        size_t obj_layer_nr_next = m_ts_data->layer_heights[layer_nr_next].obj_layer_nr;
                        for (size_t i = 0; i <= bottom_gap_layers && i <= obj_layer_nr_next; i++)
                        {
                            const Layer *below_layer      = m_object->get_layer(obj_layer_nr_next - i);
                            ExPolygons bottom_interface = intersection_ex(base_areas, below_layer->lslices);
                            floor_areas.insert(floor_areas.end(), bottom_interface.begin(), bottom_interface.end());
                        }
                    }
                    if (floor_areas.empty() == false) {
                        //floor_areas = std::move(diff_ex(floor_areas, avoid_region_interface));
                        //floor_areas = std::move(offset2_ex(floor_areas, contact_dist_scaled, -contact_dist_scaled));
                        base_areas = std::move(diff_ex(base_areas, offset_ex(floor_areas, 10)));
                    }
                }
                if (bottom_gap_layers > 0 && m_ts_data->layer_heights[layer_nr].obj_layer_nr > bottom_gap_layers) {
                    const Layer* below_layer = m_object->get_layer(m_ts_data->layer_heights[layer_nr].obj_layer_nr - bottom_gap_layers);
                    ExPolygons bottom_gap_area = intersection_ex(floor_areas, below_layer->lslices);
                    if (!bottom_gap_area.empty()) {
                        floor_areas = std::move(diff_ex(floor_areas, bottom_gap_area));
                    }
                }
                auto &area_groups = ts_layer->area_groups;
                for (auto& expoly : ts_layer->base_areas) {
                    //if (area(expoly) < SQ(scale_(1))) continue;
                    area_groups.emplace_back(&expoly, SupportLayer::BaseType, max_layers_above_base);
                    area_groups.back().need_infill = overlaps({ expoly }, area_poly);
                    area_groups.back().need_extra_wall = need_extra_wall && !area_groups.back().need_infill;
                }
                for (auto& expoly : ts_layer->roof_areas) {
                    //if (area(expoly) < SQ(scale_(1))) continue;
                    area_groups.emplace_back(&expoly, SupportLayer::RoofType, max_layers_above_roof);
                    area_groups.back().interface_id = interface_id;
                }
                for (auto &expoly : ts_layer->floor_areas) {
                    //if (area(expoly) < SQ(scale_(1))) continue;
                    area_groups.emplace_back(&expoly, SupportLayer::FloorType, 10000);
                }
                for (auto &expoly : ts_layer->roof_1st_layer) {
                    //if (area(expoly) < SQ(scale_(1))) continue;
                    area_groups.emplace_back(&expoly, SupportLayer::Roof1stLayer, max_layers_above_roof1);
                }

                for (auto &area_group : area_groups) {
                    auto& expoly = area_group.area;
                    expoly->holes.erase(std::remove_if(expoly->holes.begin(), expoly->holes.end(),
                                                       [](auto &hole) {
                                                           auto bbox_size = get_extents(hole).size();
                                                           return bbox_size[0] < scale_(2) && bbox_size[1] < scale_(2);
                                                       }),
                                        expoly->holes.end());

                    if (layer_nr < brim_skirt_layers)
                        ts_layer->lslices.emplace_back(*expoly);
                }

                ts_layer->lslices = std::move(union_ex(ts_layer->lslices));
                //Must update bounding box which is used in avoid crossing perimeter
                ts_layer->lslices_bboxes.clear();
                ts_layer->lslices_bboxes.reserve(ts_layer->lslices.size());
                for (const ExPolygon& expoly : ts_layer->lslices)
                    ts_layer->lslices_bboxes.emplace_back(get_extents(expoly));
                ts_layer->backup_untyped_slices();

            }
        });


        if (with_lightning_infill)
        {
            const bool global_lightning_infill = true;

            std::vector<Polygons> contours;
            std::vector<Polygons> overhangs;
            for (int layer_nr = 1; layer_nr < contact_nodes.size(); layer_nr++) {
                if (print->canceled()) break;
                const std::vector<SupportNode*>& curr_layer_nodes = contact_nodes[layer_nr];
                SupportLayer* ts_layer = m_object->get_support_layer(layer_nr + m_raft_layers);
                assert(ts_layer != nullptr);

                // skip if current layer has no points. This fixes potential crash in get_collision (see jira BBL001-355)
                if (curr_layer_nodes.empty()) continue;
                if (ts_layer->height < EPSILON) continue;
                if (ts_layer->area_groups.empty()) continue;

                ExPolygons& base_areas = ts_layer->base_areas;

                int layer_nr_lower = layer_nr - 1;
                for (; layer_nr_lower >= 0; layer_nr_lower--) {
                    if (!m_object->get_support_layer(layer_nr_lower + m_raft_layers)->area_groups.empty()) break;
                }
                if (layer_nr_lower <= 0) continue;

                SupportLayer* lower_layer = m_object->get_support_layer(layer_nr_lower + m_raft_layers);
                ExPolygons& base_areas_lower = lower_layer->base_areas;

                ExPolygons overhang;
                if (global_lightning_infill)
                {
                    //search overhangs globally
                    overhang = std::move(diff_ex(offset_ex(base_areas_lower, -2.0 * scale_(support_extrusion_width)), base_areas));
                }
                else
                {
                    //search overhangs only on floating islands
                    for (auto& base_area : base_areas)
                        for (auto& hole : base_area.holes)
                        {
                            Polygon rev_hole = hole;
                            rev_hole.make_counter_clockwise();
                            ExPolygons ex_hole;
                            ex_hole.emplace_back(std::move(ExPolygon(rev_hole)));
                            for (auto& other_area : base_areas)
                                //if (&other_area != &base_area)
                                    ex_hole = std::move(diff_ex(ex_hole, other_area));
                            overhang = std::move(union_ex(overhang, ex_hole));
                        }
                    overhang = std::move(intersection_ex(overhang, offset_ex(base_areas_lower, -0.5 * scale_(support_extrusion_width))));
                }

                overhangs.emplace_back(to_polygons(overhang));
                contours.emplace_back(to_polygons(base_areas_lower));
                printZ_to_lightninglayer[lower_layer->print_z] = overhangs.size() - 1;
            }


            auto m_support_material_flow = support_material_flow(m_object, m_slicing_params.layer_height);
            coordf_t support_spacing = m_object_config->support_base_pattern_spacing.value + m_support_material_flow.spacing();
            coordf_t support_density = std::min(1., m_support_material_flow.spacing() / support_spacing * 2); // for lightning infill the density is defined differently, so need to double it
            generator = std::make_unique<FillLightning::Generator>(m_object, contours, overhangs, []() {}, support_density);
        }

        else if (!with_infill) {
            // move the holes to contour so they can be well supported

            // check if poly's contour intersects with expoly's contour
            auto intersects_contour = [](Polygon poly, ExPolygon expoly, Point& pt_on_poly, Point& pt_on_expoly, Point& pt_far_on_poly, float dist_thresh = 0.01) {
                Polylines pl_out = intersection_pl(to_polylines(expoly), ExPolygon(poly));
                if (pl_out.empty()) return false;
                float min_dist = std::numeric_limits<float>::max();
                float max_dist = 0;
                for (auto from : poly.points) {
                    for (int i = 0; i < expoly.num_contours(); i++) {
                        const Point* candidate = expoly.contour_or_hole(i).closest_point(from);
                        double       dist2 = vsize2_with_unscale(*candidate - from);
                        if (dist2 < min_dist) {
                            min_dist = dist2;
                            pt_on_poly = from;
                            pt_on_expoly = *candidate;
                            }
                        if (dist2 > max_dist) {
                            max_dist = dist2;
                            pt_far_on_poly = from;
                            }
                        if (dist2 < dist_thresh) { return true; }
                        }
                    }
                return false;
                };

            // polygon pointer: depth, direction, farPoint
            std::map<const Polygon*, std::tuple<int, Point, Point>> holePropagationInfos;
            for (int layer_nr = contact_nodes.size() - 1; layer_nr > 0; layer_nr--) {
                if (print->canceled()) break;
                //m_object->print()->set_status(66, (boost::format(_u8L("Support: fix holes at layer %d")) % layer_nr).str());

                const std::vector<SupportNode*>& curr_layer_nodes = contact_nodes[layer_nr];
                SupportLayer* ts_layer = m_object->get_support_layer(layer_nr + m_raft_layers);
                assert(ts_layer != nullptr);

                // skip if current layer has no points. This fixes potential crash in get_collision (see jira BBL001-355)
                if (curr_layer_nodes.empty()) continue;
                if (ts_layer->height < EPSILON) continue;
                if (ts_layer->area_groups.empty()) continue;

                int layer_nr_lower = layer_nr - 1;
                for (; layer_nr_lower >= 0; layer_nr_lower--) {
                    if (!m_object->get_support_layer(layer_nr_lower + m_raft_layers)->area_groups.empty()) break;
                }
                if (layer_nr_lower < 0) continue;
                auto& area_groups_lower = m_object->get_support_layer(layer_nr_lower + m_raft_layers)->area_groups;

                for (const auto& area_group : ts_layer->area_groups) {
                    if (area_group.type != SupportLayer::BaseType) continue;
                    const auto& area = area_group.area;
                    for (const auto& hole : area->holes) {
                        // auto hole_bbox = get_extents(hole).polygon();
                        for (auto& area_group_lower : area_groups_lower) {
                            if (area_group.type != SupportLayer::BaseType) continue;
                            auto& base_area_lower = *area_group_lower.area;
                            Point pt_on_poly, pt_on_expoly, pt_far_on_poly;
                            // if a hole doesn't intersect with lower layer's contours, add a hole to lower layer and move it slightly to the contour
                            if (base_area_lower.contour.contains(hole.points.front()) && !intersects_contour(hole, base_area_lower, pt_on_poly, pt_on_expoly, pt_far_on_poly)) {
                                Polygon hole_lower = hole;
                                Point   direction = normal(pt_on_expoly - pt_on_poly, line_width_scaled / 2);
                                hole_lower.translate(direction);
                                // note to expand a hole, we need to do negative offset
                                auto hole_expanded = offset(hole_lower, -line_width_scaled / 4, ClipperLib::JoinType::jtSquare);
                                if (!hole_expanded.empty()) {
                                    base_area_lower.holes.push_back(std::move(hole_expanded[0]));
                                    holePropagationInfos.insert({ &base_area_lower.holes.back(), {25, direction, pt_far_on_poly} });
                                    }
                                break;
                                }
                            else if (holePropagationInfos.find(&hole) != holePropagationInfos.end() && std::get<0>(holePropagationInfos[&hole]) > 0 &&
                                base_area_lower.contour.contains(std::get<2>(holePropagationInfos[&hole]))) {
                                Polygon hole_lower = hole;
                                auto&& direction = std::get<1>(holePropagationInfos[&hole]);
                                hole_lower.translate(direction);
                                // note to shrink a hole, we need to do positive offset
                                auto  hole_expanded = offset(hole_lower, line_width_scaled / 2, ClipperLib::JoinType::jtSquare);
                                Point farPoint = std::get<2>(holePropagationInfos[&hole]) + direction * 2;
                                if (!hole_expanded.empty()) {
                                    base_area_lower.holes.push_back(std::move(hole_expanded[0]));
                                    holePropagationInfos.insert({ &base_area_lower.holes.back(), {std::get<0>(holePropagationInfos[&hole]) - 1, direction, farPoint} });
                                    }
                                break;
                                }
                            }
                        {
                        // if roof1 interface is inside a hole, need to expand the interface
                        for (auto& roof1 : ts_layer->roof_1st_layer) {
                            //if (hole.contains(roof1.contour.points.front()) && hole.contains(roof1.contour.bounding_box().center()))
                            bool is_inside_hole = std::all_of(roof1.contour.points.begin(), roof1.contour.points.end(), [&hole](Point& pt) { return hole.contains(pt); });
                            if (is_inside_hole) {
                                Polygon hole_reoriented = hole;
                                if (roof1.contour.is_counter_clockwise())
                                    hole_reoriented.make_counter_clockwise();
                                else if (roof1.contour.is_clockwise())
                                    hole_reoriented.make_clockwise();
                                auto tmp = union_({ roof1.contour }, { hole_reoriented });
                                if (!tmp.empty()) roof1.contour = tmp[0];

                                // make sure 1) roof1 and object 2) roof1 and roof, won't intersect
                                // Note: We can't replace roof1 directly, as we have recorded its address.
                                //       So instead we need to replace its members one by one.
                                auto tmp1 = diff_ex(roof1, get_collision(0, m_ts_data->layer_heights[layer_nr].obj_layer_nr));
                                tmp1 = diff_ex(tmp1, ts_layer->roof_areas);
                                if (!tmp1.empty()) {
                                    roof1.contour = std::move(tmp1[0].contour);
                                    roof1.holes = std::move(tmp1[0].holes);
                                    }
                                break;
                                }
                            }
                        }
                        }
                    }
                }
            }


#ifdef SUPPORT_TREE_DEBUG_TO_SVG
    for (int layer_nr = m_object->support_layer_count() - 1; layer_nr >= 0; layer_nr--) {
        SupportLayer* ts_layer = m_object->get_support_layer(layer_nr + m_raft_layers);
        ExPolygons& base_areas = ts_layer->base_areas;
        ExPolygons& roof_areas = ts_layer->roof_areas;
        ExPolygons& roof_1st_layer = ts_layer->roof_1st_layer;
        ExPolygons roofs = roof_areas; append(roofs, roof_1st_layer);
        if (base_areas.empty() && roof_areas.empty() && roof_1st_layer.empty()) continue;
        draw_contours_and_nodes_to_svg(debug_out_path("circles_%d_%.2f.svg", layer_nr, ts_layer->print_z), base_areas, roofs, ts_layer->lslices_extrudable, {}, {}, {"base", "roof", "lslices"}, {"blue","red","black"});
    }
#endif  // SUPPORT_TREE_DEBUG_TO_SVG

    SupportLayerPtrs& ts_layers = m_object->support_layers();
    auto iter = std::remove_if(ts_layers.begin(), ts_layers.end(), [](SupportLayer* ts_layer) { return ts_layer->height < EPSILON; });
    ts_layers.erase(iter, ts_layers.end());
    for (int layer_nr = 0; layer_nr < ts_layers.size(); layer_nr++) {
        ts_layers[layer_nr]->upper_layer = layer_nr != ts_layers.size() - 1 ? ts_layers[layer_nr + 1] : nullptr;
        ts_layers[layer_nr]->lower_layer = layer_nr > 0 ? ts_layers[layer_nr - 1] : nullptr;
    }
}

double SupportNode::diameter_angle_scale_factor;

void TreeSupport::drop_nodes()
{
    const PrintObjectConfig &config = m_object->config();
    // Use Minimum Spanning Tree to connect the points on each layer and move them while dropping them down.
    const coordf_t support_extrusion_width = m_support_params.support_extrusion_width;
    const coordf_t layer_height = config.layer_height.value;
    const double angle = config.tree_support_branch_angle.value * M_PI / 180.;
    const int wall_count = std::max(1, config.tree_support_wall_count.value);
    double tan_angle = tan(angle); // when nodes are thick, they can move further. this is the max angle
    const coordf_t max_move_distance = (angle < M_PI / 2) ? (coordf_t)(tan_angle * layer_height)*wall_count : std::numeric_limits<coordf_t>::max();
    const double max_move_distance2 = max_move_distance * max_move_distance;
    const size_t tip_layers = base_radius / layer_height; //The number of layers to be shrinking the circle to create a tip. This produces a 45 degree angle.
    const coordf_t radius_sample_resolution = m_ts_data->m_radius_sample_resolution;
    const bool support_on_buildplate_only = config.support_on_build_plate_only.value;
    const size_t top_interface_layers = config.support_interface_top_layers.value;
    const size_t bottom_interface_layers = config.support_interface_bottom_layers.value < 0 ? top_interface_layers : config.support_interface_bottom_layers.value;
    SupportNode::diameter_angle_scale_factor = diameter_angle_scale_factor;
    float        DO_NOT_MOVER_UNDER_MM       = is_slim ? 0 : 5;                     // do not move contact points under 5mm

    auto get_max_move_dist = [this, &config, tan_angle, wall_count, support_extrusion_width](const SupportNode *node, int power = 1) {
        if (node->max_move_dist == 0) {
            node->radius        = get_radius(node);
            node->max_move_dist = std::min(tan_angle * node->height, support_extrusion_width);
        }
        double move_dist = node->max_move_dist;
        if (power == 2) move_dist = SQ(move_dist);
        return move_dist;
    };

    std::vector<LayerHeightData> &layer_heights = m_ts_data->layer_heights;
    if (layer_heights.empty()) return;

    // precalculate avoidance of all possible radii.
    // This will cause computing more (radius, layer_nr) pairs, but it's worth to do so since we are doning this in parallel.
    if (1) {
        typedef std::chrono::high_resolution_clock clock_;
        typedef std::chrono::duration<double, std::ratio<1> > second_;
        std::chrono::time_point<clock_> t0{ clock_::now() };

        // get all the possible radiis
        std::vector<std::set<coordf_t> > all_layer_radius(contact_nodes.size());
        std::vector<std::set<coordf_t>> all_layer_node_dist(contact_nodes.size());
        for (size_t layer_nr = contact_nodes.size() - 1; layer_nr > 0; layer_nr--) {
            auto& layer_radius = all_layer_radius[layer_nr];
            auto& layer_node_dist = all_layer_node_dist[layer_nr];
            for (auto* p_node : contact_nodes[layer_nr]) {
                layer_node_dist.emplace(p_node->dist_mm_to_top);
            }
            size_t layer_nr_next = layer_nr - 1;
            if (layer_nr_next <= contact_nodes.size() - 1 && layer_nr_next > 0) {
                for (auto node_dist : layer_node_dist)
                    all_layer_node_dist[layer_nr_next].emplace(node_dist + layer_heights[layer_nr].height);
            }
            for (auto node_dist : layer_node_dist) {
                layer_radius.emplace(calc_radius(node_dist));
            }
        }
        // parallel pre-compute avoidance
        tbb::parallel_for(tbb::blocked_range<size_t>(0, contact_nodes.size() - 1), [&](const tbb::blocked_range<size_t> &range) {
            for (size_t layer_nr = range.begin(); layer_nr < range.end(); layer_nr++) {
            for (auto node_radius : all_layer_radius[layer_nr]) {
                size_t obj_layer_nr= layer_heights[layer_nr].obj_layer_nr;
                m_ts_data->get_avoidance(node_radius, obj_layer_nr);
                get_collision(0, obj_layer_nr);
                get_collision(node_radius, obj_layer_nr);
            }
        }
        });

        double duration{ std::chrono::duration_cast<second_>(clock_::now() - t0).count() };
        BOOST_LOG_TRIVIAL(debug) << "finish pre calculate_avoidance. before m_avoidance_cache.size()=" << m_ts_data->m_avoidance_cache.size()
            << ", takes " << duration << " secs.";
    }

    m_spanning_trees.resize(contact_nodes.size());
    //m_mst_line_x_layer_contour_caches.resize(contact_nodes.size());

    for (size_t layer_nr = contact_nodes.size() - 1; layer_nr > 0; layer_nr--) // Skip layer 0, since we can't drop down the vertices there.
    {
        if (m_object->print()->canceled())
            break;

        auto& layer_contact_nodes = contact_nodes[layer_nr];
        if (layer_contact_nodes.empty())
            continue;

        int layer_nr_next = layer_nr - 1;
        coordf_t print_z = layer_heights[layer_nr].print_z;
        coordf_t print_z_next = layer_heights[layer_nr_next].print_z;
        coordf_t height_next   = layer_heights[layer_nr_next].height;
        size_t   obj_layer_nr  = layer_heights[layer_nr].obj_layer_nr;
        size_t   obj_layer_nr_next = layer_heights[layer_nr_next].obj_layer_nr;

        std::deque<std::pair<size_t, SupportNode*>> unsupported_branch_leaves; // All nodes that are leaves on this layer that would result in unsupported ('mid-air') branches.

        m_object->print()->set_status(60 + int(10 * (1 - float(layer_nr) / contact_nodes.size())), _u8L("Generating support"));// (boost::format(_u8L("Support: propagate branches at layer %d")) % layer_nr).str());

        Polygons layer_contours = std::move(m_ts_data->get_contours_with_holes(obj_layer_nr));
        //std::unordered_map<Line, bool, LineHash>& mst_line_x_layer_contour_cache = m_mst_line_x_layer_contour_caches[layer_nr];
        tbb::concurrent_unordered_map<Line, bool, LineHash> mst_line_x_layer_contour_cache;
        auto is_line_cut_by_contour = [&mst_line_x_layer_contour_cache,&layer_contours](Point a, Point b)
        {
            auto iter = mst_line_x_layer_contour_cache.find({ a, b });
            if (iter != mst_line_x_layer_contour_cache.end()) {
                if (iter->second)
                    return true;
            }
            else {
                profiler.tic();
                Line ln(b, a);
                Lines pls_intersect = intersection_ln(ln, layer_contours);
                mst_line_x_layer_contour_cache.insert({ {a, b}, !pls_intersect.empty() });
                mst_line_x_layer_contour_cache.insert({ ln, !pls_intersect.empty() });
                profiler.stage_add(STAGE_intersection_ln);
                if (!pls_intersect.empty())
                    return true;
            }
            return false;
        };

        //Group together all nodes for each part.
        const ExPolygons& parts = m_ts_data->m_layer_outlines_below[obj_layer_nr];
        std::vector<std::unordered_map<Point, SupportNode*, PointHash>> nodes_per_part(1 + parts.size()); //All nodes that aren't inside a part get grouped together in the 0th part.
        for (SupportNode* p_node : layer_contact_nodes)
        {
            const SupportNode& node = *p_node;

            if (support_on_buildplate_only && !node.to_buildplate) //Can't rest on model and unable to reach the build plate. Then we must drop the node and leave parts unsupported.
            {
                unsupported_branch_leaves.push_front({ layer_nr, p_node });
                continue;
            }
            if (node.to_buildplate || parts.empty()) //It's outside, so make it go towards the build plate.
            {
                nodes_per_part[0][node.position] = p_node;
                continue;
            }

            /* Find which part this node is located in and group the nodes in
             * the same part together. Since nodes have a radius and the
             * avoidance areas are offset by that radius, the set of parts may
             * be different per node. Here we consider a node to be inside the
             * part that is closest. The node may be inside a bigger part that
             * is actually two parts merged together due to an offset. In that
             * case we may incorrectly keep two nodes separate, but at least
             * every node falls into some group.
             */
            coordf_t closest_part_distance2 = std::numeric_limits<coordf_t>::max();
            size_t closest_part = -1;
            for (size_t part_index = 0; part_index < parts.size(); part_index++)
            {
                //constexpr bool border_result = true;
                if (is_inside_ex(parts[part_index], node.position)) //If it's inside, the distance is 0 and this part is considered the best.
                {
                    closest_part = part_index;
                    closest_part_distance2 = 0;
                    break;
                }

                Point closest_point = *parts[part_index].contour.closest_point(node.position);
                const coordf_t distance2 = vsize2_with_unscale(node.position - closest_point);
                if (distance2 < closest_part_distance2)
                {
                    closest_part_distance2 = distance2;
                    closest_part = part_index;
                }
            }
            //Put it in the best one.
            nodes_per_part[closest_part + 1][node.position] = p_node; //Index + 1 because the 0th index is the outside part.
        }

        //Create a MST for every part.
        profiler.tic();
        //std::vector<MinimumSpanningTree>& spanning_trees = m_spanning_trees[layer_nr];
        std::vector<MinimumSpanningTree> spanning_trees;
        for (const std::unordered_map<Point, SupportNode*, PointHash>& group : nodes_per_part)
        {
            std::vector<Point> points_to_buildplate;
            for (const std::pair<const Point, SupportNode*>& entry : group)
            {
                points_to_buildplate.emplace_back(entry.first); //Just the position of the node.
            }
            spanning_trees.emplace_back(points_to_buildplate);
        }
        profiler.stage_add(STAGE_MinimumSpanningTree);

#ifdef SUPPORT_TREE_DEBUG_TO_SVG
        coordf_t branch_radius_temp = 0;
        coordf_t max_y = std::numeric_limits<coordf_t>::min();
        draw_layer_mst(debug_out_path("mtree_%.2f.svg", print_z), spanning_trees, m_object->get_layer(obj_layer_nr)->lslices_extrudable);
#endif
        for (size_t group_index = 0; group_index < nodes_per_part.size(); group_index++)
        {
            auto& nodes_this_part = nodes_per_part[group_index];
            const MinimumSpanningTree& mst = spanning_trees[group_index];
            //In the first pass, merge all nodes that are close together.
            std::vector<std::pair<const Point, SupportNode*>> nodes_vec(nodes_this_part.begin(), nodes_this_part.end());
            tbb::parallel_for_each(nodes_vec.begin(), nodes_vec.end(), [&](const std::pair<const Point, SupportNode*>& entry) {
                SupportNode* p_node = entry.second;
                SupportNode& node = *p_node;
                if (!p_node->valid)
                {
                    return; //Delete this node (don't create a new node for it on the next layer).
                }
                const std::vector<Point>& neighbours = mst.adjacent_nodes(node.position);
                if (node.type == ePolygon) {
                    // Remove all circle neighbours that are completely inside the polygon and merge them into this node.
                    for (const Point &neighbour : neighbours) {
                        SupportNode *    neighbour_node          = nodes_this_part[neighbour];
                        if (neighbour_node->valid == false) continue;
                        if (neighbour_node->type == ePolygon) continue;
                        coord_t    neighbour_radius = scale_(neighbour_node->radius);
                        Point     pt_north = neighbour + Point(0, neighbour_radius), pt_south = neighbour - Point(0, neighbour_radius),
                              pt_west = neighbour - Point(neighbour_radius, 0), pt_east = neighbour + Point(neighbour_radius, 0);
                        if (is_inside_ex(node.overhang, neighbour) && is_inside_ex(node.overhang, pt_north) && is_inside_ex(node.overhang, pt_south)
                            && is_inside_ex(node.overhang, pt_west) && is_inside_ex(node.overhang, pt_east)){
                            node.distance_to_top           = std::max(node.distance_to_top, neighbour_node->distance_to_top);
                            node.support_roof_layers_below = std::max(node.support_roof_layers_below, neighbour_node->support_roof_layers_below);
                            node.dist_mm_to_top            = std::max(node.dist_mm_to_top, neighbour_node->dist_mm_to_top);
                            node.merged_neighbours.push_front(neighbour_node);
                            node.merged_neighbours.insert(node.merged_neighbours.end(), neighbour_node->merged_neighbours.begin(), neighbour_node->merged_neighbours.end());
                            neighbour_node->valid = false;
                        }
                    }
                } else if (neighbours.size() == 1 && vsize2_with_unscale(neighbours[0] - node.position) < get_max_move_dist(p_node, 2) &&
                           mst.adjacent_nodes(neighbours[0]).size() == 1 &&
                           nodes_this_part[neighbours[0]]->type!=ePolygon) // We have just two nodes left, and they're very close, and the only neighbor is not ePolygon
                {
                    //Insert a completely new node and let both original nodes fade.
                    Point next_position = (node.position + neighbours[0]) / 2; //Average position of the two nodes.
                    coordf_t next_radius = calc_radius(node.dist_mm_to_top+height_next);
                    auto avoid_layer = get_avoidance(next_radius, obj_layer_nr_next);
                    if (group_index == 0)
                    {
                        //Avoid collisions.
                        const coordf_t max_move_between_samples = max_move_distance + radius_sample_resolution + EPSILON; //100 micron extra for rounding errors.
                        move_out_expolys(avoid_layer, next_position, radius_sample_resolution + EPSILON, max_move_between_samples);
                    }

                    SupportNode* neighbour = nodes_this_part[neighbours[0]];
                    SupportNode* node_parent;
                    if (p_node->parent && neighbour->parent)
                        node_parent = (node.dist_mm_to_top >= neighbour->dist_mm_to_top) ? p_node : neighbour;
                    else
                        node_parent = p_node->parent ? p_node : neighbour;
                    // Make sure the next pass doesn't drop down either of these (since that already happened).
                    node_parent->merged_neighbours.push_front(node_parent == p_node ? neighbour : p_node);
                    const bool to_buildplate = !is_inside_ex(get_collision(0, obj_layer_nr_next), next_position);
                    SupportNode* next_node = m_ts_data->create_node(next_position, node_parent->distance_to_top + 1, obj_layer_nr_next, node_parent->support_roof_layers_below - 1, to_buildplate, node_parent,
                        print_z_next, height_next);
                    get_max_move_dist(next_node);
                    m_ts_data->m_mutex.lock();
                    contact_nodes[layer_nr_next].push_back(next_node);
                    neighbour->valid = false;
                    p_node->valid = false;
                    m_ts_data->m_mutex.unlock();
                }
                else if (neighbours.size() > 1) //Don't merge leaf nodes because we would then incur movement greater than the maximum move distance.
                {
                    //Remove all neighbours that are too close and merge them into this node.
                    for (const Point& neighbour : neighbours)
                    {
                        if (vsize2_with_unscale(neighbour - node.position) < get_max_move_dist(&node,2))
                        {
                            SupportNode* neighbour_node = nodes_this_part[neighbour];
                            if (neighbour_node->type == ePolygon) continue;
                            // only allow bigger node to merge smaller nodes. See STUDIO-6326
                            if(node.dist_mm_to_top < neighbour_node->dist_mm_to_top) continue;

                            m_ts_data->m_mutex.lock();
                            if (p_node->valid)
                            {  // since we are processing all nodes in parallel, p_node may have been deleted by another thread. In this case, we should not delete neighbour_node.
                                node.merged_neighbours.push_front(neighbour_node);
                                node.merged_neighbours.insert(node.merged_neighbours.end(), neighbour_node->merged_neighbours.begin(), neighbour_node->merged_neighbours.end());
                                neighbour_node->valid = false;
                            }
                            m_ts_data->m_mutex.unlock();
                        }
                    }
                }
            }
            );

            //In the second pass, move all middle nodes.
            tbb::parallel_for_each(nodes_vec.begin(), nodes_vec.end(), [&](const std::pair<const Point, SupportNode*>& entry) {

                SupportNode* p_node = entry.second;
                const SupportNode& node = *p_node;
                if (!p_node->valid)
                {
                    return;
                }
                if (node.type == ePolygon) {
                    // polygon node do not merge or move
                    const bool to_buildplate = true;
                    // keep only the part that won't be removed by the next layer
                    ExPolygons overhangs_next = diff_clipped({ node.overhang }, get_collision(0, obj_layer_nr_next));
                    for(auto& overhang:overhangs_next) {
                        Point        next_pt     = overhang.contour.centroid();
                        SupportNode *next_node   = m_ts_data->create_node(next_pt, p_node->distance_to_top + 1, obj_layer_nr_next, p_node->support_roof_layers_below - 1,
                                                                          to_buildplate, p_node, print_z_next, height_next);
                        next_node->max_move_dist = 0;
                        next_node->overhang = std::move(overhang);
                        m_ts_data->m_mutex.lock();
                        contact_nodes[layer_nr_next].emplace_back(next_node);
                        m_ts_data->m_mutex.unlock();

                    }
                    return;
                }

                //If the branch falls completely inside a collision area (the entire branch would be removed by the X/Y offset), delete it.
                if (group_index > 0 && is_inside_ex(get_collision(0, obj_layer_nr), node.position))
                {
                    std::scoped_lock lock(m_ts_data->m_mutex);
                    const coordf_t branch_radius_node = get_radius(p_node);
                    Point to_outside = projection_onto(get_collision(0, obj_layer_nr), node.position);
                    double dist2_to_outside = vsize2_with_unscale(node.position - to_outside);
                    if (dist2_to_outside >= branch_radius_node * branch_radius_node) //Too far inside.
                    {
                        if (support_on_buildplate_only)
                        {
                            unsupported_branch_leaves.push_front({ layer_nr, p_node });
                        }
                        else {
                            p_node->valid = false;
                        }
                        return;
                    }
                    // if the link between parent and current is cut by contours, mark current as bottom contact node
                    if (p_node->parent && intersection_ln({p_node->position, p_node->parent->position}, layer_contours).empty()==false)
                    {
                        p_node->valid = false;
                        return;
                    }
                }
                Point next_layer_vertex = node.position;
                Point move_to_neighbor_center;
                std::vector<Point>       moves;
                std::vector<float>       weights;
                const std::vector<Point>& neighbours = mst.adjacent_nodes(node.position);
                // 1. do not merge neighbors under 5mm
                // 2. Only merge node with single neighbor in distance between [max_move_distance, 10mm/layer_height]
                float dist2_to_first_neighbor = neighbours.empty() ? 0 : vsize2_with_unscale(neighbours[0] - node.position);
                if (node.print_z > DO_NOT_MOVER_UNDER_MM &&
                    (neighbours.size() > 1 || (neighbours.size() == 1 && dist2_to_first_neighbor >= get_max_move_dist(p_node, 2)))) // Only nodes that aren't about to collapse.
                {
                    // Move towards the average position of all neighbours.
                    Point sum_direction(0, 0);
                    for (const Point &neighbour : neighbours) {
                        // do not move to the neighbor to be deleted
                        SupportNode *neighbour_node = nodes_this_part[neighbour];
                        if (!neighbour_node->valid) continue;

                        Point direction = neighbour - node.position;
                        // do not move to neighbor that's too far away (即使以最大速度移动，在接触热床之前都无法汇聚)
                        float dist2_to_neighbor = vsize2_with_unscale(direction);

                        coordf_t branch_bottom_radius = calc_radius(node.dist_mm_to_top + node.print_z);
                        coordf_t neighbour_bottom_radius = calc_radius(neighbour_node->dist_mm_to_top + neighbour_node->print_z);
                        double max_converge_distance = tan_angle * (p_node->print_z - DO_NOT_MOVER_UNDER_MM) + std::max(branch_bottom_radius, neighbour_bottom_radius);
                        if (dist2_to_neighbor > max_converge_distance * max_converge_distance) continue;

                        if (is_line_cut_by_contour(node.position, neighbour)) continue;

                        if (!is_strong)
                            sum_direction += direction * (1 / dist2_to_neighbor);
                        else
                            sum_direction += direction;
                    }

                    if (!is_strong)
                        move_to_neighbor_center = sum_direction;
                    else {
                        if (vsize2_with_unscale(sum_direction) <= get_max_move_dist(p_node, 2)) {
                            move_to_neighbor_center = sum_direction;
                        } else {
                            move_to_neighbor_center = normal(sum_direction, scale_(get_max_move_dist(p_node)));
                        }
                    }
                }

#ifdef SUPPORT_TREE_DEBUG_TO_SVG
                if (node.position(1) > max_y) {
                    max_y              = node.position(1);
                    branch_radius_temp = get_radius(p_node);
                }
#endif
                coordf_t next_radius = calc_radius(node.dist_mm_to_top + height_next);
                auto avoidance_next = get_avoidance(next_radius, obj_layer_nr_next);

                Point  to_outside         = projection_onto(avoidance_next, node.position);
                Point  direction_to_outer = to_outside - node.position;
                double dist2_to_outer     = vsize2_with_unscale(direction_to_outer);
                // don't move if
                // 1) line of node and to_outside is cut by contour (means supports may intersect with object)
                // 2) it's impossible to move to build plate
                if (is_line_cut_by_contour(node.position, to_outside) || dist2_to_outer > max_move_distance2 * SQ(obj_layer_nr) ||
                    !is_inside_ex(avoidance_next, node.position)) {
                    // try move to outside of lower layer instead
                    Point candidate_vertex = node.position;
                    const coordf_t max_move_between_samples = max_move_distance + radius_sample_resolution + EPSILON; // 100 micron extra for rounding errors.
                    // use get_collision instead of get_avoidance here (See STUDIO-4252)
                    bool           is_outside               = move_out_expolys(get_collision(next_radius,obj_layer_nr_next), candidate_vertex, max_move_between_samples, max_move_between_samples);
                    if (is_outside) {
                        direction_to_outer = candidate_vertex - node.position;
                        dist2_to_outer    = vsize2_with_unscale(direction_to_outer);
                    } else {
                        direction_to_outer = Point(0, 0);
                        dist2_to_outer     = 0;
                    }
                }
                // move to the averaged direction of neighbor center and contour edge if they are roughly same direction
                Point movement;
                if (!is_strong)
                    movement = move_to_neighbor_center*2 + (dist2_to_outer > EPSILON ? direction_to_outer * (1 / dist2_to_outer) : Point(0, 0));
                else {
                    if (movement.dot(move_to_neighbor_center) >= 0.2 || move_to_neighbor_center == Point(0, 0))
                        movement = direction_to_outer + move_to_neighbor_center;
                    else
                        movement = move_to_neighbor_center; // otherwise move to neighbor center first
                }

                if (node.is_sharp_tail && node.dist_mm_to_top < 3) {
                    movement = normal(node.skin_direction, scale_(get_max_move_dist(&node)));
                }
                else if (dist2_to_outer > 0)
                    movement = normal(direction_to_outer, scale_(get_max_move_dist(&node)));
                else
                    movement = normal(move_to_neighbor_center, scale_(get_max_move_dist(&node)));

                next_layer_vertex += movement;

                if (group_index == 0 && 0) {
                    // Avoid collisions.
                    const coordf_t max_move_between_samples = get_max_move_dist(&node, 1) + radius_sample_resolution + EPSILON; // 100 micron extra for rounding errors.
                    bool           is_outside               = move_out_expolys(avoidance_next, next_layer_vertex, radius_sample_resolution + EPSILON, max_move_between_samples);
                    if (!is_outside) {
                        Point candidate_vertex = node.position;
                        is_outside             = move_out_expolys(avoidance_next, candidate_vertex, radius_sample_resolution + EPSILON, max_move_between_samples);
                        if (is_outside) { next_layer_vertex = candidate_vertex; }
                    }
                }
                auto              next_collision = get_collision(0, obj_layer_nr_next);
                const bool   to_buildplate  = !is_inside_ex(m_ts_data->m_layer_outlines[obj_layer_nr_next], next_layer_vertex);
                SupportNode *     next_node     = m_ts_data->create_node(next_layer_vertex, node.distance_to_top + 1, obj_layer_nr_next, node.support_roof_layers_below - 1, to_buildplate, p_node,
                    print_z_next, height_next);
                // don't increase radius if next node will collide partially with the object (STUDIO-7883)
                to_outside             = projection_onto(next_collision, next_node->position);
                direction_to_outer     = to_outside - node.position;
                double dist_to_outer   = unscale_(direction_to_outer.cast<double>().norm());
                next_node->radius      = std::max(node.radius, std::min(next_node->radius, dist_to_outer));
                get_max_move_dist(next_node);
                m_ts_data->m_mutex.lock();
                contact_nodes[layer_nr_next].push_back(next_node);
                m_ts_data->m_mutex.unlock();
            }
            );
        }

#ifdef SUPPORT_TREE_DEBUG_TO_SVG
        if (contact_nodes[layer_nr].empty() == false) {
            draw_contours_and_nodes_to_svg(debug_out_path("contact_points_%.2f.svg", contact_nodes[layer_nr][0]->print_z), get_collision(0,obj_layer_nr_next),
                                           get_avoidance(branch_radius_temp, obj_layer_nr),
                                           m_ts_data->m_layer_outlines[obj_layer_nr],
            contact_nodes[layer_nr], contact_nodes[layer_nr_next], { "overhang","avoid","outline" }, { "blue","red","yellow" });

            BOOST_LOG_TRIVIAL(debug) << "drop_nodes layer->next " << layer_nr << "->" << layer_nr_next << ", print_z=" << print_z
                << ", num points: " << contact_nodes[layer_nr].size() << "->" << contact_nodes[layer_nr_next].size();
            for (size_t i = 0; i < std::min(size_t(5), contact_nodes[layer_nr].size()); i++) {
                auto &node = contact_nodes[layer_nr][i];
                BOOST_LOG_TRIVIAL(debug) << "\t node " << i << ", pos=" << node->position << ", move = " << node->movement;
            }
        }
#endif

        // Prune all branches that couldn't find support on either the model or the buildplate (resulting in 'mid-air' branches).
        for (;! unsupported_branch_leaves.empty(); unsupported_branch_leaves.pop_back())
        {
            const auto& entry = unsupported_branch_leaves.back();
            SupportNode* i_node = entry.second;
            for (; i_node != nullptr; i_node = i_node->parent)
            {
                size_t i_layer = i_node->obj_layer_nr;
                {
                    // update the parent-child chain
                    if (i_node->parent) {
                        i_node->parent->child = i_node->child;
                        for (SupportNode* parent : i_node->parents) {
                        if (parent->child==i_node)
                            parent->child = i_node->child;
                        }
                    }
                    if (i_node->child) {
                        i_node->child->parent = i_node->parent;
                        i_node->child->parents.erase(std::find(i_node->child->parents.begin(), i_node->child->parents.end(), i_node));
                        append(i_node->child->parents, i_node->parents);
                    }
                    i_node->is_processed = true;  // mark to be deleted later

                    for (SupportNode* neighbour : i_node->merged_neighbours)
                    {
                        if (neighbour && !neighbour->is_processed)
                            unsupported_branch_leaves.push_front({ i_layer, neighbour });
                    }
                }
            }
        }
        for (auto &layer_contact_nodes : contact_nodes) {
            if (!layer_contact_nodes.empty())
                layer_contact_nodes.erase(std::remove_if(layer_contact_nodes.begin(), layer_contact_nodes.end(), [](SupportNode *node) { return node->is_processed; }),
                                          layer_contact_nodes.end());
        }
    }

    BOOST_LOG_TRIVIAL(debug) << "after m_avoidance_cache.size()=" << m_ts_data->m_avoidance_cache.size();
}

void TreeSupport::smooth_nodes()
{
    for (int layer_nr = 0; layer_nr < contact_nodes.size(); layer_nr++) {
        std::vector<SupportNode *> &curr_layer_nodes = contact_nodes[layer_nr];
        if (curr_layer_nodes.empty()) continue;
        for (SupportNode *node : curr_layer_nodes) {
            node->is_processed = false;
        }
    }
    
    float max_move = scale_(m_object_config->support_line_width / 2);
    // if the branch is very tall, the tip also needs extra wall
    float thresh_tall_branch = 100;
    float thresh_dist_to_top = 30;

    for (int layer_nr = 0; layer_nr< contact_nodes.size(); layer_nr++) {
        std::vector<SupportNode *> &curr_layer_nodes = contact_nodes[layer_nr];
        if (curr_layer_nodes.empty()) continue;
        for (SupportNode *node : curr_layer_nodes) {
            if (!node->is_processed) {
                std::vector<Point> pts;
                std::vector<double> radii;
                std::vector<SupportNode *>   branch;
                SupportNode *              p_node = node;
                float                        total_height = 0;
                // add a fixed head if it's not a polygon node, see STUDIO-4403
                // Polygon node can't be added because the move distance might be huge, making the nodes in between jump and dangling
                if (node->child && node->child->type!=ePolygon) {
                    pts.push_back(p_node->child->position);
                    radii.push_back(p_node->child->radius);
                    branch.push_back(p_node->child);
                    total_height += p_node->child->height;
                }
                do {
                    pts.push_back(p_node->position);
                    radii.push_back(p_node->radius);
                    branch.push_back(p_node);
                    total_height += p_node->height;
                    p_node = p_node->parent;
                } while (p_node && !p_node->is_processed);
                if (pts.size() < 3) continue;

                std::vector<Point> pts1 = pts;
                std::vector<double> radii1 = radii;
                // TODO here we assume layer height gap is constant. If not true, need to consider height jump
                const int iterations = 100;
                for (size_t k = 0; k < iterations; k++) {
                    for (size_t i = 1; i < pts.size() - 1; i++) {
                        Point pt = ( pts[i - 1] + pts[i] + pts[i + 1] ) / 3;
                        pts1[i] = pt;
                        radii1[i] = (radii[i - 1] + radii[i] + radii[i + 1] ) / 3;
                        if (k == iterations - 1) {
                            branch[i]->position = pt;
                            branch[i]->radius = radii1[i];
                            branch[i]->movement = (pts[i + 1] - pts[i - 1]) / 2;
                            branch[i]->is_processed = true;
                            if (branch[i]->parents.size() > 1 || (branch[i]->movement.x() > max_move || branch[i]->movement.y() > max_move) ||
                                (total_height > thresh_tall_branch && branch[i]->dist_mm_to_top < thresh_dist_to_top))
                                branch[i]->need_extra_wall = true;
                            BOOST_LOG_TRIVIAL(info) << "smooth_nodes: layer_nr=" << layer_nr << ", i=" << i << ", pt=" << pt << ", movement=" << branch[i]->movement << ", radius=" << branch[i]->radius;
                        }
                    }
                    if (k < iterations - 1) {
                        std::swap(pts, pts1);
                        std::swap(radii, radii1);
                    }
                    else {
                        // interpolate need_extra_wall in the end
                        for (size_t i = 1; i < branch.size() - 1; i++) {
                            if (branch[i - 1]->need_extra_wall && branch[i + 1]->need_extra_wall)
                                branch[i]->need_extra_wall = true;
                        }
                    }
                }
            }
        }
    }
}

std::vector<LayerHeightData> TreeSupport::plan_layer_heights()
{
    std::vector<LayerHeightData> layer_heights;
    std::map<coordf_t, coordf_t> z_heights; // print_z:height
    if (!m_support_params.independent_layer_height) {
        layer_heights.resize(m_object->layer_count());
        for (int layer_nr = 0; layer_nr < m_object->layer_count(); layer_nr++) {
            z_heights[m_object->get_layer(layer_nr)->print_z] = m_object->get_layer(layer_nr)->height;
            layer_heights[layer_nr] = {m_object->get_layer(layer_nr)->print_z, m_object->get_layer(layer_nr)->height, size_t(layer_nr)};
        }
    } else {
        const coordf_t               max_layer_height = m_slicing_params.max_suport_layer_height;
        const coordf_t               min_layer_height = m_slicing_params.min_layer_height;
        std::map<coordf_t, coordf_t> bounds; // print_z: height
        // Keep first layer still
        bounds[m_object->get_layer(0)->print_z] = {m_object->get_layer(0)->height};
        std::vector<float> obj_layer_zs;
        obj_layer_zs.reserve(m_object->layer_count());
        for (const Layer *l : m_object->layers()) obj_layer_zs.emplace_back((float) l->print_z);
        z_heights[m_object->get_layer(0)->print_z] = m_object->get_layer(0)->height;
        // Collect top contact layers
        for (int layer_nr = 1; layer_nr < contact_nodes.size(); layer_nr++) {
            if (!contact_nodes[layer_nr].empty()) {
                coordf_t print_z = contact_nodes[layer_nr].front()->print_z;
                coordf_t height = contact_nodes[layer_nr].front()->height;
                // insertion will fail if there is already a key of print_z, so no need to check
                bounds.insert({print_z, height});
                bounds.insert({print_z - height, 0}); // the bottom_z of the layer
            }
        }

        auto it1 = bounds.begin();
        auto it2 = std::next(it1);
        for (; it2 != bounds.end(); it2++) {
            coordf_t z2   = it2->first;
            coordf_t h2   = it2->second;
            coordf_t z1   = it1->first;
            coordf_t h1   = it1->second;
            coordf_t dist = z2 - z1;
            if (dist < min_layer_height - EPSILON) continue;

            BOOST_LOG_TRIVIAL(trace) << format("plan_layer_heights0 (%.2f,%.2f)->(%.2f,%.2f): ", z1, h1, z2, h2);

            // Insert intermediate layers.
            size_t   n_layers_extra = size_t(ceil(dist / max_layer_height));
            coordf_t step = dist / coordf_t(n_layers_extra);
            coordf_t print_z        = z1 + step;
            for (int i = 0; i < n_layers_extra; i++, print_z += step) {
                z_heights[print_z] = step;
                BOOST_LOG_TRIVIAL(debug) << "plan_layer_heights add entry print_z, height: " << print_z << " " << step;
            }
            it1=it2;
        }

        // map z_heights to layer_heights
        int i                = 0;
        size_t obj_layer_nr     = 0;
        layer_heights.resize(z_heights.size());
        for (auto it = z_heights.begin(); it != z_heights.end(); it++, i++) {
            coordf_t print_z         = it->first;
            coordf_t height          = it->second;
            while (obj_layer_nr < obj_layer_zs.size() && obj_layer_zs[obj_layer_nr] < print_z - height / 2) obj_layer_nr++;
            layer_heights[i]       = {print_z, height, obj_layer_nr};

        }
    }

    // add support layers according to layer_heights
    int support_layer_nr = m_raft_layers;
    for (size_t i = 0; i < layer_heights.size(); i++, support_layer_nr++) {
        SupportLayer *ts_layer = m_object->add_tree_support_layer(support_layer_nr, layer_heights[i].print_z, layer_heights[i].height, layer_heights[i].print_z);
        if (ts_layer->id() > m_raft_layers) {
            SupportLayer *lower_layer = m_object->get_support_layer(ts_layer->id() - 1);
            if (lower_layer) {
                lower_layer->upper_layer = ts_layer;
                ts_layer->lower_layer    = lower_layer;
            }
        }
    }


    // re-distribute contact_nodes to support layers
    decltype(contact_nodes) contact_nodes2(support_layer_nr);
    for (int layer_nr = 0; layer_nr < contact_nodes.size(); layer_nr++) {
        if (contact_nodes[layer_nr].empty()) continue;
        SupportNode *node1 = contact_nodes[layer_nr].front();
        auto         it    = std::min_element(layer_heights.begin(), layer_heights.end(), [node1](const LayerHeightData &l1, const LayerHeightData &l2) {
            return std::abs(l1.print_z - node1->print_z) < std::abs(l2.print_z - node1->print_z);
        });
        if (it == layer_heights.end()) it = std::prev(it);
        int layer_nr2 = std::distance(layer_heights.begin(), it);
        contact_nodes2[layer_nr2].insert(contact_nodes2[layer_nr2].end(), contact_nodes[layer_nr].begin(), contact_nodes[layer_nr].end());
    }
    contact_nodes = contact_nodes2;

    // adjust contact nodes' distance_to_top and support_roof_layers_below according to layer_heights
    // In case of very large top z distance, one gap layer is not enough, we need to split it into multiple layers
    for (int layer_nr = 0; layer_nr < contact_nodes.size(); layer_nr++) {
        if (contact_nodes[layer_nr].empty()) continue;
        SupportNode *node1 = contact_nodes[layer_nr].front();
        BOOST_LOG_TRIVIAL(debug) << format("plan_layer_heights node1->layer_nr,printz,height,distance_to_top: %d, %.2f,%.2f, %d", layer_nr, node1->print_z, node1->height, node1->distance_to_top)
                                 << ", object_layer_zs[" << layer_heights[layer_nr].obj_layer_nr << "]=" << m_object->get_layer(layer_heights[layer_nr].obj_layer_nr)->print_z;
        coordf_t new_height = layer_heights[layer_nr].height;
        if (std::abs(node1->height - new_height) < EPSILON) continue;
        if (top_z_distance < EPSILON && node1->height < EPSILON) continue; // top_z_distance==0, this is soluable interface
        coordf_t              accum_height = 0;
        int                   num_layers   = 0;
        for (int i=layer_nr;i>=0;i--){
            if (layer_heights[i].height > EPSILON) {
                accum_height += layer_heights[i].height;
                num_layers++;
                if (accum_height > node1->height - EPSILON) break;
            }
        }
        BOOST_LOG_TRIVIAL(debug) << format("plan_layer_heights adjust node's height print_z[%d]=%.2f: (%.3f,%d)->(%.3f,%.3f,%d)", layer_nr, node1->print_z,
            node1->height, node1->distance_to_top, new_height, accum_height, -num_layers);
        for (SupportNode *node : contact_nodes[layer_nr]) {
            node->height          = new_height;
            node->distance_to_top = -num_layers;
            node->support_roof_layers_below += num_layers - 1;
        }
    }

    // log layer_heights
    for (size_t i = 0; i < layer_heights.size(); i++) {
        //if (layer_heights[i].height > EPSILON)
            BOOST_LOG_TRIVIAL(trace) << format("plan_layer_heights[%d] print_z, height: %.2f, %.2f",i, layer_heights[i].print_z, layer_heights[i].height);
    }
    return layer_heights;
}

void TreeSupport::generate_contact_points()
{
    const PrintObjectConfig &config = m_object->config();
    const coordf_t point_spread = scale_(config.tree_support_branch_distance.value);
    const coordf_t max_bridge_length = scale_(config.max_bridge_length.value);
    coord_t    radius_scaled         = scale_(base_radius);
    bool       on_buildplate_only    = m_object_config->support_on_build_plate_only.value;
    const bool roof_enabled          = config.support_interface_top_layers.value > 0;
    const bool force_tip_to_roof = roof_enabled && m_support_params.soluble_interface;

    //First generate grid points to cover the entire area of the print.
    BoundingBox bounding_box = m_object->bounding_box();
    const Point bounding_box_size = bounding_box.max - bounding_box.min;
    constexpr double rotate_angle = 22.0 / 180.0 * M_PI;

    const auto center = bounding_box_middle(bounding_box);
    const auto sin_angle = std::sin(rotate_angle);
    const auto cos_angle = std::cos(rotate_angle);
    const Point rotated_dims = Point(
        bounding_box_size(0) * cos_angle + bounding_box_size(1) * sin_angle,
        bounding_box_size(0) * sin_angle + bounding_box_size(1) * cos_angle) / 2;

    std::vector<Point> grid_points;
    coordf_t sample_step = std::max(point_spread, max_bridge_length / 2);
    for (auto x = -rotated_dims(0); x < rotated_dims(0); x += sample_step) {
        for (auto y = -rotated_dims(1); y < rotated_dims(1); y += sample_step) {
            Point pt(x, y);
            pt.rotate(cos_angle, sin_angle);
            pt += center;
            if (bounding_box.contains(pt)) {
                grid_points.push_back(pt);
            }
        }
    }

    const coordf_t layer_height = config.layer_height.value;
    coordf_t       z_distance_top = this->top_z_distance;
  //  if (!m_support_params.independent_layer_height) {
  //      z_distance_top = round(z_distance_top / layer_height) * layer_height;
  //  // BBS: add extra distance if thick bridge is enabled
  //  // Note: normal support uses print_z, but tree support uses integer layers, so we need to subtract layer_height
  //  if (!m_slicing_params.soluble_interface && m_object_config->thick_bridges) {
  //      z_distance_top += m_object->layers()[0]->regions()[0]->region().bridging_height_avg(m_object->print()->config()) - layer_height;
		//}
  //  }
    const int z_distance_top_layers = round_up_divide(scale_(z_distance_top), scale_(layer_height)) + 1; //Support must always be 1 layer below overhang.
    int gap_layers = z_distance_top == 0 ? 0 : 1;

    size_t support_roof_layers = config.support_interface_top_layers.value;
    if (support_roof_layers > 0)
        support_roof_layers += 1; // BBS: add a normal support layer below interface (if we have interface)
    coordf_t  thresh_angle = std::min(89.f, config.support_threshold_angle.value < EPSILON ? 30.f : config.support_threshold_angle.value);
    coordf_t  half_overhang_distance = scale_(tan(thresh_angle * M_PI / 180.0) * layer_height / 2);

    // fix bug of generating support for very thin objects
    if (m_object->layers().size() <= z_distance_top_layers + 1)
        return;

    contact_nodes.clear();
    contact_nodes.resize(m_object->layers().size());

    tbb::spin_mutex mtx;

    // add vertical enforcer points
    std::vector<float> zs = zs_from_layers(m_object->layers());
    std::vector<std::vector<std::pair<Point,Point>>> vertical_enforcer_points_by_layers(m_object->layer_count());
    for (auto& pt_and_normal : m_vertical_enforcer_points) {
        auto pt = pt_and_normal.first;
        auto normal = pt_and_normal.second;  // normal seems useless
        auto iter = std::lower_bound(zs.begin(), zs.end(), pt.z());
        if (iter != zs.end()) {
            size_t layer_nr = iter - zs.begin();
            if (layer_nr > 0 && layer_nr < contact_nodes.size()) {
                vertical_enforcer_points_by_layers[layer_nr].push_back({ to_2d(pt).cast<coord_t>(),scaled(to_2d(normal)) });
            }
        }
    }

    int      nonempty_layers = 0;
    tbb::concurrent_vector<Slic3r::Vec3f> all_nodes;
    tbb::parallel_for(tbb::blocked_range<size_t>(1, m_object->layers().size()), [&](const tbb::blocked_range<size_t>& range) {
        for (size_t layer_nr = range.begin(); layer_nr < range.end(); layer_nr++) {
            if (m_object->print()->canceled())
                break;
            Layer* layer = m_object->get_layer(layer_nr);
            auto& curr_nodes = contact_nodes[layer_nr-1];

            std::unordered_set<Point, PointHash> already_inserted;
            auto                                 bottom_z = m_object->get_layer(layer_nr)->bottom_z();
            bool                                 added = false; // Did we add a point this way?
            bool                                 is_sharp_tail = false;

            // take the least restrictive avoidance possible
            ExPolygons relevant_forbidden = offset_ex(m_ts_data->m_layer_outlines[layer_nr - 1], scale_(MIN_BRANCH_RADIUS));
            // prevent rounding errors down the line, points placed directly on the line of the forbidden area may not be added otherwise.
            relevant_forbidden = offset_ex(union_ex(relevant_forbidden), scaled<float>(0.005), jtMiter, 1.2);


            auto insert_point = [&](Point pt, const ExPolygon& overhang, double radius, bool force_add = false, bool add_interface=true) {
                Point        hash_pos = pt / ((radius_scaled + 1) / 1);
                SupportNode* contact_node = nullptr;
                if (force_add || !already_inserted.count(hash_pos)) {
                    already_inserted.emplace(hash_pos);
                    bool to_buildplate = true;
                    size_t roof_layers = add_interface ? support_roof_layers : 0;
                    // add a new node as a virtual node which acts as the invisible gap between support and object
                    // distance_to_top=-1: it's virtual
                    // print_z=object_layer->bottom_z: it directly contacts the bottom
                    // height=z_distance_top: it's height is exactly the gap distance
                    // dist_mm_to_top=0: it directly contacts the bottom
                    contact_node = m_ts_data->create_node(pt, -gap_layers, layer_nr-1, roof_layers + 1, to_buildplate, SupportNode::NO_PARENT, bottom_z, z_distance_top, 0,
                                                          radius);
                    contact_node->overhang = overhang;
                    contact_node->is_sharp_tail = is_sharp_tail;
                    curr_nodes.emplace_back(contact_node);
                    added = true;
                };
                return contact_node;
                };

            for (const auto& overhang_part : layer->loverhangs)  {
                const auto& overhang_type = this->overhang_types[&overhang_part];
                is_sharp_tail = overhang_type == OverhangType::SharpTail;
                ExPolygons overhangs_regular;
                if (m_support_params.support_style == smsTreeHybrid && overhang_part.area() > m_support_params.thresh_big_overhang && !is_sharp_tail) {
                    overhangs_regular           = offset_ex(intersection_ex({overhang_part}, m_ts_data->m_layer_outlines_below[layer_nr - 1]), radius_scaled);
                    ExPolygons overhangs_normal = diff_ex({overhang_part}, overhangs_regular);
                    if (area(overhangs_normal) > m_support_params.thresh_big_overhang) {
                        // if the outside area is still big, we can need normal nodes
                        for (auto &overhang : overhangs_normal) {
                            BoundingBox  overhang_bounds = get_extents(overhang);
                            double       radius          = unscale_(overhang_bounds.radius());
                            Point        candidate       = overhang_bounds.center();
                            SupportNode *contact_node    = insert_point(candidate, overhang, radius, true, true);
                            contact_node->type           = ePolygon;
                            curr_nodes.emplace_back(contact_node);
                        }
                    }else{
                        // otherwise, all nodes should be circle nodes
                        overhangs_regular = ExPolygons{overhang_part};
                    }
                } else {
                    overhangs_regular = ExPolygons{overhang_part};
                }

                for (auto &overhang : overhangs_regular) {
                    bool add_interface = (force_tip_to_roof || area(overhang) > minimum_roof_area) && !is_sharp_tail;
                    BoundingBox overhang_bounds = get_extents(overhang);
                    double      radius          = std::clamp(unscale_(overhang_bounds.radius()), MIN_BRANCH_RADIUS, base_radius);
                    // add supports at corners for both auto and manual overhangs, github #2008
                    auto &points = overhang.contour.points;
                    int   nSize = points.size();
                    for (int i = 0; i < nSize; i++) {
                        auto pt = points[i];
                        Vec2d v1 = (pt - points[(i - 1 + nSize) % nSize]).cast<double>().normalized();
                        Vec2d v2 = (pt - points[(i + 1) % nSize]).cast<double>().normalized();
                        if (v1.dot(v2) > -0.7) { // angle smaller than 135 degrees
                            SupportNode *contact_node = insert_point(pt, overhang, radius, false, add_interface);
                            if (contact_node) {
                                contact_node->is_corner = true;
                            }
                        }
                    }

                    // add supports along contours
                    libnest2d::placers::EdgeCache<ExPolygon> edge_cache(overhang);
                    for (size_t i = 0; i < edge_cache.holeCount() + 1; i++) {
                        double step     = point_spread / (i == 0 ? edge_cache.circumference() : edge_cache.circumference(i - 1));
                        double distance = 0;
                        while (distance < 1) {
                            auto         pt           = i == 0 ? edge_cache.coords(distance) : edge_cache.coords(i - 1, distance);
                            SupportNode *contact_node = insert_point(pt, overhang,radius, false, add_interface);
                            distance += step;
                        }
                    }

                    // don't add inner supports for sharp tails
                    if (is_sharp_tail) continue;

                    // add inner supports
                    overhang_bounds.inflated(-radius_scaled);
                    ExPolygons overhang_inner = offset_ex(overhang, -radius_scaled);
                    for (Point candidate : grid_points) {
                        if (overhang_bounds.contains(candidate)) {
                            // BBS: move_inside_expoly shouldn't be used if candidate is already inside, as it moves point to boundary and the inside is not well supported!
                            bool is_inside = is_inside_ex(overhang_inner, candidate);
                            if (is_inside) { SupportNode *contact_node = insert_point(candidate, overhang,radius, false, add_interface); }
                        }
                    }
                }
            }
            for (auto& pt_and_normal : vertical_enforcer_points_by_layers[layer_nr]) {
                is_sharp_tail = true;// fake it as sharp tail point so the contact distance will be 0
                auto vertical_enforcer_point= pt_and_normal.first;
                auto node=insert_point(vertical_enforcer_point, ExPolygon(), false);
                if (node)
                    node->skin_direction = pt_and_normal.second;
            }
            if (!curr_nodes.empty()) nonempty_layers++;
            for (auto node : curr_nodes) { all_nodes.emplace_back(node->position(0), node->position(1), scale_(node->print_z)); }
#ifdef SUPPORT_TREE_DEBUG_TO_SVG
            if (!curr_nodes.empty())
            draw_contours_and_nodes_to_svg(debug_out_path("init_contact_points_%.2f.svg", bottom_z), layer->loverhangs,layer->lslices_extrudable, m_ts_data->m_layer_outlines_below[layer_nr],
                contact_nodes[layer_nr], contact_nodes[layer_nr - 1], { "overhang","lslices","outlines_below"});
#endif
        }}
    ); // end tbb::parallel_for



    int nNodes = all_nodes.size();
    avg_node_per_layer = nodes_angle = 0;
    if (nNodes > 0) {
        avg_node_per_layer = nNodes / nonempty_layers;
        // get orientation of nodes by line fitting
        // line: y=kx+b, where
        //       k=tan(nodes_angle)=(n\sum{xy}-\sum{x}\sum{y})/(n\sum{x^2}-\sum{x}^2)
        float mx = 0, my = 0, mxy = 0, mx2 = 0;
        for (auto &pt : all_nodes) {
            float x = unscale_(pt(0));
            float y = unscale_(pt(1));
            mx += x;
            my += y;
            mxy += x * y;
            mx2 += x * x;
        }
        nodes_angle = atan2(nNodes * mxy - mx * my, nNodes * mx2 - SQ(mx));

        BOOST_LOG_TRIVIAL(info) << "avg_node_per_layer=" << avg_node_per_layer << ", nodes_angle=" << nodes_angle;
    }
}

void TreeSupport::insert_dropped_node(std::vector<SupportNode*>& nodes_layer, SupportNode* p_node)
{
    std::vector<SupportNode*>::iterator conflicting_node_it = std::find(nodes_layer.begin(), nodes_layer.end(), p_node);
    if (conflicting_node_it == nodes_layer.end()) //No conflict.
    {
        nodes_layer.emplace_back(p_node);
        return;
    }

    SupportNode* conflicting_node = *conflicting_node_it;
    conflicting_node->distance_to_top = std::max(conflicting_node->distance_to_top, p_node->distance_to_top);
    conflicting_node->support_roof_layers_below = std::max(conflicting_node->support_roof_layers_below, p_node->support_roof_layers_below);
}

TreeSupportData::TreeSupportData(const PrintObject &object, coordf_t xy_distance, coordf_t radius_sample_resolution)
    : m_xy_distance(xy_distance), m_radius_sample_resolution(radius_sample_resolution)
{
    branch_scale_factor = tan(object.config().tree_support_branch_angle.value * M_PI / 180.);
    clear_nodes();
    m_max_move_distances.resize(object.layers().size(), 0);
    for (std::size_t layer_nr  = 0; layer_nr < object.layers().size(); ++layer_nr)
    {
        const Layer* layer = object.get_layer(layer_nr);
        m_max_move_distances[layer_nr] = layer->height * branch_scale_factor;
        m_layer_outlines.push_back(ExPolygons());
        ExPolygons& outline = m_layer_outlines.back();
        for (const ExPolygon& poly : layer->lslices) {
            poly.simplify(scale_(m_radius_sample_resolution), &outline);
        }

        if (layer_nr == 0)
            m_layer_outlines_below.push_back(outline);
        else
            m_layer_outlines_below.push_back(union_ex(m_layer_outlines_below.end()[-1], outline));
    }
}

const ExPolygons& TreeSupportData::get_collision(coordf_t radius, size_t layer_nr) const
{
    profiler.tic();
    radius = ceil_radius(radius);
    RadiusLayerPair key{radius, layer_nr};
    const auto it = m_collision_cache.find(key);
    const ExPolygons& collision = it != m_collision_cache.end() ? it->second : calculate_collision(key);
    profiler.stage_add(STAGE_get_collision);
    return collision;
}

const ExPolygons& TreeSupportData::get_avoidance(coordf_t radius, size_t layer_nr, int recursions) const
{
    profiler.tic();
    radius = ceil_radius(radius);
    RadiusLayerPair key{radius, layer_nr, recursions };
    const auto it = m_avoidance_cache.find(key);
    const ExPolygons& avoidance = it != m_avoidance_cache.end() ? it->second : calculate_avoidance(key);

    profiler.stage_add(STAGE_GET_AVOIDANCE);
    return avoidance;
}

Polygons TreeSupportData::get_contours(size_t layer_nr) const
{
    Polygons contours;
    for (const ExPolygon& expoly : m_layer_outlines[layer_nr]) {
        contours.push_back(expoly.contour);
    }

    return contours;
}

Polygons TreeSupportData::get_contours_with_holes(size_t layer_nr) const
{
    Polygons contours;
    for (const ExPolygon& expoly : m_layer_outlines[layer_nr]) {
        for(int i=0;i<expoly.num_contours();i++)
            contours.push_back(expoly.contour_or_hole(i));
    }
    return contours;
}

SupportNode* TreeSupportData::create_node(const Point position, const int distance_to_top, const int obj_layer_nr, const int support_roof_layers_below, const bool to_buildplate, SupportNode* parent, coordf_t print_z_, coordf_t height_, coordf_t dist_mm_to_top_, coordf_t radius_)
{
    // this function may be called from multiple threads, need to lock
    m_mutex.lock();
    std::unique_ptr<SupportNode> node = std::make_unique<SupportNode>(position, distance_to_top, obj_layer_nr, support_roof_layers_below, to_buildplate, parent, print_z_, height_, dist_mm_to_top_, radius_);
    SupportNode* raw_ptr = node.get();
    contact_nodes.emplace_back(std::move(node));
    m_mutex.unlock();
    if (parent)
        raw_ptr->movement = position - parent->position;
    return raw_ptr;
}

void TreeSupportData::clear_nodes()
{
    tbb::spin_mutex::scoped_lock guard(m_mutex);
    contact_nodes.clear();
}

coordf_t TreeSupportData::ceil_radius(coordf_t radius) const
{
    size_t factor = (size_t)(radius / m_radius_sample_resolution);
    coordf_t remains = radius - m_radius_sample_resolution * factor;
    if (remains > EPSILON) {
        return radius + m_radius_sample_resolution - remains;
    }
    else {
        return radius;
    }
}

const ExPolygons& TreeSupportData::calculate_collision(const RadiusLayerPair& key) const
{
    assert(key.layer_nr < m_layer_outlines.size());

    ExPolygons collision_areas = offset_ex(m_layer_outlines[key.layer_nr], scale_(key.radius+m_xy_distance));
    collision_areas = expolygons_simplify(collision_areas, scale_(m_radius_sample_resolution));
    // collision_areas.emplace_back(m_machine_border);
    const auto ret = m_collision_cache.insert({ key, std::move(collision_areas) });
    return ret.first->second;
}

const ExPolygons& TreeSupportData::calculate_avoidance(const RadiusLayerPair& key) const
{
    const auto &radius = key.radius;
    const auto &layer_nr = key.layer_nr;
    ExPolygons avoidance_areas;
    if (layer_nr > 0) {
        // Avoidance for a given layer depends on all layers beneath it so could have very deep recursion depths if
        // called at high layer heights. We can limit the reqursion depth to N by checking if the layer N
        // below the current one exists and if not, forcing the calculation of that layer. This may cause another recursion
        // if the layer at 2N below the current one but we won't exceed our limit unless there are N*N uncalculated layers
        // below our current one.
        constexpr auto max_recursion_depth = 100;
        // Check if we would exceed the recursion limit by trying to process this layer
        if (layer_nr >= max_recursion_depth && m_avoidance_cache.find({radius, layer_nr - max_recursion_depth}) == m_avoidance_cache.end()) {
            // Force the calculation of the layer `max_recursion_depth` below our current one, ignoring the result.
            get_avoidance(radius, layer_nr - max_recursion_depth);
        }

        avoidance_areas = offset_ex(get_avoidance(radius, layer_nr - 1), scale_(-m_max_move_distances[layer_nr-1]));
    }
    const ExPolygons &collision       = get_collision(radius, layer_nr);
    avoidance_areas.insert(avoidance_areas.end(), collision.begin(), collision.end());
    avoidance_areas = std::move(union_ex(avoidance_areas));
    auto ret = m_avoidance_cache.insert({key, std::move(avoidance_areas)});
    //assert(ret.second);
    return ret.first->second;
}

} //namespace Slic3r
