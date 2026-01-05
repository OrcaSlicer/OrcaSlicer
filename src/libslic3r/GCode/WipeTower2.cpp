// Orca: WipeTower2 for all non bbl printers, support all MMU device and toolchanger.
#include "WipeTower2.hpp"

#include <cassert>
#include <iostream>
#include <vector>
#include <numeric>
#include <memory>
#include <sstream>
#include <iomanip>

#include "ClipperUtils.hpp"
#include "GCodeProcessor.hpp"
#include "BoundingBox.hpp"
#include "LocalesUtils.hpp"
#include "Geometry.hpp"
#include "PrintConfig.hpp"
#include "Surface.hpp"
#include "Fill/FillRectilinear.hpp"

#include <boost/algorithm/string/predicate.hpp>


namespace Slic3r
{

static constexpr float      flat_iron_area                 = 4.f;
constexpr float         flat_iron_speed                = 10.f * 60.f;
static const double     wipe_tower_wall_infill_overlap = 0.0;
static constexpr double WIPE_TOWER_RESOLUTION          = 0.1;
static constexpr double     WT_SIMPLIFY_TOLERANCE_SCALED   = 0.001f / SCALING_FACTOR_INTERNAL;
static constexpr int    arc_fit_size                   = 20;
#define SCALED_WIPE_TOWER_RESOLUTION (WIPE_TOWER_RESOLUTION / SCALING_FACTOR_INTERNAL)
enum class LimitFlow { None, LimitPrintFlow, LimitRammingFlow };
static const std::map<float, float> nozzle_diameter_to_nozzle_change_width{{0.2f, 0.5f}, {0.4f, 1.0f}, {0.6f, 1.2f}, {0.8f, 1.4f}};

inline float align_round(float value, float base) { return std::round(value / base) * base; }

inline float align_ceil(float value, float base) { return std::ceil(value / base) * base; }

inline float align_floor(float value, float base) { return std::floor((value) / base) * base; }

static bool is_valid_gcode(const std::string& gcode)
{
    int  str_size    = gcode.size();
    int  start_index = 0;
    int  end_index   = 0;
    bool is_valid    = false;
    while (end_index < str_size) {
        if (gcode[end_index] != '\n') {
            end_index++;
            continue;
        }

        if (end_index > start_index) {
            std::string line_str = gcode.substr(start_index, end_index - start_index);
            line_str.erase(0, line_str.find_first_not_of(" "));
            line_str.erase(line_str.find_last_not_of(" ") + 1);
            if (!line_str.empty() && line_str[0] != ';') {
                is_valid = true;
                break;
            }
        }

        start_index = end_index + 1;
        end_index   = start_index;
    }

    return is_valid;
}

static Polygon chamfer_polygon(Polygon& polygon, double chamfer_dis = 2., double angle_tol = 30. / 180. * PI)
{
    if (polygon.points.size() < 3)
        return polygon;
    Polygon res;
    res.points.reserve(polygon.points.size() * 2);
    int    mod           = polygon.points.size();
    double cos_angle_tol = abs(std::cos(angle_tol));

    for (int i = 0; i < polygon.points.size(); i++) {
        Vec2d  a      = unscaled(polygon.points[(i - 1 + mod) % mod]);
        Vec2d  b      = unscaled(polygon.points[i]);
        Vec2d  c      = unscaled(polygon.points[(i + 1) % mod]);
        double ab_len = (a - b).norm();
        double bc_len = (b - c).norm();
        Vec2d  ab     = (b - a) / ab_len;
        Vec2d  bc     = (c - b) / bc_len;
        assert(ab_len != 0);
        assert(bc_len != 0);
        float cosangle = ab.dot(bc);
        // std::cout << " angle " << acos(cosangle) << " cosangle " << cosangle << std::endl;
        // std::cout << " ab_len " << ab_len << " bc_len " << bc_len << std::endl;
        if (abs(cosangle) < cos_angle_tol) {
            float real_chamfer_dis = std::min({chamfer_dis, ab_len / 2.1, bc_len / 2.1}); // 2.1 to ensure the points do not coincide
            Vec2d left             = b - ab * real_chamfer_dis;
            Vec2d right            = b + bc * real_chamfer_dis;
            res.points.push_back(scaled(left));
            res.points.push_back(scaled(right));
        } else
            res.points.push_back(polygon.points[i]);
    }
    res.points.shrink_to_fit();
    return res;
}

static Polygon rounding_polygon(Polygon& polygon, double rounding = 2., double angle_tol  = 30. / 180. * PI)
{
    if (polygon.points.size() < 3)
        return polygon;
    Polygon res;
    res.points.reserve(polygon.points.size() * 2);
    int    mod           = polygon.points.size();
    double cos_angle_tol = abs(std::cos(angle_tol));

    for (int i = 0; i < polygon.points.size(); i++) {
        Vec2d  a      = unscaled(polygon.points[(i - 1 + mod) % mod]);
        Vec2d  b      = unscaled(polygon.points[i]);
        Vec2d  c      = unscaled(polygon.points[(i + 1) % mod]);
        double ab_len = (a - b).norm();
        double bc_len = (b - c).norm();
        Vec2d  ab     = (b - a) / ab_len;
        Vec2d  bc     = (c - b) / bc_len;
        assert(ab_len != 0);
        assert(bc_len != 0);
        float cosangle = ab.dot(bc);
        cosangle       = std::clamp(cosangle, -1.f, 1.f);
        bool is_ccw    = cross2(ab, bc) > 0;
        if (abs(cosangle) < cos_angle_tol) {
            float real_rounding_dis = std::min({rounding, ab_len / 2.1, bc_len / 2.1}); // 2.1 to ensure the points do not coincide
            Vec2d left              = b - ab * real_rounding_dis;
            Vec2d right             = b + bc * real_rounding_dis;
            // Point r_left            = scaled(left);
            // Point r_right            = scaled(right);
            // std::cout << " r_left  " << r_left[0] << " " << r_left[1] << std::endl;
            // std::cout << " r_right  " << r_right[0] << " " << r_right[1] << std::endl;
            {
                float half_angle = std::acos(cosangle) / 2.f;
                // std::cout << " half_angle  " << cos(half_angle) << std::endl;

                Vec2d dir  = (right - left).normalized();
                dir        = Vec2d{-dir[1], dir[0]};
                dir        = is_ccw ? dir : -dir;
                double dis = real_rounding_dis / sin(half_angle);
                // std::cout << " dis  " << dis << std::endl;

                Vec2d      center = b + dir * dis;
                double     radius = (left - center).norm();
                ArcSegment arc(scaled(center), scaled(radius), scaled(left), scaled(right),
                               is_ccw ? ArcDirection::Arc_Dir_CCW : ArcDirection::Arc_Dir_CW);
                int        n = arc_fit_size;
                // std::cout << "start  " << arc.start_point[0] << " " << arc.start_point[1] << std::endl;
                // std::cout << "end  " << arc.end_point[0] << " " << arc.end_point[1] << std::endl;
                // std::cout << "start angle   " << arc.polar_start_theta << " end angle " << arc.polar_end_theta << std::endl;
                for (int j = 0; j < n; j++) {
                    float cur_angle = arc.polar_start_theta + (float) j / n * arc.angle_radians;
                    // std::cout << " cur_angle " << cur_angle << std::endl;
                    if (cur_angle > 2 * PI)
                        cur_angle -= 2 * PI;
                    else if (cur_angle < 0)
                        cur_angle += 2 * PI;
                    Point tmp = arc.center + Point{arc.radius * std::cos(cur_angle), arc.radius * std::sin(cur_angle)};
                    // std::cout << "j = " << j << std::endl;
                    // std::cout << "tmp  = " << tmp[0]<<" "<<tmp[1] << std::endl;
                    res.points.push_back(tmp);
                }
            }
            res.points.push_back(scaled(right));
        } else
            res.points.push_back(polygon.points[i]);
    }
    res.remove_duplicate_points();
    res.points.shrink_to_fit();
    return res;
}

static Polygon rounding_rectangle(Polygon& polygon, double rounding = 2., double angle_tol = 30. / 180. * PI)
{
    if (polygon.points.size() < 3)
        return polygon;
    Polygon res;
    res.points.reserve(polygon.points.size() * 2);
    int    mod           = polygon.points.size();
    double cos_angle_tol = abs(std::cos(angle_tol));

    for (int i = 0; i < polygon.points.size(); i++) {
        Vec2d  a      = unscaled(polygon.points[(i - 1 + mod) % mod]);
        Vec2d  b      = unscaled(polygon.points[i]);
        Vec2d  c      = unscaled(polygon.points[(i + 1) % mod]);
        double ab_len = (a - b).norm();
        double bc_len = (b - c).norm();
        Vec2d  ab     = (b - a) / ab_len;
        Vec2d  bc     = (c - b) / bc_len;
        assert(ab_len != 0);
        assert(bc_len != 0);
        float cosangle = ab.dot(bc);
        cosangle       = std::clamp(cosangle, -1.f, 1.f);
        bool is_ccw    = cross2(ab, bc) > 0;
        if (abs(cosangle) < cos_angle_tol) {
            float real_rounding_dis = std::min({rounding, ab_len / 2.1, bc_len / 2.1}); // 2.1 to ensure the points do not coincide
            Vec2d left              = b - ab * real_rounding_dis;
            Vec2d right             = b + bc * real_rounding_dis;
            // Point r_left            = scaled(left);
            // Point r_right           = scaled(right);
            //  std::cout << " r_left  " << r_left[0] << " " << r_left[1] << std::endl;
            //  std::cout << " r_right  " << r_right[0] << " " << r_right[1] << std::endl;
            {
                Vec2d      center = b;
                double     radius = real_rounding_dis;
                ArcSegment arc(scaled(center), scaled(radius), scaled(left), scaled(right),
                               is_ccw ? ArcDirection::Arc_Dir_CCW : ArcDirection::Arc_Dir_CW);
                int        n = arc_fit_size;
                // std::cout << "start  " << arc.start_point[0] << " " << arc.start_point[1] << std::endl;
                // std::cout << "end  " << arc.end_point[0] << " " << arc.end_point[1] << std::endl;
                // std::cout << "start angle   " << arc.polar_start_theta << " end angle " << arc.polar_end_theta << std::endl;
                for (int j = 0; j < n; j++) {
                    float cur_angle = arc.polar_start_theta + (float) j / n * arc.angle_radians;
                    // std::cout << " cur_angle " << cur_angle << std::endl;
                    if (cur_angle > 2 * PI)
                        cur_angle -= 2 * PI;
                    else if (cur_angle < 0)
                        cur_angle += 2 * PI;
                    Point tmp = arc.center + Point{arc.radius * std::cos(cur_angle), arc.radius * std::sin(cur_angle)};
                    // std::cout << "j = " << j << std::endl;
                    // std::cout << "tmp  = " << tmp[0]<<" "<<tmp[1] << std::endl;
                    res.points.push_back(tmp);
                }
            }
            res.points.push_back(scaled(right));
        } else
            res.points.push_back(polygon.points[i]);
    }
    res.points.shrink_to_fit();
    return res;
}

static std::pair<bool, Vec2f> ray_intersetion_line(const Vec2f& a, const Vec2f& v1, const Vec2f& b, const Vec2f& c)
{
    const Vec2f v2    = c - b;
    double      denom = cross2(v1, v2);
    if (fabs(denom) < EPSILON)
        return {false, Vec2f(0, 0)};
    const Vec2f v12    = (a - b);
    double      nume_a = cross2(v2, v12);
    double      nume_b = cross2(v1, v12);
    double      t1     = nume_a / denom;
    double      t2     = nume_b / denom;
    if (t1 >= 0 && t2 >= 0 && t2 <= 1.) {
        // Get the intersection point.
        Vec2f res = a + t1 * v1;
        return std::pair<bool, Vec2f>(true, res);
    }
    return std::pair<bool, Vec2f>(false, Vec2f{0, 0});
}
static Polygon scale_polygon(const std::vector<Vec2f>& points)
{
    Polygon res;
    for (const auto& p : points)
        res.points.push_back(scaled(p));
    return res;
}
static std::vector<Vec2f> unscale_polygon(const Polygon& polygon)
{
    std::vector<Vec2f> res;
    for (const auto& p : polygon.points)
        res.push_back(unscaled<float>(p));
    return res;
}

static Polygon generate_rectange(const Line& line, coord_t offset)
{
    Point p1 = line.a;
    Point p2 = line.b;

    double dx = p2.x() - p1.x();
    double dy = p2.y() - p1.y();

    double length = std::sqrt(dx * dx + dy * dy);

    double ux = dx / length;
    double uy = dy / length;

    double vx = -uy;
    double vy = ux;

    double ox = vx * offset;
    double oy = vy * offset;

    Points rect;
    rect.resize(4);
    rect[0] = {p1.x() + ox, p1.y() + oy};
    rect[1] = {p1.x() - ox, p1.y() - oy};
    rect[2] = {p2.x() - ox, p2.y() - oy};
    rect[3] = {p2.x() + ox, p2.y() + oy};
    Polygon poly(rect);
    return poly;
};

struct Segment
{
    Vec2f      start;
    Vec2f      end;
    bool       is_arc = false;
    ArcSegment arcsegment;
    Segment(const Vec2f& s, const Vec2f& e) : start(s), end(e) {}
    bool is_valid() const { return start.y() < end.y(); }
};

static std::vector<Segment> remove_points_from_segment(const Segment& segment, const std::vector<Vec2f>& skip_points, double range)
{
    std::vector<Segment> result;
    result.push_back(segment);
    float x = segment.start.x();

    for (const Vec2f& point : skip_points) {
        std::vector<Segment> newResult;
        for (const auto& seg : result) {
            if (point.y() + range <= seg.start.y() || point.y() - range >= seg.end.y()) {
                newResult.push_back(seg);
            } else {
                if (point.y() - range > seg.start.y()) {
                    newResult.push_back(Segment(Vec2f(x, seg.start.y()), Vec2f(x, point.y() - range)));
                }
                if (point.y() + range < seg.end.y()) {
                    newResult.push_back(Segment(Vec2f(x, point.y() + range), Vec2f(x, seg.end.y())));
                }
            }
        }

        result = newResult;
    }

    result.erase(std::remove_if(result.begin(), result.end(), [](const Segment& seg) { return !seg.is_valid(); }), result.end());
    return result;
}

struct IntersectionInfo
{
    Vec2f pos;
    int   idx;
    int   pair_idx; // gap_pair idx
    float dis_from_idx;
    bool  is_forward;
};

struct PointWithFlag
{
    Vec2f pos;
    int   pair_idx; // gap_pair idx
    bool  is_forward;
};
static IntersectionInfo move_point_along_polygon(
    const std::vector<Vec2f>& points, const Vec2f& startPoint, int startIdx, float offset, bool forward, int pair_idx)
{
    float            remainingDistance = offset;
    IntersectionInfo res;
    int              mod = points.size();
    if (forward) {
        int next = (startIdx + 1) % mod;
        remainingDistance -= (points[next] - startPoint).norm();
        if (remainingDistance <= 0) {
            res.idx          = startIdx;
            res.pos          = startPoint + (points[next] - startPoint).normalized() * offset;
            res.pair_idx     = pair_idx;
            res.dis_from_idx = (points[startIdx] - res.pos).norm();
            return res;
        } else {
            for (int i = (startIdx + 1) % mod; i != startIdx; i = (i + 1) % mod) {
                float segmentLength = (points[(i + 1) % mod] - points[i]).norm();
                if (remainingDistance <= segmentLength) {
                    float ratio      = remainingDistance / segmentLength;
                    res.idx          = i;
                    res.pos          = points[i] + ratio * (points[(i + 1) % mod] - points[i]);
                    res.dis_from_idx = remainingDistance;
                    res.pair_idx     = pair_idx;
                    return res;
                }
                remainingDistance -= segmentLength;
            }
            res.idx          = (startIdx - 1 + mod) % mod;
            res.pos          = points[startIdx];
            res.pair_idx     = pair_idx;
            res.dis_from_idx = (res.pos - points[res.idx]).norm();
        }
    } else {
        int next = (startIdx + 1) % mod;
        remainingDistance -= (points[startIdx] - startPoint).norm();
        if (remainingDistance <= 0) {
            res.idx          = startIdx;
            res.pos          = startPoint - (points[next] - points[startIdx]).normalized() * offset;
            res.dis_from_idx = (res.pos - points[startIdx]).norm();
            res.pair_idx     = pair_idx;
            return res;
        }
        for (int i = (startIdx - 1 + mod) % mod; i != startIdx; i = (i - 1 + mod) % mod) {
            float segmentLength = (points[(i + 1) % mod] - points[i]).norm();
            if (remainingDistance <= segmentLength) {
                float ratio      = remainingDistance / segmentLength;
                res.idx          = i;
                res.pos          = points[(i + 1) % mod] - ratio * (points[(i + 1) % mod] - points[i]);
                res.dis_from_idx = segmentLength - remainingDistance;
                res.pair_idx     = pair_idx;
                return res;
            }
            remainingDistance -= segmentLength;
        }
        res.idx          = startIdx;
        res.pos          = points[res.idx];
        res.pair_idx     = pair_idx;
        res.dis_from_idx = 0;
    }
    return res;
};

static void insert_points(std::vector<PointWithFlag>& pl, int idx, Vec2f pos, int pair_idx, bool is_forward)
{
    int   next = (idx + 1) % pl.size();
    Vec2f pos1 = pl[idx].pos;
    Vec2f pos2 = pl[next].pos;
    if ((pos - pos1).squaredNorm() < EPSILON) {
        pl[idx].pair_idx   = pair_idx;
        pl[idx].is_forward = is_forward;
    } else if ((pos - pos2).squaredNorm() < EPSILON) {
        pl[next].pair_idx   = pair_idx;
        pl[next].is_forward = is_forward;
    } else {
        pl.insert(pl.begin() + idx + 1, PointWithFlag{pos, pair_idx, is_forward});
    }
}

static Polylines remove_points_from_polygon(
    const Polygon& polygon, const std::vector<Vec2f>& skip_points, double range, bool is_left, Polygon& insert_skip_pg)
{
    assert(polygon.size() > 2);
    Polylines                     result;
    std::vector<PointWithFlag>    new_pl; // add intersection points for gaps, where bool indicates whether it's a gap point.
    std::vector<IntersectionInfo> inter_info;
    Vec2f                         ray          = is_left ? Vec2f(-1, 0) : Vec2f(1, 0);
    auto                          polygon_box  = get_extents(polygon);
    Point                         anchor_point = is_left ? Point{polygon_box.max[0], polygon_box.min[1]} : polygon_box.min; // rd:ld
    std::vector<Vec2f>            points;
    {
        points.reserve(polygon.points.size());
        int      idx      = polygon.closest_point_index(anchor_point);
        Polyline tmp_poly = polygon.split_at_index(idx);
        for (auto& p : tmp_poly)
            points.push_back(unscale(p).cast<float>());
        points.pop_back();
    }

    for (int i = 0; i < skip_points.size(); i++) {
        for (int j = 0; j < points.size(); j++) {
            Vec2f& p1                  = points[j];
            Vec2f& p2                  = points[(j + 1) % points.size()];
            auto [is_inter, inter_pos] = ray_intersetion_line(skip_points[i], ray, p1, p2);
            if (is_inter) {
                IntersectionInfo forward  = move_point_along_polygon(points, inter_pos, j, range, true, i);
                IntersectionInfo backward = move_point_along_polygon(points, inter_pos, j, range, false, i);
                backward.is_forward       = false;
                forward.is_forward        = true;
                inter_info.push_back(backward);
                inter_info.push_back(forward);
                break;
            }
        }
    }

    // insert point to new_pl
    for (const auto& p : points)
        new_pl.push_back({p, -1});
    std::sort(inter_info.begin(), inter_info.end(), [](const IntersectionInfo& lhs, const IntersectionInfo& rhs) {
        if (rhs.idx == lhs.idx)
            return lhs.dis_from_idx < rhs.dis_from_idx;
        return lhs.idx < rhs.idx;
    });
    for (int i = inter_info.size() - 1; i >= 0; i--) {
        insert_points(new_pl, inter_info[i].idx, inter_info[i].pos, inter_info[i].pair_idx, inter_info[i].is_forward);
    }

    {
        // set insert_pg for wipe_path
        for (auto& p : new_pl)
            insert_skip_pg.points.push_back(scaled(p.pos));
    }

    int      beg  = 0;
    bool     skip = true;
    int      i    = beg;
    Polyline pl;

    do {
        if (skip || new_pl[i].pair_idx == -1) {
            pl.points.push_back(scaled(new_pl[i].pos));
            i    = (i + 1) % new_pl.size();
            skip = false;
        } else {
            if (!pl.points.empty()) {
                pl.points.push_back(scaled(new_pl[i].pos));
                result.push_back(pl);
                pl.points.clear();
            }
            int left = new_pl[i].pair_idx;
            int j    = (i + 1) % new_pl.size();
            while (j != beg && new_pl[j].pair_idx != left) {
                if (new_pl[j].pair_idx != -1 && !new_pl[j].is_forward)
                    left = new_pl[j].pair_idx;
                j = (j + 1) % new_pl.size();
            }
            i    = j;
            skip = true;
        }
    } while (i != beg);

    if (!pl.points.empty()) {
        if (new_pl[i].pair_idx == -1)
            pl.points.push_back(scaled(new_pl[i].pos));
        result.push_back(pl);
    }
    return result;
}

static Polylines contrust_gap_for_skip_points(
    const Polygon& polygon, const std::vector<Vec2f>& skip_points, float wt_width, float gap_length, Polygon& insert_skip_polygon)
{
    if (skip_points.empty()) {
        insert_skip_polygon = polygon;
        return Polylines{to_polyline(polygon)};
    }
    bool        is_left = false;
    const auto& pt      = skip_points.front();
    if (abs(pt.x()) < wt_width / 2.f) {
        is_left = true;
    }
    return remove_points_from_polygon(polygon, skip_points, gap_length, is_left, insert_skip_polygon);
};

static Polygon generate_rectange_polygon(const Vec2f& wt_box_min, const Vec2f& wt_box_max)
{
    Polygon res;
    res.points.push_back(scaled(wt_box_min));
    res.points.push_back(scaled(Vec2f{wt_box_max[0], wt_box_min[1]}));
    res.points.push_back(scaled(wt_box_max));
    res.points.push_back(scaled(Vec2f{wt_box_min[0], wt_box_max[1]}));
    return res;
}

// Calculates length of extrusion line to extrude given volume
static float volume_to_length(float volume, float line_width, float layer_height)
{
	return std::max(0.f, volume / (layer_height * (line_width - layer_height * (1.f - float(M_PI) / 4.f))));
}

static float length_to_volume(float length, float line_width, float layer_height)
{
	return std::max(0.f, length * layer_height * (line_width - layer_height * (1.f - float(M_PI) / 4.f)));
}




class WipeTowerWriter2
{
public:
    WipeTowerWriter2(float layer_height,
                     float line_width,
                     GCodeFlavor flavor,
                     const std::vector<WipeTower2::FilamentParameters>& filament_parameters,
                     bool  enable_arc_fitting)
        :
		m_current_pos(std::numeric_limits<float>::max(), std::numeric_limits<float>::max()),
		m_current_z(0.f),
		m_current_feedrate(0.f),
		m_layer_height(layer_height),
		m_extrusion_flow(0.f),
		m_preview_suppressed(false),
		m_elapsed_time(0.f),
        m_gcode_flavor(flavor), m_filpar(filament_parameters)
        //m_enable_arc_fitting(enable_arc_fitting)
    {
            // ORCA: This class is only used by non BBL printers, so set the parameter appropriately.
            // This fixes an issue where the wipe tower was using BBL tags resulting in statistics for purging in the purge tower not being displayed.
            GCodeProcessor::s_IsBBLPrinter = false;
            // adds tag for analyzer:
            std::ostringstream str;
            str << ";" << GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height) << m_layer_height << "\n"; // don't rely on GCodeAnalyzer knowing the layer height - it knows nothing at priming
            str << ";" << GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role) << ExtrusionEntity::role_to_string(erWipeTower) << "\n";
            m_gcode += str.str();
            change_analyzer_line_width(line_width);
    }

    WipeTowerWriter2& change_analyzer_line_width(float line_width) {
        // adds tag for analyzer:
        std::stringstream str;
        str << ";" << GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Width) << line_width << "\n";
        m_gcode += str.str();
        return *this;
    }

	WipeTowerWriter2& 			 set_initial_position(const Vec2f &pos, float width = 0.f, float depth = 0.f, float internal_angle = 0.f) {
        m_wipe_tower_width = width;
        m_wipe_tower_depth = depth;
        m_internal_angle = internal_angle;
		m_start_pos = this->rotate(pos);
		m_current_pos = pos;
		return *this;
	}

    WipeTowerWriter2& 			 set_position(const Vec2f &pos) { m_current_pos = pos; return *this; }

    WipeTowerWriter2&				 set_initial_tool(size_t tool) { m_current_tool = tool; return *this; }

	WipeTowerWriter2&				 set_z(float z) 
		{ m_current_z = z; return *this; }

	WipeTowerWriter2& 			 set_extrusion_flow(float flow)
		{ m_extrusion_flow = flow; return *this; }

	WipeTowerWriter2&				 set_y_shift(float shift) {
        m_current_pos.y() -= shift-m_y_shift;
        m_y_shift = shift;
        return (*this);
    }

    WipeTowerWriter2&            disable_linear_advance() {
        if (m_gcode_flavor == gcfRepRapSprinter || m_gcode_flavor == gcfRepRapFirmware)
            m_gcode += (std::string("M572 D") + std::to_string(m_current_tool) + " S0\n");
        else if (m_gcode_flavor == gcfKlipper)
            m_gcode += "SET_PRESSURE_ADVANCE ADVANCE=0\n";
        else
            m_gcode += "M900 K0\n";
        return *this;
    }

    WipeTowerWriter2& switch_filament_monitoring(bool enable) {
        m_gcode += std::string("G4 S0\n") + "M591 " + (enable ? "R" : "S0") + "\n";
        return *this;
    }

	// Suppress / resume G-code preview in Slic3r. Slic3r will have difficulty to differentiate the various
	// filament loading and cooling moves from normal extrusion moves. Therefore the writer
	// is asked to suppres output of some lines, which look like extrusions.
    WipeTowerWriter2& 			 suppress_preview() { m_preview_suppressed = true; return *this; }
  	WipeTowerWriter2& 			 resume_preview()   { m_preview_suppressed = false; return *this; }

	WipeTowerWriter2& 			 feedrate(float f)
	{
        if (f != m_current_feedrate) {
			m_gcode += "G1" + set_format_F(f) + "\n";
            m_current_feedrate = f;
        }
		return *this;
	}

	const std::string&   gcode() const { return m_gcode; }
	const std::vector<WipeTower::Extrusion>& extrusions() const { return m_extrusions; }
	float                x()     const { return m_current_pos.x(); }
	float                y()     const { return m_current_pos.y(); }
	const Vec2f& 		 pos()   const { return m_current_pos; }
	const Vec2f	 		 start_pos_rotated() const { return m_start_pos; }
	const Vec2f  		 pos_rotated() const { return this->rotate(m_current_pos); }
	float 				 elapsed_time() const { return m_elapsed_time; }
    float                get_and_reset_used_filament_length() { float temp = m_used_filament_length; m_used_filament_length = 0.f; return temp; }

	// Extrude with an explicitely provided amount of extrusion.
	WipeTowerWriter2& extrude_explicit(float x, float y, float e, float f = 0.f, bool record_length = false, bool limit_volumetric_flow = true)
	{
		if (x == m_current_pos.x() && y == m_current_pos.y() && e == 0.f && (f == 0.f || f == m_current_feedrate))
			// Neither extrusion nor a travel move.
			return *this;

		float dx = x - m_current_pos.x();
		float dy = y - m_current_pos.y();
        float len = std::sqrt(dx*dx+dy*dy);
        if (record_length)
            m_used_filament_length += e;

		// Now do the "internal rotation" with respect to the wipe tower center
		Vec2f rotated_current_pos(this->pos_rotated());
		Vec2f rot(this->rotate(Vec2f(x,y)));                               // this is where we want to go

        if (! m_preview_suppressed && e > 0.f && len > 0.f) {
      // Width of a squished extrusion, corrected for the roundings of the squished extrusions.
			// This is left zero if it is a travel move.
      float width = e * m_filpar[0].filament_area / (len * m_layer_height);
			// Correct for the roundings of a squished extrusion.
			width += m_layer_height * float(1. - M_PI / 4.);
			if (m_extrusions.empty() || m_extrusions.back().pos != rotated_current_pos)
				m_extrusions.emplace_back(WipeTower::Extrusion(rotated_current_pos, 0, m_current_tool));
			m_extrusions.emplace_back(WipeTower::Extrusion(rot, width, m_current_tool));
		}

		m_gcode += "G1";
        if (std::abs(rot.x() - rotated_current_pos.x()) > (float)EPSILON)
			m_gcode += set_format_X(rot.x());

        if (std::abs(rot.y() - rotated_current_pos.y()) > (float)EPSILON)
			m_gcode += set_format_Y(rot.y());


		if (e != 0.f)
			m_gcode += set_format_E(e);

		if (f != 0.f && f != m_current_feedrate) {
            if (limit_volumetric_flow) {
                float e_speed = e / (((len == 0.f) ? std::abs(e) : len) / f * 60.f);
                f /= std::max(1.f, e_speed / m_filpar[m_current_tool].max_e_speed);
            }
			m_gcode += set_format_F(f);
        }

        // Append newline if at least one of X,Y,E,F was changed.
        // Otherwise, remove the "G1".
        if (! boost::ends_with(m_gcode, "G1"))
            m_gcode += "\n";
        else
            m_gcode.erase(m_gcode.end()-2, m_gcode.end());

        m_current_pos.x() = x;
        m_current_pos.y() = y;

		// Update the elapsed time with a rough estimate.
        m_elapsed_time += ((len == 0.f) ? std::abs(e) : len) / m_current_feedrate * 60.f;
		return *this;
	}

	WipeTowerWriter2& extrude_explicit(const Vec2f &dest, float e, float f = 0.f, bool record_length = false, bool limit_volumetric_flow = true)
		{ return extrude_explicit(dest.x(), dest.y(), e, f, record_length); }

	// Travel to a new XY position. f=0 means use the current value.
	WipeTowerWriter2& travel(float x, float y, float f = 0.f)
		{ return extrude_explicit(x, y, 0.f, f); }

	WipeTowerWriter2& travel(const Vec2f &dest, float f = 0.f) 
		{ return extrude_explicit(dest.x(), dest.y(), 0.f, f); }

	// Extrude a line from current position to x, y with the extrusion amount given by m_extrusion_flow.
	WipeTowerWriter2& extrude(float x, float y, float f = 0.f)
	{
		float dx = x - m_current_pos.x();
		float dy = y - m_current_pos.y();
        return extrude_explicit(x, y, std::sqrt(dx*dx+dy*dy) * m_extrusion_flow, f, true);
	}

	WipeTowerWriter2& extrude(const Vec2f &dest, const float f = 0.f) 
		{ return extrude(dest.x(), dest.y(), f); }

    WipeTowerWriter2& rectangle(const Vec2f& ld,float width,float height,const float f = 0.f)
    {
        Vec2f corners[4];
        corners[0] = ld;
        corners[1] = ld + Vec2f(width,0.f);
        corners[2] = ld + Vec2f(width,height);
        corners[3] = ld + Vec2f(0.f,height);
        int index_of_closest = 0;
        if (x()-ld.x() > ld.x()+width-x())    // closer to the right
            index_of_closest = 1;
        if (y()-ld.y() > ld.y()+height-y())   // closer to the top
            index_of_closest = (index_of_closest==0 ? 3 : 2);

        travel(corners[index_of_closest].x(), y());      // travel to the closest corner
        travel(x(),corners[index_of_closest].y());

        int i = index_of_closest;
        do {
            ++i;
            if (i==4) i=0;
            extrude(corners[i], f);
        } while (i != index_of_closest);
        return (*this);
    }

    WipeTowerWriter2& rectangle(const WipeTower::box_coordinates& box, const float f = 0.f)
    {
        rectangle(Vec2f(box.ld.x(), box.ld.y()),
                  box.ru.x() - box.lu.x(),
                  box.ru.y() - box.rd.y(), f);
        return (*this);
    }

	WipeTowerWriter2& load(float e, float f = 0.f)
	{
		if (e == 0.f && (f == 0.f || f == m_current_feedrate))
			return *this;
		m_gcode += "G1";
		if (e != 0.f)
			m_gcode += set_format_E(e);
		if (f != 0.f && f != m_current_feedrate)
			m_gcode += set_format_F(f);
		m_gcode += "\n";
		return *this;
	}

	WipeTowerWriter2& retract(float e, float f = 0.f)
		{ return load(-e, f); }

// Loads filament while also moving towards given points in x-axis (x feedrate is limited by cutting the distance short if necessary)
    WipeTowerWriter2& load_move_x_advanced(float farthest_x, float loading_dist, float loading_speed, float max_x_speed = 50.f)
    {
        float time = std::abs(loading_dist / loading_speed); // time that the move must take
        float x_distance = std::abs(farthest_x - x());       // max x-distance that we can travel
        float x_speed = x_distance / time;                   // x-speed to do it in that time

        if (x_speed > max_x_speed) {
            // Necessary x_speed is too high - we must shorten the distance to achieve max_x_speed and still respect the time.
            x_distance = max_x_speed * time;
            x_speed = max_x_speed;
        }

        float end_point = x() + (farthest_x > x() ? 1.f : -1.f) * x_distance;
        return extrude_explicit(end_point, y(), loading_dist, x_speed * 60.f, false, false);
    }

    // Loads filament while also moving towards given point in x-axis. Unlike the previous function, this one respects
    // both the loading_speed and x_speed. Can shorten the move.
    WipeTowerWriter2& load_move_x_advanced_there_and_back(float farthest_x, float e_dist, float e_speed, float x_speed)
    {
        float old_x = x();
        float time = std::abs(e_dist / e_speed); // time that the whole move must take
        float x_max_dist = std::abs(farthest_x - x());       // max x-distance that we can travel
        float x_dist = x_speed * time;                       // totel x-distance to travel during the move
        int n = int(x_dist / (2*x_max_dist) + 1.f);          // how many there and back moves should we do
        float r = 2*n*x_max_dist / x_dist;                   // actual/required dist if the move is not shortened

        float end_point = x() + (farthest_x > x() ? 1.f : -1.f) * x_max_dist / r;
        for (int i=0; i<n; ++i) {
            extrude_explicit(end_point, y(), e_dist/(2.f*n), x_speed * 60.f, false, false);
            extrude_explicit(old_x, y(), e_dist/(2.f*n), x_speed * 60.f, false, false);
        }
        return *this;
    }

	// Elevate the extruder head above the current print_z position.
	WipeTowerWriter2& z_hop(float hop, float f = 0.f)
	{ 
		m_gcode += std::string("G1") + set_format_Z(m_current_z + hop);
		if (f != 0 && f != m_current_feedrate)
			m_gcode += set_format_F(f);
		m_gcode += "\n";
		return *this;
	}

	// Lower the extruder head back to the current print_z position.
	WipeTowerWriter2& z_hop_reset(float f = 0.f) 
		{ return z_hop(0, f); }

	// Move to x1, +y_increment,
	// extrude quickly amount e to x2 with feed f.
	WipeTowerWriter2& ram(float x1, float x2, float dy, float e0, float e, float f)
	{
		extrude_explicit(x1, m_current_pos.y() + dy, e0, f, true, false);
		extrude_explicit(x2, m_current_pos.y(), e, 0.f, true, false);
		return *this;
	}

	// Let the end of the pulled out filament cool down in the cooling tube
	// by moving up and down and moving the print head left / right
	// at the current Y position to spread the leaking material.
	WipeTowerWriter2& cool(float x1, float x2, float e1, float e2, float f)
	{
		extrude_explicit(x1, m_current_pos.y(), e1, f, false, false);
		extrude_explicit(x2, m_current_pos.y(), e2, false, false);
		return *this;
	}

    WipeTowerWriter2& set_tool(size_t tool)
	{
		m_current_tool = tool;
		return *this;
	}

	// Set extruder temperature, don't wait by default.
	WipeTowerWriter2& set_extruder_temp(int temperature, bool wait = false)
	{
        m_gcode += "G4 S0\n"; // to flush planner queue
        m_gcode += "M" + std::to_string(wait ? 109 : 104) + " S" + std::to_string(temperature) + "\n";
        return *this;
    }

    // Wait for a period of time (seconds).
	WipeTowerWriter2& wait(float time)
	{
        if (time==0.f)
            return *this;
        m_gcode += "G4 S" + Slic3r::float_to_string_decimal_point(time, 3) + "\n";
		return *this;
    }

	// Set speed factor override percentage.
	WipeTowerWriter2& speed_override(int speed)
	{
        m_gcode += "M220 S" + std::to_string(speed) + "\n";
		return *this;
    }

	// Let the firmware back up the active speed override value.
	WipeTowerWriter2& speed_override_backup()
    {
        // This is only supported by Prusa at this point (https://github.com/prusa3d/PrusaSlicer/issues/3114)
        if (m_gcode_flavor == gcfMarlinLegacy || m_gcode_flavor == gcfMarlinFirmware)
            m_gcode += "M220 B\n";
		return *this;
    }

	// Let the firmware restore the active speed override value.
	WipeTowerWriter2& speed_override_restore()
	{
        if (m_gcode_flavor == gcfMarlinLegacy || m_gcode_flavor == gcfMarlinFirmware)
            m_gcode += "M220 R\n";
		return *this;
    }

	// Set digital trimpot motor
	WipeTowerWriter2& set_extruder_trimpot(int current)
	{
        if (m_gcode_flavor == gcfKlipper)
            return *this;
        if (m_gcode_flavor == gcfRepRapSprinter || m_gcode_flavor == gcfRepRapFirmware)
            m_gcode += "M906 E";
        else
            m_gcode += "M907 E";
        m_gcode += std::to_string(current) + "\n";
		return *this;
    }

	WipeTowerWriter2& flush_planner_queue()
	{ 
		m_gcode += "G4 S0\n"; 
		return *this;
	}

	// Reset internal extruder counter.
	WipeTowerWriter2& reset_extruder()
	{ 
		m_gcode += "G92 E0\n";
		return *this;
	}

	WipeTowerWriter2& comment_with_value(const char *comment, int value)
    {
        m_gcode += std::string(";") + comment + std::to_string(value) + "\n";
		return *this;
    }


    WipeTowerWriter2& set_fan(unsigned speed)
	{
		if (speed == m_last_fan_speed)
			return *this;
		if (speed == 0)
			m_gcode += "M107\n";
        else
            m_gcode += "M106 S" + std::to_string(unsigned(255.0 * speed / 100.0)) + "\n";
		m_last_fan_speed = speed;
		return *this;
	}

	WipeTowerWriter2& append(const std::string& text) { m_gcode += text; return *this; }

    const std::vector<Vec2f>& wipe_path() const
    {
        return m_wipe_path;
    }

    WipeTowerWriter2& add_wipe_point(const Vec2f& pt)
    {
        m_wipe_path.push_back(rotate(pt));
        return *this;
    }

    WipeTowerWriter2& add_wipe_point(float x, float y)
    {
        return add_wipe_point(Vec2f(x, y));
    }

        	// Extrude with an explicitely provided amount of extrusion.
    WipeTowerWriter2& extrude_arc_explicit(ArcSegment& arc,
                                          float       f             = 0.f,
                                          bool        record_length = false,
                                          LimitFlow   limit_flow    = LimitFlow::LimitPrintFlow)
    {
        float x   = (float) unscale(arc.end_point).x();
        float y   = (float) unscale(arc.end_point).y();
        float len = unscaled<float>(arc.length);
        float e   = len * m_extrusion_flow;
        if (len < (float) EPSILON && e == 0.f && (f == 0.f || f == m_current_feedrate))
            // Neither extrusion nor a travel move.
            return *this;
        if (record_length)
            m_used_filament_length += e;

        // Now do the "internal rotation" with respect to the wipe tower center
        Vec2f rotated_current_pos(this->pos_rotated());
        Vec2f rot(this->rotate(Vec2f(x, y))); // this is where we want to go

        if (!m_preview_suppressed && e > 0.f && len > 0.f) {
#if ENABLE_GCODE_VIEWER_DATA_CHECKING
            change_analyzer_mm3_per_mm(len, e);
#endif // ENABLE_GCODE_VIEWER_DATA_CHECKING
       // Width of a squished extrusion, corrected for the roundings of the squished extrusions.
       // This is left zero if it is a travel move.
            float width = e * m_filpar[0].filament_area / (len * m_layer_height);
            // Correct for the roundings of a squished extrusion.
            width += m_layer_height * float(1. - M_PI / 4.);
            if (m_extrusions.empty() || m_extrusions.back().pos != rotated_current_pos)
                m_extrusions.emplace_back(WipeTower::Extrusion(rotated_current_pos, 0, m_current_tool));
            {
                int n = arc_fit_size;
                for (int j = 0; j < n; j++) {
                    float cur_angle = arc.polar_start_theta + (float) j / n * arc.angle_radians;
                    if (cur_angle > 2 * PI)
                        cur_angle -= 2 * PI;
                    else if (cur_angle < 0)
                        cur_angle += 2 * PI;
                    Point tmp = arc.center + Point{arc.radius * std::cos(cur_angle), arc.radius * std::sin(cur_angle)};
                    m_extrusions.emplace_back(WipeTower::Extrusion(this->rotate(unscaled<float>(tmp)), width, m_current_tool));
                }
                m_extrusions.emplace_back(WipeTower::Extrusion(rot, width, m_current_tool));
            }
        }

        //if (e == 0.f) {
        //    m_gcode += set_travel_acceleration();
        //} else {
        //    m_gcode += set_normal_acceleration();
        //}

        m_gcode += arc.direction == ArcDirection::Arc_Dir_CCW ? "G3" : "G2";
        const Vec2f center_offset = this->rotate(unscaled<float>(arc.center)) - rotated_current_pos;
        m_gcode += set_format_X(rot.x());
        m_gcode += set_format_Y(rot.y());
        m_gcode += set_format_I(center_offset.x());
        m_gcode += set_format_J(center_offset.y());

        if (e != 0.f)
            m_gcode += set_format_E(e);

        if (f != 0.f && f != m_current_feedrate) {
            if (limit_flow != LimitFlow::None) {
                float e_speed = e / (((len == 0.f) ? std::abs(e) : len) / f * 60.f);
                float tmp     = m_filpar[m_current_tool].max_e_speed;
                //if (limit_flow == LimitFlow::LimitRammingFlow)
                //    tmp = m_filpar[m_current_tool].max_e_ramming_speed;
                f /= std::max(1.f, e_speed / tmp);
            }
            m_gcode += set_format_F(f);
        }

        m_current_pos.x() = x;
        m_current_pos.y() = y;

        // Update the elapsed time with a rough estimate.
        m_elapsed_time += ((len == 0.f) ? std::abs(e) : len) / m_current_feedrate * 60.f;
        m_gcode += "\n";
        return *this;
    }

    WipeTowerWriter2& extrude_arc(ArcSegment& arc, float f = 0.f, LimitFlow limit_flow = LimitFlow::LimitPrintFlow)
    {
        return extrude_arc_explicit(arc, f, false, limit_flow);
    }

    void              generate_path(Polylines& pls, float feedrate, float retract_length, float retract_speed, bool used_fillet)
    {
        auto get_closet_idx = [this](std::vector<Segment>& corners) -> int {
            Vec2f anchor{this->m_current_pos.x(), this->m_current_pos.y()};
            int   closestIndex = -1;
            float minDistance  = std::numeric_limits<float>::max();
            for (int i = 0; i < corners.size(); ++i) {
                float distance = (corners[i].start - anchor).squaredNorm();
                if (distance < minDistance) {
                    minDistance  = distance;
                    closestIndex = i;
                }
            }
            return closestIndex;
        };
        std::vector<Segment> segments;
        if (m_enable_arc_fitting) {
            for (auto& pl : pls)
                pl.simplify_by_fitting_arc(SCALED_WIPE_TOWER_RESOLUTION);

            for (const auto& pl : pls) {
                if (pl.points.size() < 2)
                    continue;
                for (int i = 0; i < pl.fitting_result.size(); i++) {
                    if (pl.fitting_result[i].path_type == EMovePathType::Linear_move) {
                        for (int j = pl.fitting_result[i].start_point_index; j < pl.fitting_result[i].end_point_index; j++)
                            segments.push_back({unscaled<float>(pl.points[j]), unscaled<float>(pl.points[j + 1])});
                    } else {
                        int beg = pl.fitting_result[i].start_point_index;
                        int end = pl.fitting_result[i].end_point_index;
                        segments.push_back({unscaled<float>(pl.points[beg]), unscaled<float>(pl.points[end])});
                        segments.back().is_arc     = true;
                        segments.back().arcsegment = pl.fitting_result[i].arc_data;
                    }
                }
            }
            for (auto& pl : pls)
                pl.simplify(SCALED_WIPE_TOWER_RESOLUTION);

        } else {
            for (const auto& pl : pls) {
                if (pl.points.size() < 2)
                    continue;
                for (int i = 0; i < pl.size() - 1; i++) {
                    segments.push_back({unscaled<float>(pl.points[i]), unscaled<float>(pl.points[i + 1])});
                }
            }
        }

        int index_of_closest = get_closet_idx(segments);
        int i                = index_of_closest;
        travel(segments[i].start); // travel to the closest points
        segments[i].is_arc ? extrude_arc(segments[i].arcsegment, feedrate) : extrude(segments[i].end, feedrate);
        do {
            i = (i + 1) % segments.size();
            if (i == index_of_closest)
                break;
            float dx  = segments[i].start.x() - m_current_pos.x();
            float dy  = segments[i].start.y() - m_current_pos.y();
            float len = std::sqrt(dx * dx + dy * dy);
            if (len > EPSILON) {
                retract(retract_length, retract_speed);
                travel(segments[i].start, 600.);
                retract(-retract_length, retract_speed);
            }
            segments[i].is_arc ? extrude_arc(segments[i].arcsegment, feedrate) : extrude(segments[i].end, feedrate);
        } while (1);
    }

private:
	Vec2f         m_start_pos;
	Vec2f         m_current_pos;
    std::vector<Vec2f>  m_wipe_path;
	float    	  m_current_z;
	float 	  	  m_current_feedrate;
    size_t        m_current_tool;
	float 		  m_layer_height;
	float 	  	  m_extrusion_flow;
	bool		  m_preview_suppressed;
	std::string   m_gcode;
	std::vector<WipeTower::Extrusion> m_extrusions;
	float         m_elapsed_time;
	float   	  m_internal_angle = 0.f;
	float		  m_y_shift = 0.f;
	float 		  m_wipe_tower_width = 0.f;
	float		  m_wipe_tower_depth = 0.f;
    unsigned      m_last_fan_speed = 0;
    int           current_temp = -1;
    float         m_used_filament_length = 0.f;
    GCodeFlavor   m_gcode_flavor;
    bool          m_enable_arc_fitting = false;
    const std::vector<WipeTower2::FilamentParameters>& m_filpar;

	std::string   set_format_X(float x)
    {
        m_current_pos.x() = x;
        return " X" + Slic3r::float_to_string_decimal_point(x, 3);
	}

	std::string   set_format_Y(float y) {
        m_current_pos.y() = y;
        return " Y" + Slic3r::float_to_string_decimal_point(y, 3);
	}

	std::string   set_format_Z(float z) {
        return " Z" + Slic3r::float_to_string_decimal_point(z, 3);
	}

	std::string   set_format_E(float e) {
        return " E" + Slic3r::float_to_string_decimal_point(e, 4);
	}

	std::string   set_format_F(float f) {
        char buf[64];
        sprintf(buf, " F%d", int(floor(f + 0.5f)));
        m_current_feedrate = f;
        return buf;
	}
    std::string       set_format_I(float i) { return " I" + Slic3r::float_to_string_decimal_point(i, 3); }
    std::string       set_format_J(float j) { return " J" + Slic3r::float_to_string_decimal_point(j, 3); }

	WipeTowerWriter2& operator=(const WipeTowerWriter2 &rhs);

	// Rotate the point around center of the wipe tower about given angle (in degrees)
	Vec2f rotate(Vec2f pt) const
	{
		pt.x() -= m_wipe_tower_width / 2.f;
		pt.y() += m_y_shift - m_wipe_tower_depth / 2.f;
	    double angle = m_internal_angle * float(M_PI/180.);
	    double c = cos(angle);
	    double s = sin(angle);
	    return Vec2f(float(pt.x() * c - pt.y() * s) + m_wipe_tower_width / 2.f, float(pt.x() * s + pt.y() * c) + m_wipe_tower_depth / 2.f);
	}

}; // class WipeTowerWriter2



WipeTower::ToolChangeResult WipeTower2::construct_tcr(WipeTowerWriter2& writer,
                                                     bool priming,
                                                     size_t old_tool,
                                                     bool is_finish) const
{
    WipeTower::ToolChangeResult result;
    result.priming      = priming;
    result.initial_tool = int(old_tool);
    result.new_tool     = int(m_current_tool);
    result.print_z      = m_z_pos;
    result.layer_height = m_layer_height;
    result.elapsed_time = writer.elapsed_time();
    result.start_pos    = writer.start_pos_rotated();
    result.end_pos      = priming ? writer.pos() : writer.pos_rotated();
    result.gcode        = std::move(writer.gcode());
    result.extrusions   = std::move(writer.extrusions());
    result.wipe_path    = std::move(writer.wipe_path());
    result.is_finish_first = is_finish;
    // ORCA: Always initialize the tool_change_start_pos with a valid position
    // to avoid undefined variable travel on X in Gcode.cpp function std::string WipeTowerIntegration::post_process_wipe_tower_moves
    result.tool_change_start_pos = result.start_pos;  // always valid fallback

    return result;
}



WipeTower2::WipeTower2(const PrintConfig& config, const PrintRegionConfig& default_region_config,int plate_idx, Vec3d plate_origin, const std::vector<std::vector<float>>& wiping_matrix, size_t initial_tool) :
    m_semm(config.single_extruder_multi_material.value),
    m_enable_filament_ramming(config.enable_filament_ramming.value),
    m_wipe_tower_pos(config.wipe_tower_x.get_at(plate_idx), config.wipe_tower_y.get_at(plate_idx)),
    m_wipe_tower_width(float(config.prime_tower_width)),
    m_wipe_tower_rotation_angle(float(config.wipe_tower_rotation_angle)),
    m_wipe_tower_brim_width(float(config.prime_tower_brim_width)),
    m_wipe_tower_cone_angle(float(config.wipe_tower_cone_angle)),
    m_extra_flow(float(config.wipe_tower_extra_flow/100.)),
    m_extra_spacing_wipe(float(config.wipe_tower_extra_spacing/100. * config.wipe_tower_extra_flow/100.)),
    m_extra_spacing_ramming(float(config.wipe_tower_extra_spacing/100.)),
    m_y_shift(0.f),
    m_z_pos(0.f),
    m_bridging(float(config.wipe_tower_bridging)),
    m_no_sparse_layers(config.wipe_tower_no_sparse_layers),
    m_gcode_flavor(config.gcode_flavor),
    m_travel_speed(config.travel_speed),
    m_infill_speed(default_region_config.sparse_infill_speed),
    m_perimeter_speed(default_region_config.inner_wall_speed),
    m_current_tool(initial_tool),
    wipe_volumes(wiping_matrix), m_wipe_tower_max_purge_speed(float(config.wipe_tower_max_purge_speed)),
    m_enable_arc_fitting(config.enable_arc_fitting), 
    m_used_fillet(config.wipe_tower_fillet_wall), 
    m_rib_width(config.wipe_tower_rib_width), 
    m_extra_rib_length(config.wipe_tower_extra_rib_length),
    m_wall_type((int)config.wipe_tower_wall_type)
{
    // Read absolute value of first layer speed, if given as percentage,
    // it is taken over following default. Speeds from config are not
    // easily accessible here.
    const float default_speed = 60.f;
    m_first_layer_speed = config.initial_layer_speed;
    if (m_first_layer_speed == 0.f) // just to make sure autospeed doesn't break it.
        m_first_layer_speed = default_speed / 2.f;

    // Autospeed may be used...
    if (m_infill_speed == 0.f)
        m_infill_speed = 80.f;
    if (m_perimeter_speed == 0.f)
        m_perimeter_speed = 80.f;


    // If this is a single extruder MM printer, we will use all the SE-specific config values.
    // Otherwise, the defaults will be used to turn off the SE stuff.
    if (m_semm) {
        m_cooling_tube_retraction = float(config.cooling_tube_retraction);
        m_cooling_tube_length     = float(config.cooling_tube_length);
        m_parking_pos_retraction  = float(config.parking_pos_retraction);
        m_extra_loading_move      = float(config.extra_loading_move);
        m_set_extruder_trimpot    = config.high_current_on_filament_swap;
    }

    m_is_mk4mmu3 = boost::icontains(config.printer_notes.value, "PRINTER_MODEL_MK4") && boost::icontains(config.printer_notes.value, "MMU");

    // Calculate where the priming lines should be - very naive test not detecting parallelograms etc.
    const std::vector<Vec2d>& bed_points = config.printable_area.values;
    BoundingBoxf bb(bed_points);
    m_bed_width = float(bb.size().x());
    m_bed_shape = (bed_points.size() == 4 ? RectangularBed : CircularBed);

    if (m_bed_shape == CircularBed) {
        // this may still be a custom bed, check that the points are roughly on a circle
        double r2 = std::pow(m_bed_width/2., 2.);
        double lim2 = std::pow(m_bed_width/10., 2.);
        Vec2d center = bb.center();
        for (const Vec2d& pt : bed_points)
            if (std::abs(std::pow(pt.x()-center.x(), 2.) + std::pow(pt.y()-center.y(), 2.) - r2) > lim2) {
                m_bed_shape = CustomBed;
                break;
            }
    }

    m_bed_bottom_left = m_bed_shape == RectangularBed
                  ? Vec2f(bed_points.front().x(), bed_points.front().y())
                  : Vec2f::Zero();
}



void WipeTower2::set_extruder(size_t idx, const PrintConfig& config)
{
    //while (m_filpar.size() < idx+1)   // makes sure the required element is in the vector
    m_filpar.push_back(FilamentParameters());

    m_filpar[idx].material = config.filament_type.get_at(idx);
    m_filpar[idx].is_soluble = config.filament_soluble.get_at(idx);
    m_filpar[idx].temperature = config.nozzle_temperature.get_at(idx);
    m_filpar[idx].first_layer_temperature = config.nozzle_temperature_initial_layer.get_at(idx);
    m_filpar[idx].filament_minimal_purge_on_wipe_tower = config.filament_minimal_purge_on_wipe_tower.get_at(idx);

    // If this is a single extruder MM printer, we will use all the SE-specific config values.
    // Otherwise, the defaults will be used to turn off the SE stuff.
    if (m_semm) {
        m_filpar[idx].loading_speed           = float(config.filament_loading_speed.get_at(idx));
        m_filpar[idx].loading_speed_start     = float(config.filament_loading_speed_start.get_at(idx));
        m_filpar[idx].unloading_speed         = float(config.filament_unloading_speed.get_at(idx));
        m_filpar[idx].unloading_speed_start   = float(config.filament_unloading_speed_start.get_at(idx));
        m_filpar[idx].delay                   = float(config.filament_toolchange_delay.get_at(idx));
        m_filpar[idx].cooling_moves           = config.filament_cooling_moves.get_at(idx);
        m_filpar[idx].cooling_initial_speed   = float(config.filament_cooling_initial_speed.get_at(idx));
        m_filpar[idx].cooling_final_speed     = float(config.filament_cooling_final_speed.get_at(idx));
        m_filpar[idx].filament_stamping_loading_speed     = float(config.filament_stamping_loading_speed.get_at(idx));
        m_filpar[idx].filament_stamping_distance          = float(config.filament_stamping_distance.get_at(idx));
    }

    m_filpar[idx].filament_area = float((M_PI/4.f) * pow(config.filament_diameter.get_at(idx), 2)); // all extruders are assumed to have the same filament diameter at this point
    float nozzle_diameter = float(config.nozzle_diameter.get_at(idx));
    m_filpar[idx].nozzle_diameter = nozzle_diameter; // to be used in future with (non-single) multiextruder MM

    float max_vol_speed = float(config.filament_max_volumetric_speed.get_at(idx));
    if (max_vol_speed!= 0.f)
        m_filpar[idx].max_e_speed = (max_vol_speed / filament_area());

    m_perimeter_width = nozzle_diameter * Width_To_Nozzle_Ratio; // all extruders are now assumed to have the same diameter

    if (m_semm) {
        std::istringstream stream{config.filament_ramming_parameters.get_at(idx)};
        float speed = 0.f;
        stream >> m_filpar[idx].ramming_line_width_multiplicator >> m_filpar[idx].ramming_step_multiplicator;
        m_filpar[idx].ramming_line_width_multiplicator /= 100;
        m_filpar[idx].ramming_step_multiplicator /= 100;
        while (stream >> speed)
            m_filpar[idx].ramming_speed.push_back(speed);
        // ramming_speed now contains speeds to be used for every 0.25s piece of the ramming line.
        // This allows to have the ramming flow variable. The 0.25s value is how it is saved in config
        // and the same time step has to be used when the ramming is performed.
    } else {
        // We will use the same variables internally, but the correspondence to the configuration options will be different.
        float vol  = config.filament_multitool_ramming_volume.get_at(idx);
        float flow = config.filament_multitool_ramming_flow.get_at(idx);
        m_filpar[idx].multitool_ramming = config.filament_multitool_ramming.get_at(idx) && vol > 0.f && flow > 0.f;
        m_filpar[idx].ramming_line_width_multiplicator = 2.;
        m_filpar[idx].ramming_step_multiplicator = 1.;

        // Now the ramming speed vector. In this case it contains just one value (flow).
        // The time is calculated and saved separately. This is here so that the MM ramming
        // is not limited by the 0.25s granularity - it is not possible to create a SEMM-style
        // ramming_speed vector that would respect both the volume and flow (because of 
        // rounding issues with small volumes and high flow).
        m_filpar[idx].ramming_speed.push_back(flow);
        m_filpar[idx].multitool_ramming_time = flow > 0.f ? vol/flow : 0.f;
    }

    m_used_filament_length.resize(std::max(m_used_filament_length.size(), idx + 1)); // makes sure that the vector is big enough so we don't have to check later

    m_filpar[idx].retract_length = config.retraction_length.get_at(idx);
    m_filpar[idx].retract_speed  = config.retraction_speed.get_at(idx);
}



// Returns gcode to prime the nozzles at the front edge of the print bed.
std::vector<WipeTower::ToolChangeResult> WipeTower2::prime(
	// print_z of the first layer.
	float 						initial_layer_print_height, 
	// Extruder indices, in the order to be primed. The last extruder will later print the wipe tower brim, print brim and the object.
	const std::vector<unsigned int> &tools,
	// If true, the last priming are will be the same as the other priming areas, and the rest of the wipe will be performed inside the wipe tower.
	// If false, the last priming are will be large enough to wipe the last extruder sufficiently.
    bool 						/*last_wipe_inside_wipe_tower*/)
{
	this->set_layer(initial_layer_print_height, initial_layer_print_height, tools.size(), true, false);
	m_current_tool 		= tools.front();
    
    // The Prusa i3 MK2 has a working space of [0, -2.2] to [250, 210].
    // Due to the XYZ calibration, this working space may shrink slightly from all directions,
    // therefore the homing position is shifted inside the bed by 0.2 in the firmware to [0.2, -2.0].
//	WipeTower::box_coordinates cleaning_box(xy(0.5f, - 1.5f), m_wipe_tower_width, wipe_area);

    float prime_section_width = std::min(0.9f * m_bed_width / tools.size(), 60.f);
    WipeTower::box_coordinates cleaning_box(Vec2f(0.02f * m_bed_width, 0.01f + m_perimeter_width/2.f), prime_section_width, 100.f);
    if (m_bed_shape == CircularBed) {
        cleaning_box = WipeTower::box_coordinates(Vec2f(0.f, 0.f), prime_section_width, 100.f);
        float total_width_half = tools.size() * prime_section_width / 2.f;
        cleaning_box.translate(-total_width_half, -std::sqrt(std::max(0.f, std::pow(m_bed_width/2, 2.f) - std::pow(1.05f * total_width_half, 2.f))));
    }
    else
        cleaning_box.translate(m_bed_bottom_left);

    std::vector<WipeTower::ToolChangeResult> results;

    // Iterate over all priming toolchanges and push respective ToolChangeResults into results vector.
    for (size_t idx_tool = 0; idx_tool < tools.size(); ++ idx_tool) {
        size_t old_tool = m_current_tool;

        WipeTowerWriter2 writer(m_layer_height, m_perimeter_width, m_gcode_flavor, m_filpar, m_enable_arc_fitting);
        writer.set_extrusion_flow(m_extrusion_flow)
              .set_z(m_z_pos)
              .set_initial_tool(m_current_tool);

        // This is the first toolchange - initiate priming
        if (idx_tool == 0) {
            writer.append(";--------------------\n"
                          "; CP PRIMING START\n")
                  .append(";--------------------\n")
                  .speed_override_backup()
                  .speed_override(100)
                  .set_initial_position(Vec2f::Zero())	// Always move to the starting position
                  .travel(cleaning_box.ld, 7200);
            if (m_set_extruder_trimpot)
                writer.set_extruder_trimpot(750); 			// Increase the extruder driver current to allow fast ramming.
        }
        else
            writer.set_initial_position(results.back().end_pos);


        unsigned int tool = tools[idx_tool];
        m_left_to_right = true;
        toolchange_Change(writer, tool, m_filpar[tool].material); // Select the tool, set a speed override for soluble and flex materials.
        toolchange_Load(writer, cleaning_box); // Prime the tool.
        if (idx_tool + 1 == tools.size()) {
            // Last tool should not be unloaded, but it should be wiped enough to become of a pure color.
            toolchange_Wipe(writer, cleaning_box, wipe_volumes[tools[idx_tool-1]][tool]);
        } else {
            // Ram the hot material out of the melt zone, retract the filament into the cooling tubes and let it cool.
            //writer.travel(writer.x(), writer.y() + m_perimeter_width, 7200);
            toolchange_Wipe(writer, cleaning_box , 20.f);
            WipeTower::box_coordinates box = cleaning_box;
            box.translate(0.f, writer.y() - cleaning_box.ld.y() + m_perimeter_width);
            toolchange_Unload(writer, box , m_filpar[m_current_tool].material, m_filpar[m_current_tool].first_layer_temperature, m_filpar[tools[idx_tool + 1]].first_layer_temperature);
            cleaning_box.translate(prime_section_width, 0.f);
            writer.travel(cleaning_box.ld, 7200);
        }
        ++ m_num_tool_changes;


        // Ask our writer about how much material was consumed:
        if (m_current_tool < m_used_filament_length.size())
            m_used_filament_length[m_current_tool] += writer.get_and_reset_used_filament_length();

        // This is the last priming toolchange - finish priming
        if (idx_tool+1 == tools.size()) {
            // Reset the extruder current to a normal value.
            if (m_set_extruder_trimpot)
                writer.set_extruder_trimpot(550);
            writer.speed_override_restore()
                  .feedrate(m_travel_speed * 60.f)
                  .flush_planner_queue()
                  .reset_extruder()
                  .append("; CP PRIMING END\n"
                          ";------------------\n"
                          "\n\n");
        }

        results.emplace_back(construct_tcr(writer, true, old_tool, true));
    }

    m_old_temperature = -1; // If the priming is turned off in config, the temperature changing commands will not actually appear
                            // in the output gcode - we should not remember emitting them (we will output them twice in the worst case)

	return results;
}

WipeTower::ToolChangeResult WipeTower2::tool_change(size_t tool)
{
    size_t old_tool = m_current_tool;

    float wipe_area = 0.f;
	float wipe_volume = 0.f;
	
	// Finds this toolchange info
	if (tool != (unsigned int)(-1))
	{
		for (const auto &b : m_layer_info->tool_changes)
			if ( b.new_tool == tool ) {
                wipe_volume = b.wipe_volume;
				wipe_area = b.required_depth;
				break;
			}
	}
	else {
		// Otherwise we are going to Unload only. And m_layer_info would be invalid.
	}

    WipeTower::box_coordinates cleaning_box(
		Vec2f(m_perimeter_width / 2.f, m_perimeter_width / 2.f),
		m_wipe_tower_width - m_perimeter_width,
        (tool != (unsigned int)(-1) ? wipe_area+m_depth_traversed-0.5f*m_perimeter_width
                                    : m_wipe_tower_depth-m_perimeter_width));

	WipeTowerWriter2 writer(m_layer_height, m_perimeter_width, m_gcode_flavor, m_filpar, m_enable_arc_fitting);
	writer.set_extrusion_flow(m_extrusion_flow)
		.set_z(m_z_pos)
		.set_initial_tool(m_current_tool)
        .set_y_shift(m_y_shift + (tool!=(unsigned int)(-1) && (m_current_shape == SHAPE_REVERSED) ? m_layer_info->depth - m_layer_info->toolchanges_depth(): 0.f))
		.append(";--------------------\n"
				"; CP TOOLCHANGE START\n");

    if (tool != (unsigned)(-1)){
        writer.comment_with_value(" toolchange #", m_num_tool_changes + 1); // the number is zero-based
        writer.append(std::string("; material : " + (m_current_tool < m_filpar.size() ? m_filpar[m_current_tool].material : "(NONE)") + " -> " + m_filpar[tool].material + "\n").c_str())
            .append(";--------------------\n");
        writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_Start) + "\n");
    }

    writer.speed_override_backup();
	writer.speed_override(100);

	Vec2f initial_position = cleaning_box.ld + Vec2f(0.f, m_depth_traversed);
    writer.set_initial_position(initial_position, m_wipe_tower_width, m_wipe_tower_depth, m_internal_rotation);

    // Increase the extruder driver current to allow fast ramming.
	if (m_set_extruder_trimpot)
		writer.set_extruder_trimpot(750);

    // Ram the hot material out of the melt zone, retract the filament into the cooling tubes and let it cool.
    if (tool != (unsigned int)-1){ 			// This is not the last change.
        auto new_tool_temp = is_first_layer() ? m_filpar[tool].first_layer_temperature : m_filpar[tool].temperature;
        toolchange_Unload(writer, cleaning_box, m_filpar[m_current_tool].material,
                          (is_first_layer() ? m_filpar[m_current_tool].first_layer_temperature : m_filpar[m_current_tool].temperature),
                          new_tool_temp);
        toolchange_Change(writer, tool, m_filpar[tool].material); // Change the tool, set a speed override for soluble and flex materials.
        toolchange_Load(writer, cleaning_box);
        writer.travel(writer.x(), writer.y()-m_perimeter_width); // cooling and loading were done a bit down the road
        toolchange_Wipe(writer, cleaning_box, wipe_volume);     // Wipe the newly loaded filament until the end of the assigned wipe area.
        writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_End) + "\n");
        ++ m_num_tool_changes;
    } else
        toolchange_Unload(writer, cleaning_box, m_filpar[m_current_tool].material, m_filpar[m_current_tool].temperature, m_filpar[m_current_tool].temperature);

    m_depth_traversed += wipe_area;

	if (m_set_extruder_trimpot)
		writer.set_extruder_trimpot(550);    // Reset the extruder current to a normal value.
	writer.speed_override_restore();
    writer.feedrate(m_travel_speed * 60.f)
          .flush_planner_queue()
          .reset_extruder()
          .append("; CP TOOLCHANGE END\n"
                  ";------------------\n"
                  "\n\n");

    // Ask our writer about how much material was consumed:
    if (m_current_tool < m_used_filament_length.size())
        m_used_filament_length[m_current_tool] += writer.get_and_reset_used_filament_length();

    return construct_tcr(writer, false, old_tool, false);
}


// Ram the hot material out of the melt zone, retract the filament into the cooling tubes and let it cool.
void WipeTower2::toolchange_Unload(
	WipeTowerWriter2 &writer,
	const WipeTower::box_coordinates 	&cleaning_box,
	const std::string&		 current_material,
    const int                old_temperature,
	const int 				 new_temperature)
{
	float xl = cleaning_box.ld.x() + 1.f * m_perimeter_width;
	float xr = cleaning_box.rd.x() - 1.f * m_perimeter_width;

    const float line_width = m_perimeter_width * m_filpar[m_current_tool].ramming_line_width_multiplicator;       // desired ramming line thickness
	const float y_step = line_width * m_filpar[m_current_tool].ramming_step_multiplicator * m_extra_spacing_ramming; // spacing between lines in mm

    const Vec2f ramming_start_pos = Vec2f(xl, cleaning_box.ld.y() + m_depth_traversed + y_step/2.f);

    writer.append("; CP TOOLCHANGE UNLOAD\n")
        .change_analyzer_line_width(line_width);

	unsigned i = 0;										// iterates through ramming_speed
	m_left_to_right = true;								// current direction of ramming
	float remaining = xr - xl ;							// keeps track of distance to the next turnaround
	float e_done = 0;									// measures E move done from each segment   

    // Orca: Do ramming when SEMM and ramming is enabled or when multi tool head when ramming is enabled on the multi tool.
    const bool do_ramming = (m_semm && m_enable_filament_ramming) || m_filpar[m_current_tool].multitool_ramming;
    const bool cold_ramming = m_is_mk4mmu3;

    if (do_ramming) {
        writer.travel(ramming_start_pos); // move to starting position
        if (! m_is_mk4mmu3)
            writer.disable_linear_advance();
        if (cold_ramming)
            writer.set_extruder_temp(old_temperature - 20);
    }
    else
        writer.set_position(ramming_start_pos);

    // if the ending point of the ram would end up in mid air, align it with the end of the wipe tower:
    if (do_ramming && (m_layer_info > m_plan.begin() && m_layer_info < m_plan.end() && (m_layer_info-1!=m_plan.begin() || !m_adhesion ))) {

        // this is y of the center of previous sparse infill border
        float sparse_beginning_y = 0.f;
        if (m_current_shape == SHAPE_REVERSED)
            sparse_beginning_y += ((m_layer_info-1)->depth - (m_layer_info-1)->toolchanges_depth())
                                      - ((m_layer_info)->depth-(m_layer_info)->toolchanges_depth()) ;
        else
            sparse_beginning_y += (m_layer_info-1)->toolchanges_depth() + m_perimeter_width;

        float sum_of_depths = 0.f;
        for (const auto& tch : m_layer_info->tool_changes) {  // let's find this toolchange
            if (tch.old_tool == m_current_tool) {
                sum_of_depths += tch.ramming_depth;
                float ramming_end_y = sum_of_depths;
                ramming_end_y -= (y_step/m_extra_spacing_ramming-m_perimeter_width) / 2.f;   // center of final ramming line

                if ( (m_current_shape == SHAPE_REVERSED   && ramming_end_y < sparse_beginning_y - 0.5f*m_perimeter_width  ) ||
                     (m_current_shape == SHAPE_NORMAL && ramming_end_y > sparse_beginning_y + 0.5f*m_perimeter_width  )  )
                {
                    writer.extrude(xl + tch.first_wipe_line-1.f*m_perimeter_width,writer.y());
                    remaining -= tch.first_wipe_line-1.f*m_perimeter_width;
                }
                break;
            }
            sum_of_depths += tch.required_depth;
        }
    }

    if (m_is_mk4mmu3) {
        writer.switch_filament_monitoring(false);
        writer.wait(1.5f);
    }
    

    // now the ramming itself:
    while (do_ramming && i < m_filpar[m_current_tool].ramming_speed.size())
    {
        // The time step is different for SEMM ramming and the MM ramming. See comments in set_extruder() for details.
        const float time_step = m_semm ? 0.25f : m_filpar[m_current_tool].multitool_ramming_time;

        const float x = volume_to_length(m_filpar[m_current_tool].ramming_speed[i] * time_step, line_width, m_layer_height);
        const float e = m_filpar[m_current_tool].ramming_speed[i] * time_step / filament_area(); // transform volume per sec to E move;
        const float dist = std::min(x - e_done, remaining);		  // distance to travel for either the next time_step, or to the next turnaround
        const float actual_time = dist/x * time_step;
        writer.ram(writer.x(), writer.x() + (m_left_to_right ? 1.f : -1.f) * dist, 0.f, 0.f, e * (dist / x), dist / (actual_time / 60.f));
        remaining -= dist;

		if (remaining < WT_EPSILON)	{ // we reached a turning point
			writer.travel(writer.x(), writer.y() + y_step, 7200);
			m_left_to_right = !m_left_to_right;
			remaining = xr - xl;
		}
		e_done += dist; // subtract what was actually done
		if (e_done > x - WT_EPSILON) { // current segment finished
			++i;
			e_done = 0;
		}
	}
	Vec2f end_of_ramming(writer.x(),writer.y());
    writer.change_analyzer_line_width(m_perimeter_width);   // so the next lines are not affected by ramming_line_width_multiplier

    // Retraction:
    if(m_enable_filament_ramming)
        writer.append("; Ramming start\n");

    float old_x = writer.x();
    float turning_point = (!m_left_to_right ? xl : xr );
    if (m_enable_filament_ramming && m_semm && (m_cooling_tube_retraction != 0 || m_cooling_tube_length != 0)) {
        writer.append("; Retract(unload)\n");
        float total_retraction_distance = m_cooling_tube_retraction + m_cooling_tube_length/2.f - 15.f; // the 15mm is reserved for the first part after ramming
        writer.suppress_preview()
              .retract(15.f, m_filpar[m_current_tool].unloading_speed_start * 60.f) // feedrate 5000mm/min = 83mm/s
              .retract(0.70f * total_retraction_distance, 1.0f * m_filpar[m_current_tool].unloading_speed * 60.f)
              .retract(0.20f * total_retraction_distance, 0.5f * m_filpar[m_current_tool].unloading_speed * 60.f)
              .retract(0.10f * total_retraction_distance, 0.3f * m_filpar[m_current_tool].unloading_speed * 60.f)
              .resume_preview();
    }

    const int& number_of_cooling_moves = m_filpar[m_current_tool].cooling_moves;
    const bool cooling_will_happen = m_enable_filament_ramming && m_semm && number_of_cooling_moves > 0 && m_cooling_tube_length != 0;
    bool change_temp_later = false;

    // Wipe tower should only change temperature with single extruder MM. Otherwise, all temperatures should
    // be already set and there is no need to change anything. Also, the temperature could be changed
    // for wrong extruder.
    if (m_semm) {
        if (new_temperature != 0 && (new_temperature != m_old_temperature || is_first_layer() || cold_ramming) ) { 	// Set the extruder temperature, but don't wait.
            // If the required temperature is the same as last time, don't emit the M104 again (if user adjusted the value, it would be reset)
            // However, always change temperatures on the first layer (this is to avoid issues with priming lines turned off).
            if (cold_ramming && cooling_will_happen)
                change_temp_later = true;
            else
                writer.set_extruder_temp(new_temperature, false);
            m_old_temperature = new_temperature;
        }
    }

    // Cooling:
    if (cooling_will_happen) {
        writer.append("; Cooling\n");
        const float& initial_speed = m_filpar[m_current_tool].cooling_initial_speed;
        const float& final_speed   = m_filpar[m_current_tool].cooling_final_speed;

        float speed_inc = (final_speed - initial_speed) / (2.f * number_of_cooling_moves - 1.f);

        if (m_is_mk4mmu3)
            writer.disable_linear_advance();

        writer.suppress_preview()
              .travel(writer.x(), writer.y() + y_step);
        old_x = writer.x();
        turning_point = xr-old_x > old_x-xl ? xr : xl;
        float stamping_dist_e = m_filpar[m_current_tool].filament_stamping_distance + m_cooling_tube_length / 2.f;

        for (int i=0; i<number_of_cooling_moves; ++i) {

            // Stamping - happens after every cooling move except for the last one.
            if (i>0 && m_filpar[m_current_tool].filament_stamping_distance != 0) {

                // Stamping turning point shall be no farther than 20mm from the current nozzle position:
                float stamping_turning_point = std::clamp(old_x + 20.f * (turning_point - old_x > 0.f ? 1.f : -1.f), xl, xr);

                // Only last 5mm will be done with the fast x travel. The point is to spread possible blobs
                // along the whole wipe tower.
                if (stamping_dist_e > 5) {
                    float cent = writer.x();
                    writer.load_move_x_advanced(stamping_turning_point, (stamping_dist_e - 5), m_filpar[m_current_tool].filament_stamping_loading_speed, 200);
                    writer.load_move_x_advanced(cent, 5, m_filpar[m_current_tool].filament_stamping_loading_speed, m_travel_speed);
                    writer.travel(cent, writer.y());
                } else
                    writer.load_move_x_advanced_there_and_back(stamping_turning_point, stamping_dist_e, m_filpar[m_current_tool].filament_stamping_loading_speed, m_travel_speed);

                // Retract while the print head is stationary, so if there is a blob, it is not dragged along.
                writer.retract(stamping_dist_e, m_filpar[m_current_tool].unloading_speed * 60.f);
            }

            if (i == number_of_cooling_moves - 1 && change_temp_later) {
                // If cold_ramming, the temperature change should be done before the last cooling move.
                    writer.set_extruder_temp(new_temperature, false);
            }

            float speed = initial_speed + speed_inc * 2*i;
            writer.load_move_x_advanced(turning_point, m_cooling_tube_length, speed);
            speed += speed_inc;
            writer.load_move_x_advanced(old_x, -m_cooling_tube_length, speed);
        }
    }

    if (m_enable_filament_ramming && m_semm) {
        writer.append("; Cooling park\n");
        // let's wait is necessary:
        writer.wait(m_filpar[m_current_tool].delay);
        // we should be at the beginning of the cooling tube again - let's move to parking position:
        const auto _e = -m_cooling_tube_length / 2.f + m_parking_pos_retraction - m_cooling_tube_retraction;
        if (_e != 0.f)
            writer.retract(_e, 2000);
    }

    if(m_enable_filament_ramming)
        writer.append("; Ramming end\n");


    // this is to align ramming and future wiping extrusions, so the future y-steps can be uniform from the start:
    // the perimeter_width will later be subtracted, it is there to not load while moving over just extruded material
    Vec2f pos = Vec2f(end_of_ramming.x(), end_of_ramming.y() + (y_step/m_extra_spacing_ramming-m_perimeter_width) / 2.f + m_perimeter_width);
    if (do_ramming)
        writer.travel(pos, 2400.f);
    else
        writer.set_position(pos);

    writer.resume_preview()
          .flush_planner_queue();
}

// Change the tool, set a speed override for soluble and flex materials.
void WipeTower2::toolchange_Change(
	WipeTowerWriter2 &writer,
    const size_t 	new_tool,
    const std::string&  new_material)
{
    // Ask the writer about how much of the old filament we consumed:
    if (m_current_tool < m_used_filament_length.size())
    	m_used_filament_length[m_current_tool] += writer.get_and_reset_used_filament_length();

    // This is where we want to place the custom gcodes. We will use placeholders for this.
    // These will be substituted by the actual gcodes when the gcode is generated.
    //writer.append("[end_filament_gcode]\n");
    writer.append("[change_filament_gcode]\n");

    if (m_is_mk4mmu3)
        writer.switch_filament_monitoring(true);

    // Travel to where we assume we are. Custom toolchange or some special T code handling (parking extruder etc)
    // gcode could have left the extruder somewhere, we cannot just start extruding. We should also inform the
    // postprocessor that we absolutely want to have this in the gcode, even if it thought it is the same as before.
    Vec2f current_pos = writer.pos_rotated();
    writer.feedrate(m_travel_speed * 60.f) // see https://github.com/prusa3d/PrusaSlicer/issues/5483
          .append(std::string("G1 X") + Slic3r::float_to_string_decimal_point(current_pos.x())
                             +  " Y"  + Slic3r::float_to_string_decimal_point(current_pos.y())
                             + never_skip_tag() + "\n"
    );

    writer.append("[deretraction_from_wipe_tower_generator]");

    // The toolchange Tn command will be inserted later, only in case that the user does
    // not provide a custom toolchange gcode.
	writer.set_tool(new_tool); // This outputs nothing, the writer just needs to know the tool has changed.
    // writer.append("[filament_start_gcode]\n");


	writer.flush_planner_queue();
	m_current_tool = new_tool;
}

void WipeTower2::toolchange_Load(
	WipeTowerWriter2 &writer,
	const WipeTower::box_coordinates  &cleaning_box)
{
    if (m_semm && m_enable_filament_ramming && (m_parking_pos_retraction != 0 || m_extra_loading_move != 0)) {
        float xl = cleaning_box.ld.x() + m_perimeter_width * 0.75f;
        float xr = cleaning_box.rd.x() - m_perimeter_width * 0.75f;
        float oldx = writer.x();	// the nozzle is in place to do the first wiping moves, we will remember the position

        // Load the filament while moving left / right, so the excess material will not create a blob at a single position.
        float turning_point = ( oldx-xl < xr-oldx ? xr : xl );
        float edist = m_parking_pos_retraction+m_extra_loading_move;

        writer.append("; CP TOOLCHANGE LOAD\n")
              .suppress_preview()
              .load(0.2f * edist, 60.f * m_filpar[m_current_tool].loading_speed_start)
              .load_move_x_advanced(turning_point, 0.7f * edist,        m_filpar[m_current_tool].loading_speed)  // Fast phase
              .load_move_x_advanced(oldx,          0.1f * edist, 0.1f * m_filpar[m_current_tool].loading_speed)  // Super slow*/

              .travel(oldx, writer.y()) // in case last move was shortened to limit x feedrate
              .resume_preview();

        // Reset the extruder current to the normal value.
        if (m_set_extruder_trimpot)
            writer.set_extruder_trimpot(550);
    }
}

// Wipe the newly loaded filament until the end of the assigned wipe area.
void WipeTower2::toolchange_Wipe(
	WipeTowerWriter2 &writer,
	const WipeTower::box_coordinates  &cleaning_box,
	float wipe_volume)
{
	// Increase flow on first layer, slow down print.
    writer.set_extrusion_flow(m_extrusion_flow * (is_first_layer() ? 1.18f : 1.f))
		  .append("; CP TOOLCHANGE WIPE\n");
	const float& xl = cleaning_box.ld.x();
	const float& xr = cleaning_box.rd.x();

    writer.set_extrusion_flow(m_extrusion_flow * m_extra_flow);
    const float line_width = m_perimeter_width * m_extra_flow;
    writer.change_analyzer_line_width(line_width);

	// Variables x_to_wipe and traversed_x are here to be able to make sure it always wipes at least
    //   the ordered volume, even if it means violating the box. This can later be removed and simply
    // wipe until the end of the assigned area.

	float x_to_wipe = volume_to_length(wipe_volume, m_perimeter_width, m_layer_height) / m_extra_flow;
	float dy = (is_first_layer() ? m_extra_flow : m_extra_spacing_wipe) * m_perimeter_width; // Don't use the extra spacing for the first layer, but do use the spacing resulting from increased flow.
    // All the calculations in all other places take the spacing into account for all the layers.

	// If spare layers are excluded->if 1 or less toolchange has been done, it must be sill the first layer, too.So slow down.
    const float target_speed = is_first_layer() || (m_num_tool_changes <= 1 && m_no_sparse_layers) ? m_first_layer_speed * 60.f : std::min(m_wipe_tower_max_purge_speed * 60.f, m_infill_speed * 60.f);
    float wipe_speed = 0.33f * target_speed;

    // if there is less than 2.5*line_width to the edge, advance straightaway (there is likely a blob anyway)
    if ((m_left_to_right ? xr-writer.x() : writer.x()-xl) < 2.5f*line_width) {
        writer.travel((m_left_to_right ? xr-line_width : xl+line_width),writer.y()+dy);
        m_left_to_right = !m_left_to_right;
    }
    
    // now the wiping itself:
	for (int i = 0; true; ++i)	{
		if (i!=0) {
            if      (wipe_speed < 0.34f * target_speed) wipe_speed = 0.375f * target_speed;
            else if (wipe_speed < 0.377 * target_speed) wipe_speed = 0.458f * target_speed;
            else if (wipe_speed < 0.46f * target_speed) wipe_speed = 0.875f * target_speed;
            else wipe_speed = std::min(target_speed, wipe_speed + 50.f);
		}

		float traversed_x = writer.x();
		if (m_left_to_right)
            writer.extrude(xr - (i % 4 == 0 ? 0 : 1.5f*line_width), writer.y(), wipe_speed);
		else
            writer.extrude(xl + (i % 4 == 1 ? 0 : 1.5f*line_width), writer.y(), wipe_speed);

        if (writer.y()+float(EPSILON) > cleaning_box.lu.y()-0.5f*line_width)
            break;		// in case next line would not fit

		traversed_x -= writer.x();
        x_to_wipe -= std::abs(traversed_x);
		if (x_to_wipe < WT_EPSILON) {
            writer.travel(m_left_to_right ? xl + 1.5f*line_width : xr - 1.5f*line_width, writer.y(), 7200);
			break;
		}
		// stepping to the next line:
        writer.extrude(writer.x() + (i % 4 == 0 ? -1.f : (i % 4 == 1 ? 1.f : 0.f)) * 1.5f*line_width, writer.y() + dy);
		m_left_to_right = !m_left_to_right;
	}

    // We may be going back to the model - wipe the nozzle. If this is followed
    // by finish_layer, this wipe path will be overwritten.
    writer.add_wipe_point(writer.x(), writer.y())
          .add_wipe_point(writer.x(), writer.y() - dy)
          .add_wipe_point(! m_left_to_right ? m_wipe_tower_width : 0.f, writer.y() - dy);

    if (m_layer_info != m_plan.end() && m_current_tool != m_layer_info->tool_changes.back().new_tool)
        m_left_to_right = !m_left_to_right;

    writer.set_extrusion_flow(m_extrusion_flow); // Reset the extrusion flow.
    writer.change_analyzer_line_width(m_perimeter_width);
}




WipeTower::ToolChangeResult WipeTower2::finish_layer()
{
	assert(! this->layer_finished());
    m_current_layer_finished = true;

    size_t old_tool = m_current_tool;

	WipeTowerWriter2 writer(m_layer_height, m_perimeter_width, m_gcode_flavor, m_filpar, m_enable_arc_fitting);
	writer.set_extrusion_flow(m_extrusion_flow)
		.set_z(m_z_pos)
		.set_initial_tool(m_current_tool)
        .set_y_shift(m_y_shift - (m_current_shape == SHAPE_REVERSED ? m_layer_info->toolchanges_depth() : 0.f));


	// Slow down on the 1st layer.
    // If spare layers are excluded -> if 1 or less toolchange has been done, it must be still the first layer, too. So slow down.
    bool first_layer = is_first_layer() || (m_num_tool_changes <= 1 && m_no_sparse_layers);
    float                      feedrate      = first_layer ? m_first_layer_speed * 60.f : std::min(m_wipe_tower_max_purge_speed * 60.f, m_infill_speed * 60.f);
    float current_depth = m_layer_info->depth - m_layer_info->toolchanges_depth();
    WipeTower::box_coordinates fill_box(Vec2f(m_perimeter_width, m_layer_info->depth-(current_depth-m_perimeter_width)),
                             m_wipe_tower_width - 2 * m_perimeter_width, current_depth-m_perimeter_width);


    writer.set_initial_position((m_left_to_right ? fill_box.ru : fill_box.lu), // so there is never a diagonal travel
                                 m_wipe_tower_width, m_wipe_tower_depth, m_internal_rotation);

    bool toolchanges_on_layer = m_layer_info->toolchanges_depth() > WT_EPSILON;

    // inner perimeter of the sparse section, if there is space for it:
    if (fill_box.ru.y() - fill_box.rd.y() > m_perimeter_width - WT_EPSILON)
        writer.rectangle(fill_box.ld, fill_box.rd.x()-fill_box.ld.x(), fill_box.ru.y()-fill_box.rd.y(), feedrate);

    // we are in one of the corners, travel to ld along the perimeter:
    if (writer.x() > fill_box.ld.x()+EPSILON) writer.travel(fill_box.ld.x(),writer.y());
    if (writer.y() > fill_box.ld.y()+EPSILON) writer.travel(writer.x(),fill_box.ld.y());

    // Extrude infill to support the material to be printed above.
    const float dy = (fill_box.lu.y() - fill_box.ld.y() - m_perimeter_width);
    float left = fill_box.lu.x() + 2*m_perimeter_width;
    float right = fill_box.ru.x() - 2 * m_perimeter_width;
    if (dy > m_perimeter_width)
    {
        writer.travel(fill_box.ld + Vec2f(m_perimeter_width * 2, 0.f))
            .append(";--------------------\n"
                    "; CP EMPTY GRID START\n")
            .comment_with_value(" layer #", m_num_layer_changes + 1);

        // Is there a soluble filament wiped/rammed at the next layer?
        // If so, the infill should not be sparse.
        bool solid_infill = m_layer_info+1 == m_plan.end()
                          ? false
                          : std::any_of((m_layer_info+1)->tool_changes.begin(),
                                        (m_layer_info+1)->tool_changes.end(),
                                            [this](const WipeTowerInfo::ToolChange& tch) {
                                       return m_filpar[tch.new_tool].is_soluble
                                           || m_filpar[tch.old_tool].is_soluble;
                                            });
        solid_infill |= first_layer && m_adhesion;

        if (solid_infill) {
            float sparse_factor = 1.5f; // 1=solid, 2=every other line, etc.
            if (first_layer) { // the infill should touch perimeters
                left  -= m_perimeter_width;
                right += m_perimeter_width;
                sparse_factor = 1.f;
            }
            float y = fill_box.ld.y() + m_perimeter_width;
            int n = dy / (m_perimeter_width * sparse_factor);
            float spacing = (dy-m_perimeter_width)/(n-1);
            int i=0;
            for (i=0; i<n; ++i) {
                writer.extrude(writer.x(), y, feedrate)
                      .extrude(i%2 ? left : right, y);
                y = y + spacing;
            }
            writer.extrude(writer.x(), fill_box.lu.y());
        } else {
            // Extrude an inverse U at the left of the region and the sparse infill.
            writer.extrude(fill_box.lu + Vec2f(m_perimeter_width * 2, 0.f), feedrate);

            const int n = 1+int((right-left)/m_bridging);
            const float dx = (right-left)/n;
            for (int i=1;i<=n;++i) {
                float x=left+dx*i;
                writer.travel(x,writer.y());
                writer.extrude(x,i%2 ? fill_box.rd.y() : fill_box.ru.y());
            }
        }

        writer.append("; CP EMPTY GRID END\n"
                      ";------------------\n\n\n\n\n\n\n");
    }

    const float spacing = m_perimeter_width - m_layer_height*float(1.-M_PI_4);
    feedrate = first_layer ? m_first_layer_speed * 60.f : std::min(m_wipe_tower_max_purge_speed * 60.f, m_perimeter_speed * 60.f);

    Polygon poly;
    if (m_wall_type == (int)wtwCone) {
         WipeTower::box_coordinates wt_box(Vec2f(0.f, (m_current_shape == SHAPE_REVERSED ? m_layer_info->toolchanges_depth() : 0.f)),
                                           m_wipe_tower_width, m_layer_info->depth + m_perimeter_width);
        // outer contour (always)
        bool infill_cone = first_layer && m_wipe_tower_width > 2 * spacing && m_wipe_tower_depth > 2 * spacing;
        poly = generate_support_cone_wall(writer, wt_box, feedrate, infill_cone, spacing);
    } else {
        WipeTower::box_coordinates wt_box(Vec2f(0.f, 0.f), m_wipe_tower_width, m_layer_info->depth + m_perimeter_width);
        poly = generate_support_rib_wall(writer, wt_box, feedrate, first_layer, m_wall_type == (int)wtwRib, true, false);
    }

    // brim (first layer only)
    if (first_layer) {
        writer.append("; WIPE_TOWER_BRIM_START\n");
        size_t loops_num = (m_wipe_tower_brim_width + spacing/2.f) / spacing;
        
        for (size_t i = 0; i < loops_num; ++ i) {
            poly = offset(poly, scale_(spacing)).front();
            int cp = poly.closest_point_index(Point::new_scale(writer.x(), writer.y()));
            writer.travel(unscale(poly.points[cp]).cast<float>());
            for (int i=cp+1; true; ++i ) {
                if (i==int(poly.points.size()))
                    i = 0;
                writer.extrude(unscale(poly.points[i]).cast<float>());
                if (i == cp)
                    break;
            }
        }
        writer.append("; WIPE_TOWER_BRIM_END\n");
        // Save actual brim width to be later passed to the Print object, which will use it
        // for skirt calculation and pass it to GLCanvas for precise preview box
        m_wipe_tower_brim_width_real = loops_num * spacing;
    }

    // Now prepare future wipe.
    int i = poly.closest_point_index(Point::new_scale(writer.x(), writer.y()));
    writer.add_wipe_point(writer.pos());
    writer.add_wipe_point(unscale(poly.points[i==0 ? int(poly.points.size())-1 : i-1]).cast<float>());

    // Ask our writer about how much material was consumed.
    // Skip this in case the layer is sparse and config option to not print sparse layers is enabled.
    if (! m_no_sparse_layers || toolchanges_on_layer || first_layer) {
        if (m_current_tool < m_used_filament_length.size())
            m_used_filament_length[m_current_tool] += writer.get_and_reset_used_filament_length();
        m_current_height += m_layer_info->height;
    }

    return construct_tcr(writer, false, old_tool, true);
}

// Static method to get the radius and x-scaling of the stabilizing cone base.
std::pair<double, double> WipeTower2::get_wipe_tower_cone_base(double width, double height, double depth, double angle_deg)
{
    double R = std::tan(Geometry::deg2rad(angle_deg/2.)) * height;
    double fake_width = 0.66 * width;
    double diag = std::hypot(fake_width / 2., depth / 2.);
    double support_scale = 1.;
    if (R > diag) {
        double w = fake_width;
        double sin = 0.5 * depth / diag;
        double tan = depth / w;
        double t = (R - diag) * sin;
        support_scale = (w / 2. + t / tan + t * tan) / (w / 2.);
    }
    return std::make_pair(R, support_scale);
}

// Static method to extract wipe_volumes[from][to] from the configuration.
std::vector<std::vector<float>> WipeTower2::extract_wipe_volumes(const PrintConfig& config)
{
    // Get wiping matrix to get number of extruders and convert vector<double> to vector<float>:
    std::vector<float> wiping_matrix(cast<float>(config.flush_volumes_matrix.values));
    auto scale = config.flush_multiplier.get_at(0);

    // The values shall only be used when SEMM is enabled. The purging for other printers
    // is determined by filament_minimal_purge_on_wipe_tower.
    if (! config.purge_in_prime_tower.value || ! config.single_extruder_multi_material.value)
        std::fill(wiping_matrix.begin(), wiping_matrix.end(), 0.f);

    // Extract purging volumes for each extruder pair:
    std::vector<std::vector<float>> wipe_volumes;
    const unsigned int number_of_extruders = (unsigned int)(sqrt(wiping_matrix.size())+EPSILON);
    for (size_t i = 0; i<number_of_extruders; ++i)
        wipe_volumes.push_back(std::vector<float>(wiping_matrix.begin()+i*number_of_extruders, wiping_matrix.begin()+(i+1)*number_of_extruders));

    // Also include filament_minimal_purge_on_wipe_tower. This is needed for the preview.
    for (unsigned int i = 0; i<number_of_extruders; ++i)
        for (unsigned int j = 0; j<number_of_extruders; ++j)
            wipe_volumes[i][j] = std::max<float>(wipe_volumes[i][j] * scale, config.filament_minimal_purge_on_wipe_tower.get_at(j));

    return wipe_volumes;
}

static float get_wipe_depth(float volume, float layer_height, float perimeter_width, float extra_flow, float extra_spacing, float width)
{
    float length_to_extrude = (volume_to_length(volume, perimeter_width, layer_height)) / extra_flow;
    length_to_extrude = std::max(length_to_extrude,0.f);

	return (int(length_to_extrude / width) + 1) * perimeter_width * extra_spacing;
}

// Appends a toolchange into m_plan and calculates neccessary depth of the corresponding box
void WipeTower2::plan_toolchange(float z_par, float layer_height_par, unsigned int old_tool,
                                unsigned int new_tool, float wipe_volume)
{
	assert(m_plan.empty() || m_plan.back().z <= z_par + WT_EPSILON);	// refuses to add a layer below the last one

	if (m_plan.empty() || m_plan.back().z + WT_EPSILON < z_par) // if we moved to a new layer, we'll add it to m_plan first
		m_plan.push_back(WipeTowerInfo(z_par, layer_height_par));

    if (m_first_layer_idx == size_t(-1) && (! m_no_sparse_layers || old_tool != new_tool || m_plan.size() == 1))
        m_first_layer_idx = m_plan.size() - 1;

    if (old_tool == new_tool)	// new layer without toolchanges - we are done
        return;

    // this is an actual toolchange - let's calculate depth to reserve on the wipe tower
    float width = m_wipe_tower_width - 3*m_perimeter_width; 
	float length_to_extrude = volume_to_length(0.25f * std::accumulate(m_filpar[old_tool].ramming_speed.begin(), m_filpar[old_tool].ramming_speed.end(), 0.f),
										m_perimeter_width * m_filpar[old_tool].ramming_line_width_multiplicator,
										layer_height_par);
    // Orca: Set ramming depth to 0 if ramming is disabled.
    float ramming_depth = m_enable_filament_ramming ? ((int(length_to_extrude / width) + 1) * (m_perimeter_width * m_filpar[old_tool].ramming_line_width_multiplicator * m_filpar[old_tool].ramming_step_multiplicator) * m_extra_spacing_ramming) : 0;
    float first_wipe_line = - (width*((length_to_extrude / width)-int(length_to_extrude / width)) - width);

    float first_wipe_volume = length_to_volume(first_wipe_line, m_perimeter_width * m_extra_flow, layer_height_par);
    float wiping_depth = get_wipe_depth(wipe_volume - first_wipe_volume, layer_height_par, m_perimeter_width, m_extra_flow, m_extra_spacing_wipe, width);
    
	m_plan.back().tool_changes.push_back(WipeTowerInfo::ToolChange(old_tool, new_tool, ramming_depth + wiping_depth, ramming_depth, first_wipe_line, wipe_volume));
}



void WipeTower2::plan_tower()
{
	// Calculate m_wipe_tower_depth (maximum depth for all the layers) and propagate depths downwards
	m_wipe_tower_depth = 0.f;
	for (auto& layer : m_plan)
		layer.depth = 0.f;
    m_wipe_tower_height = m_plan.empty() ? 0.f : m_plan.back().z;
    m_current_height = 0.f;
	
    for (int layer_index = int(m_plan.size()) - 1; layer_index >= 0; --layer_index)
	{
		float this_layer_depth = std::max(m_plan[layer_index].depth, m_plan[layer_index].toolchanges_depth());
		m_plan[layer_index].depth = this_layer_depth;
		
		if (this_layer_depth > m_wipe_tower_depth - m_perimeter_width)
			m_wipe_tower_depth = this_layer_depth + m_perimeter_width;

		for (int i = layer_index - 1; i >= 0 ; i--)
		{
			if (m_plan[i].depth - this_layer_depth < 2*m_perimeter_width )
				m_plan[i].depth = this_layer_depth;
		}
	}
}

void WipeTower2::save_on_last_wipe()
{
    for (m_layer_info=m_plan.begin();m_layer_info<m_plan.end();++m_layer_info) {
        set_layer(m_layer_info->z, m_layer_info->height, 0, m_layer_info->z == m_plan.front().z, m_layer_info->z == m_plan.back().z);
        if (m_layer_info->tool_changes.size()==0)   // we have no way to save anything on an empty layer
            continue;

        // Which toolchange will finish_layer extrusions be subtracted from?
        int idx = first_toolchange_to_nonsoluble(m_layer_info->tool_changes);

        if (idx == -1) {
            // In this case, finish_layer will be called at the very beginning.
            finish_layer().total_extrusion_length_in_plane();
        }

        for (int i=0; i<int(m_layer_info->tool_changes.size()); ++i) {
            auto& toolchange = m_layer_info->tool_changes[i];
            tool_change(toolchange.new_tool);

            if (i == idx) {
                float width = m_wipe_tower_width - 3*m_perimeter_width; // width we draw into

                float volume_to_save = length_to_volume(finish_layer().total_extrusion_length_in_plane(), m_perimeter_width, m_layer_info->height);
                float volume_left_to_wipe = std::max(m_filpar[toolchange.new_tool].filament_minimal_purge_on_wipe_tower, toolchange.wipe_volume_total - volume_to_save);
                float volume_we_need_depth_for = std::max(0.f, volume_left_to_wipe - length_to_volume(toolchange.first_wipe_line, m_perimeter_width*m_extra_flow, m_layer_info->height));
                float depth_to_wipe = get_wipe_depth(volume_we_need_depth_for, m_layer_info->height, m_perimeter_width, m_extra_flow, m_extra_spacing_wipe, width);

                toolchange.required_depth = toolchange.ramming_depth + depth_to_wipe;
                toolchange.wipe_volume = volume_left_to_wipe;
            }
        }
    }
}


// Return index of first toolchange that switches to non-soluble extruder
// ot -1 if there is no such toolchange.
int WipeTower2::first_toolchange_to_nonsoluble(
        const std::vector<WipeTowerInfo::ToolChange>& tool_changes) const
{
    // Orca: allow calculation of the required depth and wipe volume for soluable toolchanges as well
    // NOTE: it's not clear if this is the right way, technically we should disable wipe tower if soluble filament is used as it
    // will will make the wipe tower unstable. Need to revist this in the future.
    return tool_changes.empty() ? -1 : 0;
    //for (size_t idx=0; idx<tool_changes.size(); ++idx)
    //    if (! m_filpar[tool_changes[idx].new_tool].is_soluble)
    //        return idx;
    //return -1;
}

static WipeTower::ToolChangeResult merge_tcr(WipeTower::ToolChangeResult& first,
                                             WipeTower::ToolChangeResult& second)
{
    assert(first.new_tool == second.initial_tool);
    WipeTower::ToolChangeResult out = first;
    if (first.end_pos != second.start_pos)
        out.gcode += "G1 X" + Slic3r::float_to_string_decimal_point(second.start_pos.x(), 3)
                     + " Y" + Slic3r::float_to_string_decimal_point(second.start_pos.y(), 3)
                     + " F7200\n";
    out.gcode += second.gcode;
    out.extrusions.insert(out.extrusions.end(), second.extrusions.begin(), second.extrusions.end());
    out.end_pos = second.end_pos;
    out.wipe_path = second.wipe_path;
    out.initial_tool = first.initial_tool;
    out.new_tool = second.new_tool;
    return out;
}


// Processes vector m_plan and calls respective functions to generate G-code for the wipe tower
// Resulting ToolChangeResults are appended into vector "result"
void WipeTower2::generate(std::vector<std::vector<WipeTower::ToolChangeResult>> &result)
{
	if (m_plan.empty())
        return;

	plan_tower();
#if 1
    for (int i=0;i<5;++i) {
        save_on_last_wipe();
        plan_tower();
    }
#endif

    m_rib_length = std::max({m_rib_length, sqrt(m_wipe_tower_depth * m_wipe_tower_depth + m_wipe_tower_width * m_wipe_tower_width)});
    m_rib_length += m_extra_rib_length;
    m_rib_length = std::max(0.f, m_rib_length);
    m_rib_width  = std::min(m_rib_width, std::min(m_wipe_tower_depth, m_wipe_tower_width) /
                                             2.f); // Ensure that the rib wall of the wipetower are attached to the infill.

    m_layer_info = m_plan.begin();
    m_current_height = 0.f;

    // we don't know which extruder to start with - we'll set it according to the first toolchange
    for (const auto& layer : m_plan) {
        if (!layer.tool_changes.empty()) {
            m_current_tool = layer.tool_changes.front().old_tool;
            break;
        }
    }

    m_used_filament_length.assign(m_used_filament_length.size(), 0.f); // reset used filament stats
    assert(m_used_filament_length_until_layer.empty());
    m_used_filament_length_until_layer.emplace_back(0.f, m_used_filament_length);

    m_old_temperature = -1; // reset last temperature written in the gcode

	for (const WipeTower2::WipeTowerInfo& layer : m_plan)
	{
        std::vector<WipeTower::ToolChangeResult> layer_result;
        set_layer(layer.z, layer.height, 0, false/*layer.z == m_plan.front().z*/, layer.z == m_plan.back().z);
        m_internal_rotation += 180.f;

        if (m_layer_info->depth < m_wipe_tower_depth - m_perimeter_width)
			m_y_shift = (m_wipe_tower_depth-m_layer_info->depth-m_perimeter_width)/2.f;

        int idx = first_toolchange_to_nonsoluble(layer.tool_changes);
        WipeTower::ToolChangeResult finish_layer_tcr;

        if (idx == -1) {
            // if there is no toolchange switching to non-soluble, finish layer
            // will be called at the very beginning. That's the last possibility
            // where a nonsoluble tool can be.
            finish_layer_tcr = finish_layer();
        }

        for (int i=0; i<int(layer.tool_changes.size()); ++i) {
            layer_result.emplace_back(tool_change(layer.tool_changes[i].new_tool));
            if (i == idx) // finish_layer will be called after this toolchange
                finish_layer_tcr = finish_layer();
        }

        if (layer_result.empty()) {
            // there is nothing to merge finish_layer with
            layer_result.emplace_back(std::move(finish_layer_tcr));
        }
        else {
            if (idx == -1) {
                layer_result[0] = merge_tcr(finish_layer_tcr, layer_result[0]);
                layer_result[0].force_travel = true;
            }
            else
                layer_result[idx] = merge_tcr(layer_result[idx], finish_layer_tcr);
        }

		result.emplace_back(std::move(layer_result));

        if (m_used_filament_length_until_layer.empty() || m_used_filament_length_until_layer.back().first != layer.z)
            m_used_filament_length_until_layer.emplace_back();
        m_used_filament_length_until_layer.back() = std::make_pair(layer.z, m_used_filament_length);
	}
}



std::vector<std::pair<float, float>> WipeTower2::get_z_and_depth_pairs() const
{
    std::vector<std::pair<float, float>> out = {{0.f, m_wipe_tower_depth}};
    for (const WipeTowerInfo& wti : m_plan) {
        assert(wti.depth < wti.depth + WT_EPSILON);
        if (wti.depth < out.back().second - WT_EPSILON)
            out.emplace_back(wti.z, wti.depth);
    }
    if (out.back().first < m_wipe_tower_height - WT_EPSILON)
        out.emplace_back(m_wipe_tower_height, 0.f);
    return out;
}


Polygon WipeTower2::generate_rib_polygon(const WipeTower::box_coordinates& wt_box)
{

    auto get_current_layer_rib_len = [](float cur_height, float max_height, float max_len) -> float {
        return std::abs(max_height - cur_height) / max_height * max_len;
    };
    coord_t diagonal_width = scaled(m_rib_width) / 2;
    float   a = this->m_wipe_tower_width, b = this->m_wipe_tower_depth;
    Line    line_1(Point::new_scale(Vec2f{0, 0}), Point::new_scale(Vec2f{a, b}));
    Line    line_2(Point::new_scale(Vec2f{a, 0}), Point::new_scale(Vec2f{0, b}));
    float   diagonal_extra_length = std::max(0.f, m_rib_length - (float) unscaled(line_1.length())) / 2.f;
    diagonal_extra_length         = scaled(get_current_layer_rib_len(this->m_z_pos, this->m_wipe_tower_height, diagonal_extra_length));
    Point y_shift{0, scaled(this->m_y_shift)};

    line_1.extend(double(diagonal_extra_length));
    line_2.extend(double(diagonal_extra_length));
    line_1.translate(-y_shift);
    line_2.translate(-y_shift);

    Polygon poly_1 = generate_rectange(line_1, diagonal_width);
    Polygon poly_2 = generate_rectange(line_2, diagonal_width);
    Polygon poly;
    poly.points.push_back(Point::new_scale(wt_box.ld));
    poly.points.push_back(Point::new_scale(wt_box.rd));
    poly.points.push_back(Point::new_scale(wt_box.ru));
    poly.points.push_back(Point::new_scale(wt_box.lu));

    Polygons p_1_2 = union_({poly_1, poly_2, poly});
    // Polygon            res_poly = p_1_2.front();
    // for (auto &p : res_poly.points) res.push_back(unscale(p).cast<float>());
    /*if (p_1_2.front().points.size() != 16)
        std::cout << "error " << std::endl;*/
    return p_1_2.front();
};

Polygon WipeTower2::generate_support_rib_wall(WipeTowerWriter2&                 writer,
                                              const WipeTower::box_coordinates& wt_box,
                                             double                 feedrate,
                                             bool                   first_layer,
                                             bool                   rib_wall,
                                             bool                   extrude_perimeter,
                                             bool                   skip_points)
{

    float     retract_length = m_filpar[m_current_tool].retract_length;
    float     retract_speed  = m_filpar[m_current_tool].retract_speed * 60;
    Polygon   wall_polygon   = rib_wall ? generate_rib_polygon(wt_box) : generate_rectange_polygon(wt_box.ld, wt_box.ru);
    Polylines result_wall;
    Polygon   insert_skip_polygon;
    if (m_used_fillet) {
        if (!rib_wall && m_y_shift > EPSILON) // do nothing because the fillet will cause it to be suspended.
        {
        } else {
            wall_polygon           = rib_wall ? rounding_polygon(wall_polygon) : wall_polygon; // rectangle_wall do nothing
            Polygon wt_box_polygon = generate_rectange_polygon(wt_box.ld, wt_box.ru);
            wall_polygon           = union_({wall_polygon, wt_box_polygon}).front();
        }
    }
    if (!extrude_perimeter)
        return wall_polygon;

    if (skip_points) {
        result_wall = contrust_gap_for_skip_points(wall_polygon, std::vector<Vec2f>(), m_wipe_tower_width, 2.5 * m_perimeter_width,
                                                   insert_skip_polygon);
    } else {
        result_wall.push_back(to_polyline(wall_polygon));
        insert_skip_polygon = wall_polygon;
    }
    writer.generate_path(result_wall, feedrate, retract_length, retract_speed, m_used_fillet);
    //if (m_cur_layer_id == 0) {
    //    BoundingBox bbox = get_extents(result_wall);
    //    m_rib_offset     = Vec2f(-unscaled<float>(bbox.min.x()), -unscaled<float>(bbox.min.y()));
    //}

    return insert_skip_polygon;
}


// This block creates the stabilization cone.
// First define a lambda to draw the rectangle with stabilization.
Polygon WipeTower2::generate_support_cone_wall(
    WipeTowerWriter2& writer, const WipeTower::box_coordinates& wt_box, double feedrate, bool infill_cone, float spacing){

    const auto [R, support_scale] = get_wipe_tower_cone_base(m_wipe_tower_width, m_wipe_tower_height, m_wipe_tower_depth,
                                                             m_wipe_tower_cone_angle);

    double z = m_no_sparse_layers ?
                   (m_current_height + m_layer_info->height) :
                   m_layer_info->z; // the former should actually work in both cases, but let's stay on the safe side (the 2.6.0 is close)

    double r      = std::tan(Geometry::deg2rad(m_wipe_tower_cone_angle / 2.f)) * (m_wipe_tower_height - z);
    Vec2f  center = (wt_box.lu + wt_box.rd) / 2.;
    double w      = wt_box.lu.y() - wt_box.ld.y();
    enum Type { Arc, Corner, ArcStart, ArcEnd };

    // First generate vector of annotated point which form the boundary.
    std::vector<std::pair<Vec2f, Type>> pts = {{wt_box.ru, Corner}};
    if (double alpha_start = std::asin((0.5 * w) / r); !std::isnan(alpha_start) && r > 0.5 * w + 0.01) {
        for (double alpha = alpha_start; alpha < M_PI - alpha_start + 0.001; alpha += (M_PI - 2 * alpha_start) / 40.)
            pts.emplace_back(Vec2f(center.x() + r * std::cos(alpha) / support_scale, center.y() + r * std::sin(alpha)),
                             alpha == alpha_start ? ArcStart : Arc);
        pts.back().second = ArcEnd;
    }
    pts.emplace_back(wt_box.lu, Corner);
    pts.emplace_back(wt_box.ld, Corner);
    for (int i = int(pts.size()) - 3; i > 0; --i)
        pts.emplace_back(Vec2f(pts[i].first.x(), 2 * center.y() - pts[i].first.y()), i == int(pts.size()) - 3 ? ArcStart :
                                                                                     i == 1                   ? ArcEnd :
                                                                                                                Arc);
    pts.emplace_back(wt_box.rd, Corner);

    // Create a Polygon from the points.
    Polygon poly;
    for (const auto& [pt, tag] : pts)
        poly.points.push_back(Point::new_scale(pt));

    // Prepare polygons to be filled by infill.
    Polylines polylines;
    if (infill_cone && m_wipe_tower_width > 2 * spacing && m_wipe_tower_depth > 2 * spacing) {
        ExPolygons infill_areas;
        ExPolygon  wt_contour(poly);
        Polygon    wt_rectangle(
            Points{Point::new_scale(wt_box.ld), Point::new_scale(wt_box.rd), Point::new_scale(wt_box.ru), Point::new_scale(wt_box.lu)});
        wt_rectangle = offset(wt_rectangle, scale_(-spacing / 2.)).front();
        wt_contour   = offset_ex(wt_contour, scale_(-spacing / 2.)).front();
        infill_areas = diff_ex(wt_contour, wt_rectangle);
        if (infill_areas.size() == 2) {
            ExPolygon& bottom_expoly = infill_areas.front().contour.points.front().y() < infill_areas.back().contour.points.front().y() ?
                                           infill_areas[0] :
                                           infill_areas[1];
            std::unique_ptr<Fill> filler(Fill::new_from_type(ipMonotonicLine));
            filler->angle   = Geometry::deg2rad(45.f);
            filler->spacing = spacing;
            FillParams params;
            params.density = 1.f;
            Surface surface(stBottom, bottom_expoly);
            filler->bounding_box = get_extents(bottom_expoly);
            polylines            = filler->fill_surface(&surface, params);
            if (!polylines.empty()) {
                if (polylines.front().points.front().x() > polylines.back().points.back().x()) {
                    std::reverse(polylines.begin(), polylines.end());
                    for (Polyline& p : polylines)
                        p.reverse();
                }
            }
        }
    }

    // Find the closest corner and travel to it.
    int    start_i  = 0;
    double min_dist = std::numeric_limits<double>::max();
    for (int i = 0; i < int(pts.size()); ++i) {
        if (pts[i].second == Corner) {
            double dist = (pts[i].first - Vec2f(writer.x(), writer.y())).squaredNorm();
            if (dist < min_dist) {
                min_dist = dist;
                start_i  = i;
            }
        }
    }
    writer.travel(pts[start_i].first);

    // Now actually extrude the boundary (and possibly infill):
    int i = start_i + 1 == int(pts.size()) ? 0 : start_i + 1;
    while (i != start_i) {
        writer.extrude(pts[i].first, feedrate);
        if (pts[i].second == ArcEnd) {
            // Extrude the infill.
            if (!polylines.empty()) {
                // Extrude the infill and travel back to where we were.
                bool mirror = ((pts[i].first.y() - center.y()) * (unscale(polylines.front().points.front()).y() - center.y())) < 0.;
                for (const Polyline& line : polylines) {
                    writer.travel(center - (mirror ? 1.f : -1.f) * (unscale(line.points.front()).cast<float>() - center));
                    for (size_t i = 0; i < line.points.size(); ++i)
                        writer.extrude(center - (mirror ? 1.f : -1.f) * (unscale(line.points[i]).cast<float>() - center));
                }
                writer.travel(pts[i].first);
            }
        }
        if (++i == int(pts.size()))
            i = 0;
    }
    writer.extrude(pts[start_i].first, feedrate);
    return poly;
}
} // namespace Slic3r
