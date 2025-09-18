#include "WipeTower.hpp"

#include <cassert>
#include <iostream>
#include <vector>
#include <numeric>
#include <sstream>
#include <iomanip>

#include "GCodeProcessor.hpp"
#include "BoundingBox.hpp"
#include "ClipperUtils.hpp"
#include "LocalesUtils.hpp"
#include "Triangulation.hpp"


namespace Slic3r
{
static constexpr float  flat_iron_area                 = 4.f;
constexpr float         flat_iron_speed                = 10.f * 60.f;
static const double wipe_tower_wall_infill_overlap = 0.0;
static constexpr double WIPE_TOWER_RESOLUTION = 0.1;
#define WT_SIMPLIFY_TOLERANCE_SCALED (0.001 / SCALING_FACTOR)
static constexpr int    arc_fit_size = 20;
#define SCALED_WIPE_TOWER_RESOLUTION (WIPE_TOWER_RESOLUTION / SCALING_FACTOR)
inline float align_round(float value, float base)
{
    return std::round(value / base) * base;
}

inline float align_ceil(float value, float base)
{
    return std::ceil(value / base) * base;
}

inline float align_floor(float value, float base)
{
    return std::floor((value) / base) * base;
}

static bool is_valid_gcode(const std::string &gcode)
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

static Polygon chamfer_polygon(Polygon &polygon, double chamfer_dis = 2., double angle_tol = 30. / 180. * PI)
{
    if (polygon.points.size() < 3) return polygon;
    Polygon res;
    res.points.reserve(polygon.points.size() * 2);
    int    mod           = polygon.points.size();
    double cos_angle_tol = abs(std::cos(angle_tol));

    for (int i = 0; i < polygon.points.size(); i++) {
        Vec2d  a        = unscaled(polygon.points[(i - 1 + mod) % mod]);
        Vec2d  b        = unscaled(polygon.points[i]);
        Vec2d  c        = unscaled(polygon.points[(i + 1) % mod]);
        double ab_len   = (a - b).norm();
        double bc_len   = (b - c).norm();
        Vec2d  ab       = (b - a) / ab_len;
        Vec2d  bc       = (c - b) / bc_len;
        assert(ab_len != 0);
        assert(bc_len != 0);
        float  cosangle = ab.dot(bc);
        //std::cout << " angle " << acos(cosangle) << " cosangle " << cosangle << std::endl;
        //std::cout << " ab_len " << ab_len << " bc_len " << bc_len << std::endl;
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

Polygon WipeTower::rounding_polygon(Polygon &polygon, double rounding /*= 2.*/, double angle_tol/* = 30. / 180. * PI*/)
{
    if (polygon.points.size() < 3) return polygon;
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
        bool  is_ccw   = cross2(ab, bc) > 0;
        if (abs(cosangle) < cos_angle_tol) {
            float real_rounding_dis = std::min({rounding, ab_len / 2.1, bc_len / 2.1}); // 2.1 to ensure the points do not coincide
            Vec2d left              = b - ab * real_rounding_dis;
            Vec2d right             = b + bc * real_rounding_dis;
            //Point r_left            = scaled(left);
            //Point r_right            = scaled(right);
           // std::cout << " r_left  " << r_left[0] << " " << r_left[1] << std::endl;
            //std::cout << " r_right  " << r_right[0] << " " << r_right[1] << std::endl;
            {
                float half_angle = std::acos(cosangle)/2.f;
                //std::cout << " half_angle  " << cos(half_angle) << std::endl;

                Vec2d dir        = (right - left).normalized();
                dir              = Vec2d{-dir[1], dir[0]};
                dir                  = is_ccw ? dir : -dir;
                double dis       = real_rounding_dis / sin(half_angle);
                //std::cout << " dis  " << dis << std::endl;

                Vec2d  center    = b + dir * dis;
                double radius    = (left - center).norm();
                ArcSegment arc(scaled(center), scaled(radius), scaled(left), scaled(right), is_ccw ? ArcDirection::Arc_Dir_CCW : ArcDirection::Arc_Dir_CW);
                int        n            = arc_fit_size;
                //std::cout << "start  " << arc.start_point[0] << " " << arc.start_point[1] << std::endl;
                //std::cout << "end  " << arc.end_point[0] << " " << arc.end_point[1] << std::endl;
                //std::cout << "start angle   " << arc.polar_start_theta << " end angle " << arc.polar_end_theta << std::endl;
                for (int j = 0; j < n; j++) {
                    float cur_angle = arc.polar_start_theta + (float)j/n * arc.angle_radians ;
                    //std::cout << " cur_angle " << cur_angle << std::endl;
                    if (cur_angle > 2 * PI)
                        cur_angle -= 2 * PI;
                    else if (cur_angle < 0)
                        cur_angle += 2 * PI;
                    Point tmp = arc.center + Point{arc.radius * std::cos(cur_angle), arc.radius *std::sin(cur_angle)};
                    //std::cout << "j = " << j << std::endl;
                    //std::cout << "tmp  = " << tmp[0]<<" "<<tmp[1] << std::endl;
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

static Polygon rounding_rectangle(Polygon &polygon, double rounding = 2., double angle_tol = 30. / 180. * PI) {
    if (polygon.points.size() < 3) return polygon;
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
            //Point r_left            = scaled(left);
            //Point r_right           = scaled(right);
            // std::cout << " r_left  " << r_left[0] << " " << r_left[1] << std::endl;
            // std::cout << " r_right  " << r_right[0] << " " << r_right[1] << std::endl;
            {
                Vec2d      center = b;
                double     radius = real_rounding_dis;
                ArcSegment arc(scaled(center), scaled(radius), scaled(left), scaled(right), is_ccw ? ArcDirection::Arc_Dir_CCW : ArcDirection::Arc_Dir_CW);
                int        n            = arc_fit_size;
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

static std::pair<bool, Vec2f> ray_intersetion_line(const Vec2f &a, const Vec2f &v1, const Vec2f &b, const Vec2f &c)
{
    const Vec2f v2    = c - b;
    double      denom = cross2(v1, v2);
    if (fabs(denom) < EPSILON) return {false, Vec2f(0, 0)};
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
static Polygon scale_polygon(const std::vector<Vec2f> &points) {
    Polygon res;
    for (const auto &p : points) res.points.push_back(scaled(p));
    return res;
}
static std::vector<Vec2f> unscale_polygon(const Polygon& polygon)
{
    std::vector<Vec2f> res;
    for (const auto &p : polygon.points) res.push_back(unscaled<float>(p));
    return res;
}

static Polygon generate_rectange(const Line &line, coord_t offset)
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
    Vec2f start;
    Vec2f end;
    bool  is_arc = false;
    ArcSegment arcsegment;
    Segment(const Vec2f &s, const Vec2f &e) : start(s), end(e) {}
    bool is_valid() const { return start.y() < end.y(); }
};

static std::vector<Segment> remove_points_from_segment(const Segment &segment, const std::vector<Vec2f> &skip_points, double range)
{
    std::vector<Segment> result;
    result.push_back(segment);
    float x = segment.start.x();

    for (const Vec2f &point : skip_points) {
        std::vector<Segment> newResult;
        for (const auto &seg : result) {
            if (point.y() + range <= seg.start.y() || point.y() - range >= seg.end.y()) {
                newResult.push_back(seg);
            } else {
                if (point.y() - range > seg.start.y()) { newResult.push_back(Segment(Vec2f(x, seg.start.y()), Vec2f(x, point.y() - range))); }
                if (point.y() + range < seg.end.y()) { newResult.push_back(Segment(Vec2f(x, point.y() + range), Vec2f(x, seg.end.y()))); }
            }
        }

        result = newResult;
    }

    result.erase(std::remove_if(result.begin(), result.end(), [](const Segment &seg) { return !seg.is_valid(); }), result.end());
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
static IntersectionInfo move_point_along_polygon(const std::vector<Vec2f> &points, const Vec2f &startPoint, int startIdx, float offset, bool forward, int pair_idx)
{
    float            remainingDistance = offset;
    IntersectionInfo res;
    int  mod = points.size();
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

static void insert_points(std::vector<PointWithFlag> &pl, int idx, Vec2f pos, int pair_idx, bool is_forward)
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

static Polylines remove_points_from_polygon(const Polygon &polygon, const std::vector<Vec2f> &skip_points, double range, bool is_left ,Polygon& insert_skip_pg)
{
    assert(polygon.size() > 2);
    Polylines                     result;
    std::vector<PointWithFlag>    new_pl; // add intersection points for gaps, where bool indicates whether it's a gap point.
    std::vector<IntersectionInfo> inter_info;
    Vec2f                         ray = is_left ? Vec2f(-1, 0) : Vec2f(1, 0);
    auto                          polygon_box  = get_extents(polygon);
    Point                         anchor_point = is_left ? Point{polygon_box.max[0], polygon_box.min[1]} : polygon_box.min; // rd:ld
    std::vector<Vec2f>            points;
    {
        points.reserve(polygon.points.size());
        int idx = polygon.closest_point_index(anchor_point);
        Polyline tmp_poly = polygon.split_at_index(idx);
        for (auto &p : tmp_poly) points.push_back(unscale(p).cast<float>());
        points.pop_back();
    }

    for (int i = 0; i < skip_points.size(); i++) {
        for (int j = 0; j < points.size(); j++) {
            Vec2f& p1                   = points[j];
            Vec2f& p2                   = points[(j + 1) % points.size()];
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
    for (const auto &p : points) new_pl.push_back({p, -1});
    std::sort(inter_info.begin(), inter_info.end(), [](const IntersectionInfo &lhs, const IntersectionInfo &rhs) {
        if (rhs.idx == lhs.idx) return lhs.dis_from_idx < rhs.dis_from_idx;
        return lhs.idx < rhs.idx;
    });
    for (int i = inter_info.size() - 1; i >= 0; i--) { insert_points(new_pl, inter_info[i].idx, inter_info[i].pos, inter_info[i].pair_idx, inter_info[i].is_forward); }

    {
        //set insert_pg for wipe_path
        for (auto &p : new_pl) insert_skip_pg.points.push_back(scaled(p.pos));
    }

    int beg = 0;
    bool skip = true;
    int  i    = beg;
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
                if (new_pl[j].pair_idx != -1 && !new_pl[j].is_forward) left = new_pl[j].pair_idx;
                j = (j + 1) % new_pl.size();
            }
            i    = j;
            skip = true;
        }
    } while (i != beg);

    if (!pl.points.empty()) {
        if (new_pl[i].pair_idx==-1) pl.points.push_back(scaled(new_pl[i].pos));
        result.push_back(pl);
    }
    return result;
}

static Polylines contrust_gap_for_skip_points(const Polygon &polygon, const std::vector<Vec2f> & skip_points ,float wt_width,float gap_length,Polygon& insert_skip_polygon)
{
    if (skip_points.empty()) {
        insert_skip_polygon = polygon;
        return Polylines{to_polyline(polygon)};
    }
    bool is_left  = false;
    const auto &pt      = skip_points.front();
    if (abs(pt.x()) < wt_width/2.f) {
        is_left = true;
    }
    return remove_points_from_polygon(polygon, skip_points, gap_length, is_left, insert_skip_polygon);

};

static Polygon generate_rectange_polygon(const Vec2f &wt_box_min ,const Vec2f & wt_box_max) {
    Polygon res;
    res.points.push_back(scaled(wt_box_min));
    res.points.push_back(scaled(Vec2f{wt_box_max[0], wt_box_min[1]}));
    res.points.push_back(scaled(wt_box_max));
    res.points.push_back(scaled(Vec2f{wt_box_min[0], wt_box_max[1]}));
    return res;
}

class WipeTowerWriter
{
public:
	WipeTowerWriter(float layer_height, float line_width, GCodeFlavor flavor, const std::vector<WipeTower::FilamentParameters>& filament_parameters) :
		m_current_pos(std::numeric_limits<float>::max(), std::numeric_limits<float>::max()),
		m_current_z(0.f),
		m_current_feedrate(0.f),
		m_layer_height(layer_height),
		m_extrusion_flow(0.f),
		m_preview_suppressed(false),
		m_elapsed_time(0.f),
#if ENABLE_GCODE_VIEWER_DATA_CHECKING
        m_default_analyzer_line_width(line_width),
#endif // ENABLE_GCODE_VIEWER_DATA_CHECKING
        m_gcode_flavor(flavor),
        m_filpar(filament_parameters)
        {
            // ORCA: This class is only used by BBL printers, so set the parameter appropriately.
            // This fixes an issue where the wipe tower was using BBL tags resulting in statistics for purging in the purge tower not being displayed.
            GCodeProcessor::s_IsBBLPrinter = true;
            // adds tag for analyzer:
            std::ostringstream str;
            str << ";" << GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height) << std::to_string(m_layer_height) << "\n"; // don't rely on GCodeAnalyzer knowing the layer height - it knows nothing at priming
            str << ";" << GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role) << ExtrusionEntity::role_to_string(erWipeTower) << "\n";
            m_gcode += str.str();
            change_analyzer_line_width(line_width);
    }

    WipeTowerWriter& change_analyzer_line_width(float line_width) {
        // adds tag for analyzer:
        std::stringstream str;
        str << ";" << GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Width) << std::to_string(line_width) << "\n";
        m_gcode += str.str();
        return *this;
    }

#if ENABLE_GCODE_VIEWER_DATA_CHECKING
    WipeTowerWriter& change_analyzer_mm3_per_mm(float len, float e) {
        static const float area = float(M_PI) * 1.75f * 1.75f / 4.f;
        float mm3_per_mm = (len == 0.f ? 0.f : area * e / len);
        // adds tag for processor:
        std::stringstream str;
        str << ";" << GCodeProcessor::Mm3_Per_Mm_Tag << mm3_per_mm << "\n";
        m_gcode += str.str();
        return *this;
    }
#endif // ENABLE_GCODE_VIEWER_DATA_CHECKING

	WipeTowerWriter& 			 set_initial_position(const Vec2f &pos, float width = 0.f, float depth = 0.f, float internal_angle = 0.f) {
        m_wipe_tower_width = width;
        m_wipe_tower_depth = depth;
        m_internal_angle = internal_angle;
		m_start_pos = this->rotate(pos);
		m_current_pos = pos;
		return *this;
	}

    WipeTowerWriter&				 set_initial_tool(size_t tool) { m_current_tool = tool; return *this; }

	WipeTowerWriter&				 set_z(float z)
		{ m_current_z = z; return *this; }

	WipeTowerWriter& 			 set_extrusion_flow(float flow)
		{ m_extrusion_flow = flow; return *this; }

	WipeTowerWriter&				 set_y_shift(float shift) {
        m_current_pos.y() -= shift-m_y_shift;
        m_y_shift = shift;
        return (*this);
    }

    WipeTowerWriter&            disable_linear_advance() {
        if (m_gcode_flavor == gcfKlipper)
            m_gcode += "SET_PRESSURE_ADVANCE ADVANCE=0\n";
        else if (m_gcode_flavor == gcfRepRapFirmware)
            m_gcode += std::string("M572 D") + std::to_string(m_current_tool) + " S0\n";
        else
            m_gcode += "M900 K0\n";

        return *this;
    }

	// Suppress / resume G-code preview in Slic3r. Slic3r will have difficulty to differentiate the various
	// filament loading and cooling moves from normal extrusion moves. Therefore the writer
	// is asked to suppres output of some lines, which look like extrusions.
#if ENABLE_GCODE_VIEWER_DATA_CHECKING
    WipeTowerWriter& suppress_preview() { change_analyzer_line_width(0.f); m_preview_suppressed = true; return *this; }
    WipeTowerWriter& resume_preview() { change_analyzer_line_width(m_default_analyzer_line_width); m_preview_suppressed = false; return *this; }
#else
    WipeTowerWriter& 			 suppress_preview() { m_preview_suppressed = true; return *this; }
	WipeTowerWriter& 			 resume_preview()   { m_preview_suppressed = false; return *this; }
#endif // ENABLE_GCODE_VIEWER_DATA_CHECKING

	WipeTowerWriter& 			 feedrate(float f)
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
	WipeTowerWriter& extrude_explicit(float x, float y, float e, float f = 0.f, bool record_length = false, bool limit_volumetric_flow = true)
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

        m_current_pos.x() = x;
        m_current_pos.y() = y;

		// Update the elapsed time with a rough estimate.
        m_elapsed_time += ((len == 0.f) ? std::abs(e) : len) / m_current_feedrate * 60.f;
		m_gcode += "\n";
		return *this;
	}

    	// Extrude with an explicitely provided amount of extrusion.
    WipeTowerWriter &extrude_arc_explicit(ArcSegment &arc, float f = 0.f, bool record_length = false, bool limit_volumetric_flow = true)
    {
        float x   = (float)unscale(arc.end_point).x();
        float y   = (float)unscale(arc.end_point).y();
        float len = unscaled<float>(arc.length);
        float e   = len * m_extrusion_flow;
        if (len < (float) EPSILON && e == 0.f && (f == 0.f || f == m_current_feedrate))
            // Neither extrusion nor a travel move.
            return *this;
        if (record_length) m_used_filament_length += e;

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
            if (m_extrusions.empty() || m_extrusions.back().pos != rotated_current_pos) m_extrusions.emplace_back(WipeTower::Extrusion(rotated_current_pos, 0, m_current_tool));
            {
                int   n            = arc_fit_size;
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
        m_gcode += arc.direction == ArcDirection::Arc_Dir_CCW ? "G3" : "G2";
        const Vec2f center_offset = this->rotate(unscaled<float>(arc.center)) - rotated_current_pos;
        m_gcode += set_format_X(rot.x());
        m_gcode += set_format_Y(rot.y());
        m_gcode += set_format_I(center_offset.x());
        m_gcode += set_format_J(center_offset.y());

        if (e != 0.f) m_gcode += set_format_E(e);

        if (f != 0.f && f != m_current_feedrate) {
            if (limit_volumetric_flow) {
                float e_speed = e / (((len == 0.f) ? std::abs(e) : len) / f * 60.f);
                f /= std::max(1.f, e_speed / m_filpar[m_current_tool].max_e_speed);
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

	WipeTowerWriter& extrude_explicit(const Vec2f &dest, float e, float f = 0.f, bool record_length = false, bool limit_volumetric_flow = true)
		{ return extrude_explicit(dest.x(), dest.y(), e, f, record_length); }

	// Travel to a new XY position. f=0 means use the current value.
	WipeTowerWriter& travel(float x, float y, float f = 0.f)
		{ return extrude_explicit(x, y, 0.f, f); }

	WipeTowerWriter& travel(const Vec2f &dest, float f = 0.f)
		{ return extrude_explicit(dest.x(), dest.y(), 0.f, f); }

	// Extrude a line from current position to x, y with the extrusion amount given by m_extrusion_flow.
	WipeTowerWriter& extrude(float x, float y, float f = 0.f)
	{
		float dx = x - m_current_pos.x();
		float dy = y - m_current_pos.y();
        return extrude_explicit(x, y, std::sqrt(dx*dx+dy*dy) * m_extrusion_flow, f, true);
	}
    WipeTowerWriter &extrude_arc(ArcSegment &arc, float f = 0.f)
    {
        return extrude_arc_explicit(arc, f, true);
    }

	WipeTowerWriter& extrude(const Vec2f &dest, const float f = 0.f)
		{ return extrude(dest.x(), dest.y(), f); }

    WipeTowerWriter& rectangle(const Vec2f& ld,float width,float height,const float f = 0.f)
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

    WipeTowerWriter &rectangle_fill_box(const WipeTower* wipe_tower, const Vec2f &ld, float width, float height, const float f = 0.f)
    {
        bool need_change_flow = wipe_tower->need_thick_bridge_flow(ld.y());

        Vec2f corners[4];
        corners[0]           = ld;
        corners[1]           = ld + Vec2f(width, 0.f);
        corners[2]           = ld + Vec2f(width, height);
        corners[3]           = ld + Vec2f(0.f, height);
        int index_of_closest = 0;
        if (x() - ld.x() > ld.x() + width - x()) // closer to the right
            index_of_closest = 1;
        if (y() - ld.y() > ld.y() + height - y()) // closer to the top
            index_of_closest = (index_of_closest == 0 ? 3 : 2);

        travel(corners[index_of_closest].x(), y()); // travel to the closest corner
        travel(x(), corners[index_of_closest].y());

        int i = index_of_closest;
        bool flow_changed = false;
        do {
            ++i;
            if (i == 4) i = 0;
            if (need_change_flow) {
                if (i == 1) {
                    // using bridge flow in bridge area, and add notes for gcode-check when flow changed
                    set_extrusion_flow(wipe_tower->extrusion_flow(0.2));
                    append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height) + std::to_string(0.2) + "\n");
                    flow_changed = true;
                } else if (i == 2 && flow_changed) {
                    set_extrusion_flow(wipe_tower->get_extrusion_flow());
                    append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height) + std::to_string(m_layer_height) + "\n");
                }
            }
            extrude(corners[i], f);
        } while (i != index_of_closest);
        return (*this);
    }
    WipeTowerWriter &line(const WipeTower *wipe_tower, Vec2f p0, Vec2f p1,const float f = 0.f)
    {
        bool need_change_flow = wipe_tower->need_thick_bridge_flow(p0.y());
        if (need_change_flow) {
            set_extrusion_flow(wipe_tower->extrusion_flow(0.2));
            append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height) + std::to_string(0.2) + "\n");
        }
        if (abs(x() - p0.x()) > abs(x() - p1.x())) std::swap(p0, p1);
        travel(p0.x(), y());
        travel(x(), p0.y());
        extrude(p1, f);
        if (need_change_flow) {
            set_extrusion_flow(wipe_tower->get_extrusion_flow());
            append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height) + std::to_string(m_layer_height) + "\n");
        }
        return (*this);
    }

    WipeTowerWriter &rectangle_fill_box(const WipeTower *wipe_tower, const WipeTower::box_coordinates &fill_box, std::vector<Vec2f> &finish_rect_wipe_path, const float f = 0.f)
    {
        float width  = fill_box.rd.x() - fill_box.ld.x();
        float height = fill_box.ru.y() - fill_box.rd.y();
        if (height > wipe_tower->m_perimeter_width - wipe_tower->WT_EPSILON) {
            rectangle_fill_box(wipe_tower, fill_box.ld, width, height, f);
            Vec2f target = (pos() == fill_box.ld ? fill_box.rd : (pos() == fill_box.rd ? fill_box.ru : (pos() == fill_box.ru ? fill_box.lu : fill_box.ld)));
            finish_rect_wipe_path.emplace_back(pos());
            finish_rect_wipe_path.emplace_back(target);
        } else if (height > wipe_tower->WT_EPSILON) {
            line(wipe_tower, fill_box.ld, fill_box.rd);
            Vec2f target = (pos() == fill_box.ld ? fill_box.rd : fill_box.ld);
            finish_rect_wipe_path.emplace_back(pos());
            finish_rect_wipe_path.emplace_back(target);
        }
        return (*this);
    }
    WipeTowerWriter& rectangle(const WipeTower::box_coordinates& box, const float f = 0.f)
    {
        rectangle(Vec2f(box.ld.x(), box.ld.y()),
                  box.ru.x() - box.lu.x(),
                  box.ru.y() - box.rd.y(), f);
        return (*this);
    }
    WipeTowerWriter &polygon(const Polygon &wall_polygon, const float f = 0.f)
    {
        Polyline    pl = to_polyline(wall_polygon);
        pl.simplify(WT_SIMPLIFY_TOLERANCE_SCALED);
        pl.simplify_by_fitting_arc(SCALED_WIPE_TOWER_RESOLUTION);

        auto get_closet_idx = [this](std::vector<Segment> &corners) -> int {
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

        int index_of_closest = get_closet_idx(segments);
        int i                = index_of_closest;
        travel(segments[i].start); // travel to the closest points
        segments[i].is_arc ? extrude_arc(segments[i].arcsegment, f) : extrude(segments[i].end, f);
        do {
            i = (i + 1) % segments.size();
            if (i == index_of_closest) break;
            segments[i].is_arc ? extrude_arc(segments[i].arcsegment, f) : extrude(segments[i].end, f);
        } while (1);
        return (*this);
    }

	WipeTowerWriter& load(float e, float f = 0.f)
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

	WipeTowerWriter& retract(float e, float f = 0.f)
		{ return load(-e, f); }

// Loads filament while also moving towards given points in x-axis (x feedrate is limited by cutting the distance short if necessary)
    WipeTowerWriter& load_move_x_advanced(float farthest_x, float loading_dist, float loading_speed, float max_x_speed = 50.f)
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

	// Elevate the extruder head above the current print_z position.
	WipeTowerWriter& z_hop(float hop, float f = 0.f)
	{
		m_gcode += std::string("G1") + set_format_Z(m_current_z + hop);
		if (f != 0 && f != m_current_feedrate)
			m_gcode += set_format_F(f);
		m_gcode += "\n";
		return *this;
	}

	// Lower the extruder head back to the current print_z position.
	WipeTowerWriter& z_hop_reset(float f = 0.f)
		{ return z_hop(0, f); }

	// Move to x1, +y_increment,
	// extrude quickly amount e to x2 with feed f.
	WipeTowerWriter& ram(float x1, float x2, float dy, float e0, float e, float f)
	{
		extrude_explicit(x1, m_current_pos.y() + dy, e0, f, true, false);
		extrude_explicit(x2, m_current_pos.y(), e, 0.f, true, false);
		return *this;
	}

	// Let the end of the pulled out filament cool down in the cooling tube
	// by moving up and down and moving the print head left / right
	// at the current Y position to spread the leaking material.
	WipeTowerWriter& cool(float x1, float x2, float e1, float e2, float f)
	{
		extrude_explicit(x1, m_current_pos.y(), e1, f, false, false);
		extrude_explicit(x2, m_current_pos.y(), e2, false, false);
		return *this;
	}

    WipeTowerWriter& set_tool(size_t tool)
	{
		m_current_tool = tool;
		return *this;
	}

	// Set extruder temperature, don't wait by default.
	WipeTowerWriter& set_extruder_temp(int temperature, bool wait = false)
	{
        m_gcode += "M" + std::to_string(wait ? 109 : 104) + " S" + std::to_string(temperature) + "\n";
        return *this;
    }

    // Wait for a period of time (seconds).
	WipeTowerWriter& wait(float time)
	{
        if (time==0.f)
            return *this;
        m_gcode += "G4 S" + Slic3r::float_to_string_decimal_point(time, 3) + "\n";
		return *this;
    }

	// Set speed factor override percentage.
	WipeTowerWriter& speed_override(int speed)
	{
        m_gcode += "M220 S" + std::to_string(speed) + "\n";
		return *this;
    }

	// Let the firmware back up the active speed override value.
	WipeTowerWriter& speed_override_backup()
    {
        // BBS: BBL machine don't support speed backup
#if 0
        if (m_gcode_flavor == gcfMarlinLegacy || m_gcode_flavor == gcfMarlinFirmware)
            m_gcode += "M220 B\n";
#endif
		return *this;
    }

	// Let the firmware restore the active speed override value.
	WipeTowerWriter& speed_override_restore()
	{
	    // BBS: BBL machine don't support speed restore
#if 0
        if (m_gcode_flavor == gcfMarlinLegacy || m_gcode_flavor == gcfMarlinFirmware)
            m_gcode += "M220 R\n";
#endif
		return *this;
    }

	// Set digital trimpot motor
	WipeTowerWriter& set_extruder_trimpot(int current)
	{
        // BBS: don't control trimpot
#if 0
        if (m_gcode_flavor == gcfRepRapSprinter || m_gcode_flavor == gcfRepRapFirmware)
            m_gcode += "M906 E";
        else
            m_gcode += "M907 E";
        m_gcode += std::to_string(current) + "\n";
#endif
		return *this;
    }

	WipeTowerWriter& flush_planner_queue()
	{
		m_gcode += "G4 S0\n";
		return *this;
	}

	// Reset internal extruder counter.
	WipeTowerWriter& reset_extruder()
	{
		m_gcode += "G92 E0\n";
		return *this;
	}

	WipeTowerWriter& comment_with_value(const char *comment, int value)
    {
        m_gcode += std::string(";") + comment + std::to_string(value) + "\n";
		return *this;
    }


    WipeTowerWriter& set_fan(unsigned speed)
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

	WipeTowerWriter& append(const std::string& text) { m_gcode += text; return *this; }

    const std::vector<Vec2f>& wipe_path() const
    {
        return m_wipe_path;
    }

    WipeTowerWriter& add_wipe_point(const Vec2f& pt)
    {
        m_wipe_path.push_back(rotate(pt));
        return *this;
    }

    WipeTowerWriter& add_wipe_point(float x, float y)
    {
        return add_wipe_point(Vec2f(x, y));
    }

    WipeTowerWriter &add_wipe_path(const Polygon & polygon,double wipe_dist)
    {
        int closest_idx = polygon.closest_point_index(scaled(m_current_pos));
        Polyline wipe_path   = polygon.split_at_index(closest_idx);
        wipe_path.reverse();
        for (int i = 0; i < wipe_path.size(); ++i) {
            if (wipe_dist < EPSILON) break;
            add_wipe_point(unscaled<float>(wipe_path[i]));
            if (i != 0) wipe_dist -= (unscaled(wipe_path[i]) - unscaled(wipe_path[i - 1])).norm();
        }
        return *this;
    }
    void generate_path(Polylines &pls, float feedrate, float retract_length, float retract_speed, bool used_fillet)
    {
        auto get_closet_idx = [this](std::vector<Segment> &corners) -> int {
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
        for (auto &pl : pls) pl.simplify_by_fitting_arc(SCALED_WIPE_TOWER_RESOLUTION);

        std::vector<Segment> segments;
        for (const auto &pl : pls) {
            if (pl.points.size()<2) continue;
            for (int i = 0; i < pl.fitting_result.size(); i++) {
                if (pl.fitting_result[i].path_type == EMovePathType::Linear_move) {
                    for (int j = pl.fitting_result[i].start_point_index; j < pl.fitting_result[i].end_point_index; j++)
                        segments.push_back({unscaled<float>(pl.points[j]), unscaled<float>(pl.points[j + 1])});
                } else {
                    int beg = pl.fitting_result[i].start_point_index;
                    int end = pl.fitting_result[i].end_point_index;
                    segments.push_back({unscaled<float>(pl.points[beg]), unscaled<float>(pl.points[end])});
                    segments.back().is_arc = true;
                    segments.back().arcsegment = pl.fitting_result[i].arc_data;
                }

            }
        }
        int index_of_closest = get_closet_idx(segments);
        int i = index_of_closest;
        travel(segments[i].start); // travel to the closest points
        segments[i].is_arc? extrude_arc(segments[i].arcsegment,feedrate) : extrude(segments[i].end, feedrate);
        do {
            i         = (i + 1) % segments.size();
            if (i == index_of_closest) break;
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
    void spiral_flat_ironing(const Vec2f &center, float area, float step_length, float feedrate)
    {
        float edge_length = std::sqrt(area);
        Vec2f box_max     = center + Vec2f{step_length, step_length};
        Vec2f box_min     = center - Vec2f{step_length, step_length};
        int   n           = std::ceil(edge_length / step_length / 2.f);
        assert(n > 0);
        while (n--) {
            travel(box_max.x(), m_current_pos.y(), feedrate);
            travel(m_current_pos.x(), box_max.y(), feedrate);
            travel(box_min.x(), m_current_pos.y(), feedrate);
            travel(m_current_pos.x(), box_min.y(), feedrate);

            box_max += Vec2f{step_length, step_length};
            box_min -= Vec2f{step_length, step_length};
        }
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
#if ENABLE_GCODE_VIEWER_DATA_CHECKING
    const float   m_default_analyzer_line_width;
#endif // ENABLE_GCODE_VIEWER_DATA_CHECKING
    float         m_used_filament_length = 0.f;
    GCodeFlavor   m_gcode_flavor;
    const std::vector<WipeTower::FilamentParameters>& m_filpar;

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
    std::string set_format_I(float i) { return " I" + Slic3r::float_to_string_decimal_point(i, 3); }
    std::string set_format_J(float j) { return " J" + Slic3r::float_to_string_decimal_point(j, 3); }

	WipeTowerWriter& operator=(const WipeTowerWriter &rhs);

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

}; // class WipeTowerWriter



WipeTower::ToolChangeResult WipeTower::construct_tcr(WipeTowerWriter& writer,
                                                     bool priming,
                                                     size_t old_tool,
                                                     bool is_finish,
                                                     bool is_tool_change,
                                                     float purge_volume) const
{
    ToolChangeResult result;
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
    result.nozzle_change_result = m_nozzle_change_result;
    result.is_tool_change       = is_tool_change;
    result.tool_change_start_pos = is_tool_change ? result.start_pos : Vec2f(0, 0);

    // BBS
    result.purge_volume = purge_volume;
    return result;
}

WipeTower::ToolChangeResult WipeTower::construct_block_tcr(WipeTowerWriter &writer, bool priming, size_t filament_id, bool is_finish, float purge_volume) const
{
    ToolChangeResult result;
    result.priming              = priming;
    result.initial_tool         = int(filament_id);
    result.new_tool             = int(filament_id);
    result.print_z              = m_z_pos;
    result.layer_height         = m_layer_height;
    result.elapsed_time         = writer.elapsed_time();
    result.start_pos            = writer.start_pos_rotated();
    result.end_pos              = priming ? writer.pos() : writer.pos_rotated();
    result.gcode                = std::move(writer.gcode());
    result.extrusions           = std::move(writer.extrusions());
    result.wipe_path            = std::move(writer.wipe_path());
    result.is_finish_first      = is_finish;
    result.is_tool_change       = false;
    result.tool_change_start_pos = Vec2f(0, 0);
    // BBS
    result.purge_volume = purge_volume;
    return result;
}

// BBS
const double wrapping_wipe_tower_depth = 10;

// BBS
const std::map<float, float> WipeTower::min_depth_per_height = {
    {5.f,5.f}, {100.f, 20.f}, {250.f, 40.f}, {350.f, 60.f}
};

float WipeTower::get_limit_depth_by_height(float max_height)
{
    float min_wipe_tower_depth = 0.f;
    auto  iter                 = WipeTower::min_depth_per_height.begin();
    while (iter != WipeTower::min_depth_per_height.end()) {
        auto curr_height_to_depth = *iter;

        // This is the case that wipe tower height is lower than the first min_depth_to_height member.
        if (curr_height_to_depth.first >= max_height) {
            min_wipe_tower_depth = curr_height_to_depth.second;
            break;
        }

        iter++;

        // If curr_height_to_depth is the last member, use its min_depth.
        if (iter == WipeTower::min_depth_per_height.end()) {
            min_wipe_tower_depth = curr_height_to_depth.second;
            break;
        }

        // If wipe tower height is between the current and next member, set the min_depth as linear interpolation between them
        auto next_height_to_depth = *iter;
        if (next_height_to_depth.first > max_height) {
            float height_base    = curr_height_to_depth.first;
            float height_diff    = next_height_to_depth.first - curr_height_to_depth.first;
            float min_depth_base = curr_height_to_depth.second;
            float depth_diff     = next_height_to_depth.second - curr_height_to_depth.second;

            min_wipe_tower_depth = min_depth_base + (max_height - curr_height_to_depth.first) / height_diff * depth_diff;
            break;
        }
    }
    return min_wipe_tower_depth;
}

float WipeTower::get_auto_brim_by_height(float max_height) {
    if (max_height < 100) return max_height/100.f * 8.f;
    return 8.f;
}

Vec2f WipeTower::move_box_inside_box(const BoundingBox &box1, const BoundingBox &box2,int scaled_offset)
{
    Vec2f res{0, 0};
    if (box1.size()[0] >= box2.size()[0]- 2*scaled_offset || box1.size()[1] >= box2.size()[1]-2*scaled_offset) return res;

    if (box1.max[0] > box2.max[0] - scaled_offset) {
        res[0] = unscaled<float>((box2.max[0] - scaled_offset) - box1.max[0]);
    }
    else if (box1.min[0] < box2.min[0] + scaled_offset) {
        res[0] = unscaled<float>((box2.min[0] + scaled_offset) - box1.min[0]);
    }

    if (box1.max[1] > box2.max[1] - scaled_offset) {
        res[1] = unscaled<float>((box2.max[1] - scaled_offset) - box1.max[1]);
    }
    else if (box1.min[1] < box2.min[1] + scaled_offset) {
        res[1] = unscaled<float>((box2.min[1] + scaled_offset) - box1.min[1]);
    }
    return res;
}

Polygon WipeTower::rib_section(float width, float depth, float rib_length, float rib_width,bool fillet_wall)
{
    Polygon res;
    res.points.resize(16);
    float              theta     = std::atan(width / depth);
    float              costheta  = std::cos(theta);
    float              sintheta  = std::sin(theta);
    float              w         = rib_width / 2.f;
    float              diag      = std::sqrt(width * width + depth * depth);
    float              l         = (rib_length - diag) / 2;
    Vec2f              diag_dir1 = Vec2f{width, depth}.normalized();
    Vec2f              diag_dir1_perp{-diag_dir1[1], diag_dir1[0]};
    Vec2f              diag_dir2 = Vec2f{-width, depth}.normalized();
    Vec2f              diag_dir2_perp{-diag_dir2[1], diag_dir2[0]};
    std::vector<Vec2f> p{{0, 0}, {width, 0}, {width, depth}, {0, depth}};
    Polyline           p_render;
    for (auto &x : p) p_render.points.push_back(scaled(x));
    res.points[0] = scaled(Vec2f{p[0].x(), p[0].y() + w / sintheta});
    res.points[1] = scaled(Vec2f{p[0] - diag_dir1 * l + diag_dir1_perp * w});
    res.points[2] = scaled(Vec2f{p[0] - diag_dir1 * l - diag_dir1_perp * w});
    res.points[3] = scaled(Vec2f{p[0].x() + w / costheta, p[0].y()});

    res.points[4] = scaled(Vec2f{p[1].x() - w / costheta, p[1].y()});
    res.points[5] = scaled(Vec2f{p[1] - diag_dir2 * l + diag_dir2_perp * w});
    res.points[6] = scaled(Vec2f{p[1] - diag_dir2 * l - diag_dir2_perp * w});
    res.points[7] = scaled(Vec2f{p[1].x(), p[1].y() + w / sintheta});

    res.points[8]  = scaled(Vec2f{p[2].x(), p[2].y() - w / sintheta});
    res.points[9]  = scaled(Vec2f{p[2] + diag_dir1 * l - diag_dir1_perp * w});
    res.points[10] = scaled(Vec2f{p[2] + diag_dir1 * l + diag_dir1_perp * w});
    res.points[11] = scaled(Vec2f{p[2].x() - w / costheta, p[2].y()});

    res.points[12] = scaled(Vec2f{p[3].x() + w / costheta, p[3].y()});
    res.points[13] = scaled(Vec2f{p[3] + diag_dir2 * l - diag_dir2_perp * w});
    res.points[14] = scaled(Vec2f{p[3] + diag_dir2 * l + diag_dir2_perp * w});
    res.points[15] = scaled(Vec2f{p[3].x(), p[3].y() - w / sintheta});
    res.remove_duplicate_points();
    if (fillet_wall) { res = rounding_polygon(res); }
    res.points.shrink_to_fit();
    return res;
}

TriangleMesh WipeTower::its_make_rib_tower(float width, float depth, float height, float rib_length, float rib_width, bool fillet_wall)
{
    TriangleMesh res;
    Polygon      bottom = rib_section(width, depth, rib_length, rib_width, fillet_wall);
    Polygon      top    = rib_section(width, depth, std::sqrt(width * width + depth * depth), rib_width, fillet_wall);
    if (fillet_wall)
        assert(bottom.points.size() == top.points.size());
    int     offset       = bottom.points.size();
    res.its.vertices.reserve(offset * 2);
    if (bottom.area() < scaled(EPSILON) || top.area() < scaled(EPSILON) || bottom.points.size() != top.points.size()) return res;
    auto    faces_bottom = Triangulation::triangulate(bottom);
    auto    faces_top    = Triangulation::triangulate(top);
    res.its.indices.reserve(offset * 2 + faces_bottom.size() + faces_top.size());
    for (auto &t : faces_bottom) res.its.indices.push_back({t[1], t[0], t[2]});
    for (auto &t : faces_top) res.its.indices.push_back({t[0] + offset, t[1] + offset, t[2] + offset});

    for (int i = 0; i < bottom.size(); i++) res.its.vertices.push_back({unscaled<float>(bottom[i][0]), unscaled<float>(bottom[i][1]), 0});
    for (int i = 0; i < top.size(); i++) res.its.vertices.push_back({unscaled<float>(top[i][0]), unscaled<float>(top[i][1]), height});

    for (int i = 0; i < offset; i++) {
        int a = i;
        int b = (i + 1) % offset;
        int c = i + offset;
        int d = b + offset;
        res.its.indices.push_back({a, b, c});
        res.its.indices.push_back({d, c, b});
    }
    return res;
}

TriangleMesh WipeTower::its_make_rib_brim(const Polygon& brim, float layer_height) {
    TriangleMesh res;
    if (brim.area() < scaled(EPSILON))return res;
    int          offset = brim.size();
    res.its.vertices.reserve(brim.size() * 2);
    auto    faces= Triangulation::triangulate(brim);
    res.its.indices.reserve(brim.size() * 2  + 2 * faces.size());
    for (auto &t : faces) res.its.indices.push_back({t[1], t[0], t[2]});
    for (auto &t : faces) res.its.indices.push_back({t[0] + offset, t[1] + offset, t[2] + offset});

    for (int i = 0; i < brim.size(); i++) res.its.vertices.push_back({unscaled<float>(brim[i][0]), unscaled<float>(brim[i][1]), 0});
    for (int i = 0; i < brim.size(); i++) res.its.vertices.push_back({unscaled<float>(brim[i][0]), unscaled<float>(brim[i][1]), layer_height});

    for (int i = 0; i < offset; i++) {
        int a = i;
        int b = (i + 1) % offset;
        int c = i + offset;
        int d = b + offset;
        res.its.indices.push_back({a, b, c});
        res.its.indices.push_back({d, c, b});
    }
    return res;
}


WipeTower::WipeTower(const PrintConfig& config, int plate_idx, Vec3d plate_origin, size_t initial_tool, const float wipe_tower_height, const std::vector<unsigned int>& slice_used_filaments) :
    m_semm(config.single_extruder_multi_material.value),
    m_wipe_tower_pos(config.wipe_tower_x.get_at(plate_idx), config.wipe_tower_y.get_at(plate_idx)),
    m_wipe_tower_width(float(config.prime_tower_width)),
    // BBS
    m_wipe_tower_height(wipe_tower_height),
    m_wipe_tower_rotation_angle(float(config.wipe_tower_rotation_angle)),
    m_wipe_tower_brim_width(float(config.prime_tower_brim_width)),
    m_y_shift(0.f),
    m_z_pos(0.f),
    //m_bridging(float(config.wipe_tower_bridging)),
    m_bridging(10.f),
    m_no_sparse_layers(config.wipe_tower_no_sparse_layers),
    m_gcode_flavor(config.gcode_flavor),
    m_travel_speed(config.travel_speed),
    m_current_tool(initial_tool),
    //wipe_volumes(flush_matrix)
    m_enable_timelapse_print(config.timelapse_type.value == TimelapseType::tlSmooth),
    m_enable_wrapping_detection(config.enable_wrapping_detection),
    m_wrapping_detection_layers(config.wrapping_detection_layers.value && (config.wrapping_exclude_area.values.size() > 2)),
    m_slice_used_filaments(slice_used_filaments.size()),
    m_filaments_change_length(config.filament_change_length.values),
    m_is_multi_extruder(config.nozzle_diameter.size() > 1),
    m_use_gap_wall(config.prime_tower_skip_points.value),
    m_use_rib_wall(config.wipe_tower_wall_type.value == WipeTowerWallType::wtwRib),
    m_extra_rib_length((float)config.wipe_tower_extra_rib_length.value),
    m_rib_width((float)config.wipe_tower_rib_width.value),
    m_used_fillet(config.wipe_tower_fillet_wall.value),
    m_extra_spacing((float)config.prime_tower_infill_gap.value/100.f),
    m_tower_framework(config.prime_tower_enable_framework.value),
    m_flat_ironing(config.prime_tower_flat_ironing.value)
{
    m_flat_ironing = (m_flat_ironing && m_use_gap_wall);
    // Read absolute value of first layer speed, if given as percentage,
    // it is taken over following default. Speeds from config are not
    // easily accessible here.
    const float default_speed = 60.f;
    m_first_layer_speed = config.get_abs_value("initial_layer_speed");
    if (m_first_layer_speed == 0.f) // just to make sure autospeed doesn't break it.
        m_first_layer_speed = default_speed / 2.f;

    // If this is a single extruder MM printer, we will use all the SE-specific config values.
    // Otherwise, the defaults will be used to turn off the SE stuff.
    // BBS: remove useless config
#if 0
    if (m_semm) {
        m_cooling_tube_retraction = float(config.cooling_tube_retraction);
        m_cooling_tube_length     = float(config.cooling_tube_length);
        m_parking_pos_retraction  = float(config.parking_pos_retraction);
        m_extra_loading_move      = float(config.extra_loading_move);
        m_set_extruder_trimpot    = config.high_current_on_filament_swap;
    }
#endif
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



void WipeTower::set_extruder(size_t idx, const PrintConfig& config)
{
    //while (m_filpar.size() < idx+1)   // makes sure the required element is in the vector
    m_filpar.push_back(FilamentParameters());

    m_filpar[idx].material = config.filament_type.get_at(idx);
    m_filpar[idx].is_soluble = config.filament_soluble.get_at(idx);
    // BBS
    m_filpar[idx].is_support = config.filament_is_support.get_at(idx);
    m_filpar[idx].nozzle_temperature = config.nozzle_temperature.get_at(idx);
    m_filpar[idx].nozzle_temperature_initial_layer = config.nozzle_temperature_initial_layer.get_at(idx);
    m_filpar[idx].category = config.filament_adhesiveness_category.get_at(idx);

    // If this is a single extruder MM printer, we will use all the SE-specific config values.
    // Otherwise, the defaults will be used to turn off the SE stuff.
    // BBS: remove useless config
#if 0
    if (m_semm) {
        m_filpar[idx].loading_speed           = float(config.filament_loading_speed.get_at(idx));
        m_filpar[idx].loading_speed_start     = float(config.filament_loading_speed_start.get_at(idx));
        m_filpar[idx].unloading_speed         = float(config.filament_unloading_speed.get_at(idx));
        m_filpar[idx].unloading_speed_start   = float(config.filament_unloading_speed_start.get_at(idx));
        m_filpar[idx].delay                   = float(config.filament_toolchange_delay.get_at(idx));
        m_filpar[idx].cooling_moves           = config.filament_cooling_moves.get_at(idx);
        m_filpar[idx].cooling_initial_speed   = float(config.filament_cooling_initial_speed.get_at(idx));
        m_filpar[idx].cooling_final_speed     = float(config.filament_cooling_final_speed.get_at(idx));
    }
#endif

    m_filpar[idx].filament_area = float((M_PI/4.f) * pow(config.filament_diameter.get_at(idx), 2)); // all extruders are assumed to have the same filament diameter at this point
    float nozzle_diameter = float(config.nozzle_diameter.get_at(idx));
    m_filpar[idx].nozzle_diameter = nozzle_diameter; // to be used in future with (non-single) multiextruder MM

    float max_vol_speed = float(config.filament_max_volumetric_speed.get_at(idx));
    if (max_vol_speed!= 0.f)
        m_filpar[idx].max_e_speed = (max_vol_speed / filament_area());

    m_perimeter_width = nozzle_diameter * Width_To_Nozzle_Ratio; // all extruders are now assumed to have the same diameter
    m_nozzle_change_perimeter_width = 2*m_perimeter_width;
    // BBS: remove useless config
#if 0
    if (m_semm) {
        std::istringstream stream{config.filament_ramming_parameters.get_at(idx)};
        float speed = 0.f;
        stream >> m_filpar[idx].ramming_line_width_multiplicator >> m_filpar[idx].ramming_step_multiplicator;
        m_filpar[idx].ramming_line_width_multiplicator /= 100;
        m_filpar[idx].ramming_step_multiplicator /= 100;
        while (stream >> speed)
            m_filpar[idx].ramming_speed.push_back(speed);
    }
#endif

    m_used_filament_length.resize(std::max(m_used_filament_length.size(), idx + 1)); // makes sure that the vector is big enough so we don't have to check later

    m_filpar[idx].retract_length = config.retraction_length.get_at(idx);
    m_filpar[idx].retract_speed  = config.retraction_speed.get_at(idx);
    m_filpar[idx].wipe_dist      = config.wipe_distance.get_at(idx);
}



// Returns gcode to prime the nozzles at the front edge of the print bed.
std::vector<WipeTower::ToolChangeResult> WipeTower::prime(
	// print_z of the first layer.
	float 						initial_layer_print_height,
	// Extruder indices, in the order to be primed. The last extruder will later print the wipe tower brim, print brim and the object.
	const std::vector<unsigned int> &tools,
	// If true, the last priming are will be the same as the other priming areas, and the rest of the wipe will be performed inside the wipe tower.
	// If false, the last priming are will be large enough to wipe the last extruder sufficiently.
    bool 						/*last_wipe_inside_wipe_tower*/)
{
    return std::vector<ToolChangeResult>();
}

Vec2f WipeTower::get_next_pos(const WipeTower::box_coordinates &cleaning_box, float wipe_length)
{
    const float &xl = cleaning_box.ld.x();
    const float &xr = cleaning_box.rd.x();
    int line_count = wipe_length / (xr - xl);

    float dy = m_layer_info->extra_spacing * m_perimeter_width;
    float y_offset = float(line_count) * dy;
    const Vec2f pos_offset = Vec2f(0.f, m_depth_traversed);

    Vec2f res;
    int   index = m_cur_layer_id % 4;
    //Vec2f offset = m_use_gap_wall ? Vec2f(5 * m_perimeter_width, 0) : Vec2f{0, 0};
    Vec2f offset = Vec2f{0, 0};
    switch (index % 4) {
    case 0:
        res = offset +cleaning_box.ld + pos_offset;
        break;
    case 1:
        res = -offset +cleaning_box.rd + pos_offset + Vec2f(0, y_offset);
        break;
    case 2:
        res = -offset+ cleaning_box.rd + pos_offset;
        break;
    case 3:
        res = offset+cleaning_box.ld + pos_offset + Vec2f(0, y_offset);
        break;
    default: break;
    }
    return res;
}

WipeTower::ToolChangeResult WipeTower::tool_change(size_t tool, bool extrude_perimeter, bool first_toolchange_to_nonsoluble)
{
    m_nozzle_change_result.gcode.clear();
    if (!m_filament_map.empty() && tool < m_filament_map.size() && m_filament_map[m_current_tool] != m_filament_map[tool]) {
        m_nozzle_change_result = nozzle_change(m_current_tool, tool);
    }

    size_t old_tool = m_current_tool;

    float wipe_depth = 0.f;
	float wipe_length = 0.f;
    float purge_volume = 0.f;
    float nozzle_change_depth = 0.f;
	// Finds this toolchange info
	if (tool != (unsigned int)(-1))
	{
		for (const auto &b : m_layer_info->tool_changes)
			if ( b.new_tool == tool ) {
                wipe_length = b.wipe_length;
                wipe_depth = b.required_depth;
                purge_volume = b.purge_volume;
                nozzle_change_depth = b.nozzle_change_depth;
				break;
			}
	}
	else {
		// Otherwise we are going to Unload only. And m_layer_info would be invalid.
	}

    box_coordinates cleaning_box(
		Vec2f(m_perimeter_width, m_perimeter_width),
		m_wipe_tower_width - 2 * m_perimeter_width,
        (tool != (unsigned int)(-1) ? wipe_depth + m_depth_traversed - m_perimeter_width
                                    : m_wipe_tower_depth - m_perimeter_width));

	WipeTowerWriter writer(m_layer_height, m_perimeter_width, m_gcode_flavor, m_filpar);
	writer.set_extrusion_flow(m_extrusion_flow)
		.set_z(m_z_pos)
		.set_initial_tool(m_current_tool)
        .set_y_shift(m_y_shift + (tool!=(unsigned int)(-1) && (m_current_shape == SHAPE_REVERSED) ? m_layer_info->depth - m_layer_info->toolchanges_depth(): 0.f))
		.append(";--------------------\n"
				"; CP TOOLCHANGE START\n")
		.comment_with_value(" toolchange #", m_num_tool_changes + 1); // the number is zero-based


    if (tool != (unsigned)(-1))
        writer.append(std::string("; material : " + (m_current_tool < m_filpar.size() ? m_filpar[m_current_tool].material : "(NONE)") + " -> " + m_filpar[tool].material + "\n").c_str())
              .append(";--------------------\n");

    writer.speed_override_backup();
	writer.speed_override(100);

    float feedrate = is_first_layer() ? std::min(m_first_layer_speed * 60.f, 5400.f) : std::min(60.0f * m_filpar[m_current_tool].max_e_speed / m_extrusion_flow, 5400.f);

    // Increase the extruder driver current to allow fast ramming.
    //BBS
	//if (m_set_extruder_trimpot)
	//	writer.set_extruder_trimpot(750);

    // Ram the hot material out of the melt zone, retract the filament into the cooling tubes and let it cool.
    if (tool != (unsigned int)-1){ 			// This is not the last change.
        writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_Start) + "\n");
        toolchange_Unload(writer, cleaning_box, m_filpar[m_current_tool].material,
                          is_first_layer() ? m_filpar[tool].nozzle_temperature_initial_layer : m_filpar[tool].nozzle_temperature);
        toolchange_Change(writer, tool, m_filpar[tool].material); // Change the tool, set a speed override for soluble and flex materials.
        toolchange_Load(writer, cleaning_box);
        // BBS
        //writer.travel(writer.x(), writer.y()-m_perimeter_width); // cooling and loading were done a bit down the road

        if (m_is_multi_extruder && is_tpu_filament(tool)) {
            float dy                  = 2 * m_perimeter_width;
            float nozzle_change_speed = 60.0f * m_filpar[tool].max_e_speed / m_extrusion_flow;
            nozzle_change_speed *= 0.25;

            const float &xl = cleaning_box.ld.x();
            const float &xr = cleaning_box.rd.x();

            Vec2f start_pos = m_nozzle_change_result.start_pos + Vec2f(0, m_perimeter_width);
            bool   left_to_right     = true;
            double tpu_travel_length = 5;
            double e_flow            = extrusion_flow(m_layer_height);
            double length            = tpu_travel_length / e_flow;
            int    tpu_line_count    = length / (m_wipe_tower_width - 2 * m_perimeter_width) + 1;

            writer.travel(start_pos);

            for (int i = 0; true; ++i) {
                if (left_to_right)
                    writer.travel(xr - m_perimeter_width, writer.y(), nozzle_change_speed);
                else
                    writer.travel(xl + m_perimeter_width, writer.y(), nozzle_change_speed);

                if (i == tpu_line_count - 1)
                    break;

                writer.travel(writer.x(), writer.y() + dy);
                left_to_right = !left_to_right;
            }
        }

        Vec2f initial_position = get_next_pos(cleaning_box, wipe_length);
        writer.set_initial_position(initial_position, m_wipe_tower_width, m_wipe_tower_depth, m_internal_rotation);

        if (extrude_perimeter) {
            box_coordinates wt_box(Vec2f(0.f, (m_current_shape == SHAPE_REVERSED) ? m_layer_info->toolchanges_depth() - m_layer_info->depth : 0.f), m_wipe_tower_width,
                                   m_layer_info->depth + m_perimeter_width);

            // align the perimeter
            Vec2f pos = initial_position;
            switch (m_cur_layer_id % 4){
            case 0:
                pos = wt_box.ld;
                break;
            case 1:
                pos = wt_box.rd;
                break;
            case 2:
                pos = wt_box.ru;
                break;
            case 3:
                pos = wt_box.lu;
                break;
            default: break;
            }
            writer.set_initial_position(pos, m_wipe_tower_width, m_wipe_tower_depth, m_internal_rotation);

            wt_box = align_perimeter(wt_box);
            writer.rectangle(wt_box, feedrate);
        }

        writer.travel(initial_position);

        toolchange_Wipe(writer, cleaning_box, wipe_length);     // Wipe the newly loaded filament until the end of the assigned wipe area.

        writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_End) + "\n");
        ++ m_num_tool_changes;
    } else
        toolchange_Unload(writer, cleaning_box, m_filpar[m_current_tool].material, m_filpar[m_current_tool].nozzle_temperature);

    m_depth_traversed += (wipe_depth - nozzle_change_depth);

    //BBS
	//if (m_set_extruder_trimpot)
	//	writer.set_extruder_trimpot(550);    // Reset the extruder current to a normal value.
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

    return construct_tcr(writer, false, old_tool, false, true, purge_volume);
}

WipeTower::NozzleChangeResult WipeTower::nozzle_change(int old_filament_id, int new_filament_id)
{
    float wipe_depth               = 0.f;
    float wipe_length              = 0.f;
    float purge_volume             = 0.f;
    int   nozzle_change_line_count = 0;

    // Finds this toolchange info
    if (new_filament_id != (unsigned int) (-1)) {
        for (const auto &b : m_layer_info->tool_changes)
            if (b.new_tool == new_filament_id) {
                wipe_length              = b.wipe_length;
                wipe_depth               = b.required_depth;
                purge_volume             = b.purge_volume;
                if (has_tpu_filament())
                    nozzle_change_line_count = ((b.nozzle_change_depth + WT_EPSILON) / m_nozzle_change_perimeter_width) / 2;
                else
                    nozzle_change_line_count = (b.nozzle_change_depth + WT_EPSILON) / m_nozzle_change_perimeter_width;
                break;
            }
    } else {
        // Otherwise we are going to Unload only. And m_layer_info would be invalid.
    }

    float nozzle_change_speed = 60.0f * m_filpar[m_current_tool].max_e_speed / m_extrusion_flow;
    if (is_tpu_filament(m_current_tool)) {
        nozzle_change_speed *= 0.25;
    }

    WipeTowerWriter writer(m_layer_height, m_perimeter_width, m_gcode_flavor, m_filpar);
    writer.set_extrusion_flow(m_extrusion_flow)
        .set_z(m_z_pos)
        .set_initial_tool(m_current_tool)
        .set_extrusion_flow(m_extrusion_flow)
        .set_y_shift(m_y_shift + (new_filament_id != (unsigned int) (-1) && (m_current_shape == SHAPE_REVERSED) ? m_layer_info->depth - m_layer_info->toolchanges_depth() : 0.f))
        .append("; Nozzle change start\n");

    box_coordinates cleaning_box(Vec2f(m_perimeter_width, m_perimeter_width), m_wipe_tower_width - 2 * m_perimeter_width,
                                 (new_filament_id != (unsigned int) (-1) ? wipe_depth + m_depth_traversed - m_perimeter_width : m_wipe_tower_depth - m_perimeter_width));

    Vec2f initial_position = cleaning_box.ld + Vec2f(0.f, m_depth_traversed);
    writer.set_initial_position(initial_position, m_wipe_tower_width, m_wipe_tower_depth, m_internal_rotation);

    const float &xl = cleaning_box.ld.x();
    const float &xr = cleaning_box.rd.x();

    float dy = m_layer_info->extra_spacing * m_perimeter_width;
    if (has_tpu_filament())
        dy = 2 * m_perimeter_width;

    float start_y = writer.y();

    m_left_to_right = true;

    bool need_change_flow = false;
    // now the wiping itself:
    for (int i = 0; true; ++i) {
        if (m_left_to_right)
            writer.extrude(xr + wipe_tower_wall_infill_overlap * m_perimeter_width, writer.y(), nozzle_change_speed);
        else
            writer.extrude(xl - wipe_tower_wall_infill_overlap * m_perimeter_width, writer.y(), nozzle_change_speed);

        if (writer.y() - float(EPSILON) > cleaning_box.lu.y())
            break; // in case next line would not fit

        if (i == nozzle_change_line_count - 1)
            break;

        // stepping to the next line:
        writer.extrude(writer.x(), writer.y() + dy);
        m_left_to_right = !m_left_to_right;
    }

    writer.set_extrusion_flow(m_extrusion_flow); // Reset the extrusion flow.

    m_depth_traversed += nozzle_change_line_count * dy;

    NozzleChangeResult result;

    if (is_tpu_filament(m_current_tool))
    {
        bool left_to_right = !m_left_to_right;
        double tpu_travel_length        = 5;
        double e_flow                   = extrusion_flow(m_layer_height);
        double length                   = tpu_travel_length / e_flow;
        int    tpu_line_count = length / (m_wipe_tower_width - 2 * m_perimeter_width) + 1;

        writer.travel(writer.x(), writer.y() - m_perimeter_width);

        for (int i = 0; true; ++i) {
            if (left_to_right)
                writer.travel(xr - m_perimeter_width, writer.y(), nozzle_change_speed);
            else
                writer.travel(xl + m_perimeter_width, writer.y(), nozzle_change_speed);

            if (i == tpu_line_count - 1)
                break;

            writer.travel(writer.x(), writer.y() - dy);
            left_to_right = !left_to_right;
        }
    }
    else {
        result.wipe_path.push_back(writer.pos());
        if (m_left_to_right) {
             result.wipe_path.push_back(Vec2f(0, writer.y()));
        } else {
             result.wipe_path.push_back(Vec2f(m_wipe_tower_width, writer.y()));
        }
    }

    writer.append("; Nozzle change end\n");

    result.start_pos = writer.start_pos_rotated();
    result.end_pos   = writer.pos();
    result.gcode     = std::move(writer.gcode());
    return result;
}

// Ram the hot material out of the melt zone, retract the filament into the cooling tubes and let it cool.
void WipeTower::toolchange_Unload(
	WipeTowerWriter &writer,
	const box_coordinates 	&cleaning_box,
	const std::string&		 current_material,
	const int 				 new_temperature)
{
    // BBS: toolchange unload is done in change_filament_gcode
#if 0
	float xl = cleaning_box.ld.x() + 1.f * m_perimeter_width;
	float xr = cleaning_box.rd.x() - 1.f * m_perimeter_width;

	const float line_width = m_perimeter_width * m_filpar[m_current_tool].ramming_line_width_multiplicator;       // desired ramming line thickness
	const float y_step = line_width * m_filpar[m_current_tool].ramming_step_multiplicator * m_extra_spacing; // spacing between lines in mm

    writer.append("; CP TOOLCHANGE UNLOAD\n")
        .change_analyzer_line_width(line_width);

	unsigned i = 0;										// iterates through ramming_speed
	m_left_to_right = true;								// current direction of ramming
	float remaining = xr - xl ;							// keeps track of distance to the next turnaround
	float e_done = 0;									// measures E move done from each segment

	writer.travel(xl, cleaning_box.ld.y() + m_depth_traversed + y_step/2.f ); // move to starting position

    // if the ending point of the ram would end up in mid air, align it with the end of the wipe tower:
    if (m_layer_info > m_plan.begin() && m_layer_info < m_plan.end() && (m_layer_info-1!=m_plan.begin() || !m_adhesion )) {

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
                ramming_end_y -= (y_step/m_extra_spacing-m_perimeter_width) / 2.f;   // center of final ramming line

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

    writer.disable_linear_advance();

    // now the ramming itself:
    while (i < m_filpar[m_current_tool].ramming_speed.size())
    {
        const float x = volume_to_length(m_filpar[m_current_tool].ramming_speed[i] * 0.25f, line_width, m_layer_height);
        const float e = m_filpar[m_current_tool].ramming_speed[i] * 0.25f / filament_area(); // transform volume per sec to E move;
        const float dist = std::min(x - e_done, remaining);		  // distance to travel for either the next 0.25s, or to the next turnaround
        const float actual_time = dist/x * 0.25f;
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
    float old_x = writer.x();
    float turning_point = (!m_left_to_right ? xl : xr );
    if (m_semm && (m_cooling_tube_retraction != 0 || m_cooling_tube_length != 0)) {
        float total_retraction_distance = m_cooling_tube_retraction + m_cooling_tube_length/2.f - 15.f; // the 15mm is reserved for the first part after ramming
        writer.suppress_preview()
              .retract(15.f, m_filpar[m_current_tool].unloading_speed_start * 60.f) // feedrate 5000mm/min = 83mm/s
              .retract(0.70f * total_retraction_distance, 1.0f * m_filpar[m_current_tool].unloading_speed * 60.f)
              .retract(0.20f * total_retraction_distance, 0.5f * m_filpar[m_current_tool].unloading_speed * 60.f)
              .retract(0.10f * total_retraction_distance, 0.3f * m_filpar[m_current_tool].unloading_speed * 60.f)
              .resume_preview();
    }
    // Wipe tower should only change temperature with single extruder MM. Otherwise, all temperatures should
    // be already set and there is no need to change anything. Also, the temperature could be changed
    // for wrong extruder.
    if (m_semm) {
        if (new_temperature != 0 && (new_temperature != m_old_temperature || is_first_layer()) ) { 	// Set the extruder temperature, but don't wait.
            // If the required temperature is the same as last time, don't emit the M104 again (if user adjusted the value, it would be reset)
            // However, always change temperatures on the first layer (this is to avoid issues with priming lines turned off).
            writer.set_extruder_temp(new_temperature, false);
            m_old_temperature = new_temperature;
        }
    }

    // Cooling:
    const int& number_of_moves = m_filpar[m_current_tool].cooling_moves;
    if (number_of_moves > 0) {
        const float& initial_speed = m_filpar[m_current_tool].cooling_initial_speed;
        const float& final_speed   = m_filpar[m_current_tool].cooling_final_speed;

        float speed_inc = (final_speed - initial_speed) / (2.f * number_of_moves - 1.f);

        writer.suppress_preview()
              .travel(writer.x(), writer.y() + y_step);
        old_x = writer.x();
        turning_point = xr-old_x > old_x-xl ? xr : xl;
        for (int i=0; i<number_of_moves; ++i) {
            float speed = initial_speed + speed_inc * 2*i;
            writer.load_move_x_advanced(turning_point, m_cooling_tube_length, speed);
            speed += speed_inc;
            writer.load_move_x_advanced(old_x, -m_cooling_tube_length, speed);
        }
    }

    // let's wait is necessary:
    writer.wait(m_filpar[m_current_tool].delay);
    // we should be at the beginning of the cooling tube again - let's move to parking position:
    writer.retract(-m_cooling_tube_length/2.f+m_parking_pos_retraction-m_cooling_tube_retraction, 2000);

	// this is to align ramming and future wiping extrusions, so the future y-steps can be uniform from the start:
    // the perimeter_width will later be subtracted, it is there to not load while moving over just extruded material
	writer.travel(end_of_ramming.x(), end_of_ramming.y() + (y_step/m_extra_spacing-m_perimeter_width) / 2.f + m_perimeter_width, 2400.f);

	writer.resume_preview()
		  .flush_planner_queue();
#endif
}

// Change the tool, set a speed override for soluble and flex materials.
void WipeTower::toolchange_Change(
	WipeTowerWriter &writer,
    const size_t 	new_tool,
    const std::string&  new_material)
{
    // Ask the writer about how much of the old filament we consumed:
    if (m_current_tool < m_used_filament_length.size())
    	m_used_filament_length[m_current_tool] += writer.get_and_reset_used_filament_length();

    // This is where we want to place the custom gcodes. We will use placeholders for this.
    // These will be substituted by the actual gcodes when the gcode is generated.
    writer.append("[filament_end_gcode]\n");
    writer.append("[change_filament_gcode]\n");

    // BBS: do travel in GCode::append_tcr() for lazy_lift
#if 0
    // Travel to where we assume we are. Custom toolchange or some special T code handling (parking extruder etc)
    // gcode could have left the extruder somewhere, we cannot just start extruding. We should also inform the
    // postprocessor that we absolutely want to have this in the gcode, even if it thought it is the same as before.
    Vec2f current_pos = writer.pos_rotated();
    writer.feedrate(m_travel_speed * 60.f)
          .append(std::string("G1 X") + Slic3r::float_to_string_decimal_point(current_pos.x())
                             +  " Y"  + Slic3r::float_to_string_decimal_point(current_pos.y())
                             + never_skip_tag() + "\n");
#endif

    // The toolchange Tn command will be inserted later, only in case that the user does
    // not provide a custom toolchange gcode.
	writer.set_tool(new_tool); // This outputs nothing, the writer just needs to know the tool has changed.
    writer.append("[filament_start_gcode]\n");

	writer.flush_planner_queue();
	m_current_tool = new_tool;
}

void WipeTower::toolchange_Load(
	WipeTowerWriter &writer,
	const box_coordinates  &cleaning_box)
{
    // BBS: tool load is done in change_filament_gcode
#if 0
    if (m_semm && (m_parking_pos_retraction != 0 || m_extra_loading_move != 0)) {
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
#endif
}

// Wipe the newly loaded filament until the end of the assigned wipe area.
void WipeTower::toolchange_Wipe(
	WipeTowerWriter &writer,
	const box_coordinates  &cleaning_box,
	float wipe_length)
{
	// Increase flow on first layer, slow down print.
    writer.set_extrusion_flow(m_extrusion_flow * (is_first_layer() ? 1.15f : 1.f))
		  .append("; CP TOOLCHANGE WIPE\n");

    // BBS: add the note for gcode-check, when the flow changed, the width should follow the change
    if (is_first_layer()) {
        writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Width) + std::to_string(1.15 * m_perimeter_width) + "\n");
    }

	const float& xl = cleaning_box.ld.x();
	const float& xr = cleaning_box.rd.x();

	// Variables x_to_wipe and traversed_x are here to be able to make sure it always wipes at least
    //   the ordered volume, even if it means violating the box. This can later be removed and simply
    // wipe until the end of the assigned area.

    float x_to_wipe = wipe_length;
    float dy = m_layer_info->extra_spacing * m_perimeter_width;

    const float target_speed = is_first_layer() ? std::min(m_first_layer_speed * 60.f, 4800.f) : 4800.f;
    float wipe_speed = 0.33f * target_speed;

    float start_y = writer.y();

#if 0
    // if there is less than 2.5*m_perimeter_width to the edge, advance straightaway (there is likely a blob anyway)
    if ((m_left_to_right ? xr-writer.x() : writer.x()-xl) < 2.5f*m_perimeter_width) {
        writer.travel((m_left_to_right ? xr-m_perimeter_width : xl+m_perimeter_width),writer.y()+dy);
        m_left_to_right = !m_left_to_right;
    }
#endif

    m_left_to_right = ((m_cur_layer_id + 3) % 4 >= 2);
    bool is_from_up = (m_cur_layer_id % 2 == 1);

    // BBS: do not need to move dy
#if 0
    if (m_depth_traversed != 0)
        writer.travel(xl, writer.y() + dy);
#endif
    
    bool need_change_flow = false;
    // now the wiping itself:
	for (int i = 0; true; ++i)	{
		if (i!=0) {
            if      (wipe_speed < 0.34f * target_speed) wipe_speed = 0.375f * target_speed;
            else if (wipe_speed < 0.377 * target_speed) wipe_speed = 0.458f * target_speed;
            else if (wipe_speed < 0.46f * target_speed) wipe_speed = 0.875f * target_speed;
            else wipe_speed = std::min(target_speed, wipe_speed + 50.f);
		}

        // BBS: check the bridging area and use the bridge flow
        if (need_change_flow || need_thick_bridge_flow(writer.y())) {
            writer.set_extrusion_flow(extrusion_flow(0.2));
            writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height) + std::to_string(0.2) + "\n");
            need_change_flow = true;
        }

        if (m_left_to_right)
            writer.extrude(xr + wipe_tower_wall_infill_overlap * m_perimeter_width, writer.y(), wipe_speed);
        else
            writer.extrude(xl - wipe_tower_wall_infill_overlap * m_perimeter_width, writer.y(), wipe_speed);

        // BBS: recover the flow in non-bridging area
        if (need_change_flow) {
            writer.set_extrusion_flow(m_extrusion_flow);
            writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height) + std::to_string(m_layer_height) + "\n");
        }

        if (!is_from_up && (writer.y() - float(EPSILON) > cleaning_box.lu.y()))
            break;		// in case next line would not fit

        if (is_from_up && (writer.y() + float(EPSILON) < cleaning_box.ld.y()))
            break;

        x_to_wipe -= (xr - xl);
		if (x_to_wipe < WT_EPSILON) {
            // BBS: Delete some unnecessary travel
            //writer.travel(m_left_to_right ? xl + 1.5f*m_perimeter_width : xr - 1.5f*m_perimeter_width, writer.y(), 7200);
			break;
		}
		// stepping to the next line:
        if (is_from_up)
            writer.extrude(writer.x(), writer.y() - dy);
        else
            writer.extrude(writer.x(), writer.y() + dy);

		m_left_to_right = !m_left_to_right;
	}

    float end_y = writer.y();

    // We may be going back to the model - wipe the nozzle. If this is followed
    // by finish_layer, this wipe path will be overwritten.
    //writer.add_wipe_point(writer.x(), writer.y())
    //      .add_wipe_point(writer.x(), writer.y() - dy)
    //      .add_wipe_point(! m_left_to_right ? m_wipe_tower_width : 0.f, writer.y() - dy);
    // BBS: modify the wipe_path after toolchange
    writer.add_wipe_point(writer.x(), writer.y())
          .add_wipe_point(! m_left_to_right ? m_wipe_tower_width : 0.f, writer.y());

    if (m_layer_info != m_plan.end() && m_current_tool != m_layer_info->tool_changes.back().new_tool)
        m_left_to_right = !m_left_to_right;

    writer.set_extrusion_flow(m_extrusion_flow); // Reset the extrusion flow.
    // BBS: add the note for gcode-check when the flow changed
    if (is_first_layer()) {
        writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Width) + std::to_string(m_perimeter_width) + "\n");
    }
}



// BBS
WipeTower::box_coordinates WipeTower::align_perimeter(const WipeTower::box_coordinates& perimeter_box)
{
    box_coordinates aligned_box = perimeter_box;

    float spacing = m_extra_spacing * m_perimeter_width;
    float up = perimeter_box.lu(1) - m_perimeter_width - EPSILON;
    up = align_ceil(up, spacing);
    up += m_perimeter_width;
    up = std::min(up, m_wipe_tower_depth);

    float down = perimeter_box.ld(1) - m_perimeter_width + EPSILON;
    down = align_floor(down, spacing);
    down += m_perimeter_width;
    down = std::max(down, -m_y_shift);

    aligned_box.lu(1) = aligned_box.ru(1) = up;
    aligned_box.ld(1) = aligned_box.rd(1) = down;

    return aligned_box;
}

WipeTower::ToolChangeResult WipeTower::finish_layer(bool extrude_perimeter, bool extruder_fill)
{
	assert(! this->layer_finished());
    m_current_layer_finished = true;

    size_t old_tool = m_current_tool;

	WipeTowerWriter writer(m_layer_height, m_perimeter_width, m_gcode_flavor, m_filpar);
	writer.set_extrusion_flow(m_extrusion_flow)
		.set_z(m_z_pos)
		.set_initial_tool(m_current_tool)
        .set_y_shift(m_y_shift - (m_current_shape == SHAPE_REVERSED ? m_layer_info->toolchanges_depth() : 0.f));

    writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_Start) + "\n");

	// Slow down on the 1st layer.
    bool first_layer = is_first_layer();
    // BBS: speed up perimeter speed to 90mm/s for non-first layer
    float           feedrate   = first_layer ? std::min(m_first_layer_speed * 60.f, 5400.f) : std::min(60.0f * m_filpar[m_current_tool].max_e_speed / m_extrusion_flow, 5400.f);
    float fill_box_y = m_layer_info->toolchanges_depth() + m_perimeter_width;
    box_coordinates fill_box(Vec2f(m_perimeter_width, fill_box_y),
                             m_wipe_tower_width - 2 * m_perimeter_width, m_layer_info->depth - fill_box_y);

    writer.set_initial_position((m_left_to_right ? fill_box.ru : fill_box.lu), // so there is never a diagonal travel
                                 m_wipe_tower_width, m_wipe_tower_depth, m_internal_rotation);

    bool toolchanges_on_layer = m_layer_info->toolchanges_depth() > WT_EPSILON;

    // inner perimeter of the sparse section, if there is space for it:
    if (fill_box.ru.y() - fill_box.rd.y() > m_perimeter_width - WT_EPSILON)
        writer.rectangle_fill_box(this, fill_box.ld, fill_box.rd.x() - fill_box.ld.x(), fill_box.ru.y() - fill_box.rd.y(), feedrate);

    // we are in one of the corners, travel to ld along the perimeter:
    // BBS: Delete some unnecessary travel
    //if (writer.x() > fill_box.ld.x() + EPSILON) writer.travel(fill_box.ld.x(), writer.y());
    //if (writer.y() > fill_box.ld.y() + EPSILON) writer.travel(writer.x(), fill_box.ld.y());

    // Extrude infill to support the material to be printed above.
    const float dy = (fill_box.lu.y() - fill_box.ld.y() - m_perimeter_width);
    float left = fill_box.lu.x() + 2*m_perimeter_width;
    float right = fill_box.ru.x() - 2 * m_perimeter_width;
    std::vector<Vec2f> finish_rect_wipe_path;
    if (extruder_fill && dy > m_perimeter_width)
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
            // BBS: add wipe_path for this case: only with finish rectangle
            finish_rect_wipe_path.emplace_back(writer.pos());
            finish_rect_wipe_path.emplace_back(Vec2f(left + dx * n, n % 2 ? fill_box.ru.y() : fill_box.rd.y()));
        }

        writer.append("; CP EMPTY GRID END\n"
                      ";------------------\n\n\n\n\n\n\n");
    }

    // outer perimeter (always):
    // BBS
    box_coordinates wt_box(Vec2f(0.f, (m_current_shape == SHAPE_REVERSED ? m_layer_info->toolchanges_depth() : 0.f)),
        m_wipe_tower_width, m_layer_info->depth + m_perimeter_width);
    wt_box = align_perimeter(wt_box);
    if (extrude_perimeter) {
        writer.rectangle(wt_box, feedrate);
    }

    // brim chamfer
    float spacing = m_perimeter_width - m_layer_height * float(1. - M_PI_4);
    // How many perimeters shall the brim have?
    int loops_num = (m_wipe_tower_brim_width + spacing / 2.f) / spacing;
    const float max_chamfer_width = 3.f;
    if (!first_layer) {
        // stop print chamfer if depth changes
        if (m_layer_info->depth != m_plan.front().depth) {
            loops_num = 0;
        }
        else {
            // limit max chamfer width to 3 mm
            int chamfer_loops_num = (int)(max_chamfer_width / spacing);
            int dist_to_1st = m_layer_info - m_plan.begin() - m_first_layer_idx;
            loops_num = std::min(loops_num, chamfer_loops_num) - dist_to_1st;
        }
    }

    if (loops_num > 0) {
        box_coordinates box = wt_box;
        for (size_t i = 0; i < loops_num; ++i) {
            box.expand(spacing);
            writer.rectangle(box, feedrate);
        }

        if (first_layer) {
            // Save actual brim width to be later passed to the Print object, which will use it
            // for skirt calculation and pass it to GLCanvas for precise preview box
            m_wipe_tower_brim_width_real = wt_box.ld.x() - box.ld.x() + spacing / 2.f;
        }
        wt_box = box;
    }

    // Now prepare future wipe. box contains rectangle that was extruded last (ccw).
    Vec2f target = (writer.pos() == wt_box.ld ? wt_box.rd :
                   (writer.pos() == wt_box.rd ? wt_box.ru :
                   (writer.pos() == wt_box.ru ? wt_box.lu :
                    wt_box.ld)));

    // BBS: add wipe_path for this case: only with finish rectangle
    if (finish_rect_wipe_path.size() == 2 && finish_rect_wipe_path[0] == writer.pos())
        target = finish_rect_wipe_path[1];

    writer.add_wipe_point(writer.pos())
          .add_wipe_point(target);

    writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_End) + "\n");

    // Ask our writer about how much material was consumed.
    // Skip this in case the layer is sparse and config option to not print sparse layers is enabled.
    if (! m_no_sparse_layers || toolchanges_on_layer)
        if (m_current_tool < m_used_filament_length.size())
            m_used_filament_length[m_current_tool] += writer.get_and_reset_used_filament_length();

    return construct_tcr(writer, false, old_tool, true, false, 0.f);
}

// Appends a toolchange into m_plan and calculates neccessary depth of the corresponding box
void WipeTower::plan_toolchange(float z_par, float layer_height_par, unsigned int old_tool,
                                unsigned int new_tool, float wipe_volume, float purge_volume)
{
	assert(m_plan.empty() || m_plan.back().z <= z_par + WT_EPSILON);	// refuses to add a layer below the last one

	if (m_plan.empty() || m_plan.back().z + WT_EPSILON < z_par) // if we moved to a new layer, we'll add it to m_plan first
		m_plan.push_back(WipeTowerInfo(z_par, layer_height_par));

    if (m_first_layer_idx == size_t(-1) && (! m_no_sparse_layers || old_tool != new_tool))
        m_first_layer_idx = m_plan.size() - 1;

    if (old_tool == new_tool)	// new layer without toolchanges - we are done
        return;

	// this is an actual toolchange - let's calculate depth to reserve on the wipe tower
    float depth = 0.f;
    float width = m_wipe_tower_width - 2 * m_perimeter_width;

    // BBS: if the wipe tower width is too small, the depth will be infinity
    if (width <= EPSILON)
        return;

    // BBS: remove old filament ramming and first line
#if 0
	float length_to_extrude = volume_to_length(0.25f * std::accumulate(m_filpar[old_tool].ramming_speed.begin(), m_filpar[old_tool].ramming_speed.end(), 0.f),
										m_perimeter_width * m_filpar[old_tool].ramming_line_width_multiplicator,
										layer_height_par);
	depth = (int(length_to_extrude / width) + 1) * (m_perimeter_width * m_filpar[old_tool].ramming_line_width_multiplicator * m_filpar[old_tool].ramming_step_multiplicator);
    float ramming_depth = depth;
    length_to_extrude = width*((length_to_extrude / width)-int(length_to_extrude / width)) - width;
    float first_wipe_line = -length_to_extrude;
    length_to_extrude += volume_to_length(wipe_volume, m_perimeter_width, layer_height_par);
    length_to_extrude = std::max(length_to_extrude,0.f);

    depth += (int(length_to_extrude / width) + 1) * m_perimeter_width;
    depth *= m_extra_spacing;

    m_plan.back().tool_changes.push_back(WipeTowerInfo::ToolChange(old_tool, new_tool, depth, ramming_depth, first_wipe_line, wipe_volume));
#else
    float length_to_extrude = volume_to_length(wipe_volume, m_perimeter_width, layer_height_par);

    depth += std::ceil(length_to_extrude / width) * m_perimeter_width;
    //depth *= m_extra_spacing;

    float nozzle_change_depth = 0;
    if (!m_filament_map.empty() && m_filament_map[old_tool] != m_filament_map[new_tool]) {
        double e_flow                   = nozzle_change_extrusion_flow(layer_height_par);
        double length                   = m_filaments_change_length[old_tool] / e_flow;
        int    nozzle_change_line_count = length / (m_wipe_tower_width - 2*m_nozzle_change_perimeter_width) + 1;
        if (has_tpu_filament())
            nozzle_change_depth = m_tpu_fixed_spacing * nozzle_change_line_count * m_nozzle_change_perimeter_width;
        else
            nozzle_change_depth = nozzle_change_line_count * m_nozzle_change_perimeter_width;
        depth += nozzle_change_depth;
    }
    WipeTowerInfo::ToolChange tool_change = WipeTowerInfo::ToolChange(old_tool, new_tool, depth, 0.f, 0.f, wipe_volume, length_to_extrude, purge_volume);
    tool_change.nozzle_change_depth       = nozzle_change_depth;
    m_plan.back().tool_changes.push_back(tool_change);
#endif
}



void WipeTower::plan_tower()
{
    // BBS
    // calculate extra spacing
    float max_depth = 0.f;
    for (auto& info : m_plan)
        max_depth = std::max(max_depth, info.toolchanges_depth());

    float min_wipe_tower_depth = WipeTower::get_limit_depth_by_height(m_wipe_tower_height);

    {
        if (m_enable_wrapping_detection && max_depth < EPSILON)
            max_depth = wrapping_wipe_tower_depth;

        if (m_enable_timelapse_print && max_depth < EPSILON)
            max_depth = min_wipe_tower_depth;

        if (max_depth + EPSILON < min_wipe_tower_depth && !has_tpu_filament())
            m_extra_spacing = min_wipe_tower_depth / max_depth;
        else
            m_extra_spacing = 1.f;

        for (int idx = 0; idx < m_plan.size(); idx++) {
            auto& info = m_plan[idx];
            if (idx == 0 && m_extra_spacing > 1.f + EPSILON) {
                // apply solid fill for the first layer
                info.extra_spacing = 1.f;
                for (auto& toolchange : info.tool_changes) {
                    float x_to_wipe = volume_to_length(toolchange.wipe_volume, m_perimeter_width, info.height);
                    float line_len = m_wipe_tower_width - 2 * m_perimeter_width;
                    float x_to_wipe_new = x_to_wipe * m_extra_spacing;
                    x_to_wipe_new = std::floor(x_to_wipe_new / line_len) * line_len;
                    x_to_wipe_new = std::max(x_to_wipe_new, x_to_wipe);

                    int line_count = std::ceil((x_to_wipe_new - WT_EPSILON) / line_len);

                    {  // nozzle change length
                        int nozzle_change_line_count = (toolchange.nozzle_change_depth + WT_EPSILON) / m_perimeter_width;
                        line_count += nozzle_change_line_count;
                    }

                    toolchange.required_depth = line_count * m_perimeter_width;
                    toolchange.wipe_volume = x_to_wipe_new / x_to_wipe * toolchange.wipe_volume;
                    toolchange.wipe_length = x_to_wipe_new;
                }
            }
            else {
                info.extra_spacing = m_extra_spacing;
                for (auto& toolchange : info.tool_changes) {
                    toolchange.required_depth *= m_extra_spacing;
                    toolchange.wipe_length = volume_to_length(toolchange.wipe_volume, m_perimeter_width, info.height);
                }
            }
        }
    }

	// Calculate m_wipe_tower_depth (maximum depth for all the layers) and propagate depths downwards
	m_wipe_tower_depth = 0.f;
	for (auto& layer : m_plan)
		layer.depth = 0.f;

    float max_depth_for_all = 0;
    for (int layer_index = int(m_plan.size()) - 1; layer_index >= 0; --layer_index)
	{
        float this_layer_depth = std::max(m_plan[layer_index].depth, m_plan[layer_index].toolchanges_depth());
        if (m_enable_wrapping_detection && (layer_index < m_wrapping_detection_layers) && this_layer_depth < EPSILON)
            this_layer_depth = wrapping_wipe_tower_depth;

        if (m_enable_timelapse_print && this_layer_depth < EPSILON)
            this_layer_depth = min_wipe_tower_depth;

		m_plan[layer_index].depth = this_layer_depth;

		if (this_layer_depth > m_wipe_tower_depth - m_perimeter_width)
			m_wipe_tower_depth = this_layer_depth + m_perimeter_width;

		for (int i = layer_index - 1; i >= 0 ; i--)
		{
			if (m_plan[i].depth - this_layer_depth < 2*m_perimeter_width )
				m_plan[i].depth = this_layer_depth;
		}

        if (m_enable_timelapse_print && layer_index == 0)
            max_depth_for_all = m_plan[0].depth;
    }

    if (m_enable_wrapping_detection) {
        for (int i = m_wrapping_detection_layers - 1; i >= 0; i--) {
            if (m_plan.size() <= m_wrapping_detection_layers && (m_plan[i].depth < wrapping_wipe_tower_depth)) {
                m_plan[i].depth = wrapping_wipe_tower_depth;
            }
        }
    }

    if (m_enable_timelapse_print) {
        for (int i = int(m_plan.size()) - 1; i >= 0; i--) {
            m_plan[i].depth = max_depth_for_all;
        }
    }
}

void WipeTower::save_on_last_wipe()
{
    for (m_layer_info=m_plan.begin();m_layer_info<m_plan.end();++m_layer_info) {
        set_layer(m_layer_info->z, m_layer_info->height, 0, m_layer_info->z == m_plan.front().z, m_layer_info->z == m_plan.back().z);
        if (m_layer_info->tool_changes.size()==0)   // we have no way to save anything on an empty layer
            continue;

        // Which toolchange will finish_layer extrusions be subtracted from?
        // BBS: consider both soluable and support properties
        int idx = first_toolchange_to_nonsoluble_nonsupport(m_layer_info->tool_changes);

        for (int i=0; i<int(m_layer_info->tool_changes.size()); ++i) {
            auto& toolchange = m_layer_info->tool_changes[i];
            tool_change(toolchange.new_tool);

            if (i == idx) {
                float width = m_wipe_tower_width - 3*m_perimeter_width; // width we draw into
                float length_to_save = finish_layer().total_extrusion_length_in_plane();
                float length_to_wipe = volume_to_length(toolchange.wipe_volume,
                                      m_perimeter_width, m_layer_info->height)  - toolchange.first_wipe_line - length_to_save;

                length_to_wipe = std::max(length_to_wipe,0.f);
                float depth_to_wipe = m_perimeter_width * (std::floor(length_to_wipe/width) + ( length_to_wipe > 0.f ? 1.f : 0.f ) ) * m_extra_spacing;

                toolchange.required_depth = toolchange.ramming_depth + depth_to_wipe;
            }
        }
    }
}

bool WipeTower::is_tpu_filament(int filament_id) const
{
    return m_filpar[filament_id].material == "TPU";
}

// BBS: consider both soluable and support properties
// Return index of first toolchange that switches to non-soluble and non-support extruder
// ot -1 if there is no such toolchange.
int WipeTower::first_toolchange_to_nonsoluble_nonsupport(
        const std::vector<WipeTowerInfo::ToolChange>& tool_changes) const
{
    for (size_t idx=0; idx<tool_changes.size(); ++idx)
        if (! m_filpar[tool_changes[idx].new_tool].is_soluble && ! m_filpar[tool_changes[idx].new_tool].is_support)
            return idx;
    return -1;
}

static WipeTower::ToolChangeResult merge_tcr(WipeTower::ToolChangeResult& first,
                                             WipeTower::ToolChangeResult& second)
{
    assert(first.new_tool == second.initial_tool);
    WipeTower::ToolChangeResult out = first;
    if ((first.end_pos - second.start_pos).norm() > (float)EPSILON) {
        std::string travel_gcode = "G1 X" + Slic3r::float_to_string_decimal_point(second.start_pos.x(), 3) + " Y" +
                                   Slic3r::float_to_string_decimal_point(second.start_pos.y(), 3) + " F5400" + "\n";
        bool need_insert_travel = true;
        if (second.is_tool_change
            && is_approx(second.start_pos.x(), second.tool_change_start_pos.x())
            && is_approx(second.start_pos.y(), second.tool_change_start_pos.y())) {
            // will insert travel in gcode.cpp
            need_insert_travel = false;
        }

        if (need_insert_travel)
            out.gcode += travel_gcode;
    }
    out.gcode += second.gcode;
    out.extrusions.insert(out.extrusions.end(), second.extrusions.begin(), second.extrusions.end());
    out.end_pos = second.end_pos;
    out.wipe_path = second.wipe_path;
    out.initial_tool = first.initial_tool;
    out.new_tool = second.new_tool;

    if (!first.nozzle_change_result.gcode.empty())
        out.nozzle_change_result = first.nozzle_change_result;
    else if (!second.nozzle_change_result.gcode.empty())
        out.nozzle_change_result = second.nozzle_change_result;

    if (first.is_tool_change) {
        out.is_tool_change = true;
        out.tool_change_start_pos = first.tool_change_start_pos;
    }
    else if (second.is_tool_change) {
        out.is_tool_change = true;
        out.tool_change_start_pos = second.tool_change_start_pos;
    }
    else {
        out.is_tool_change = false;
    }

    // BBS
    out.purge_volume += second.purge_volume;
    return out;
}

void WipeTower::get_wall_skip_points(const WipeTowerInfo &layer)
{
    m_wall_skip_points.clear();
    std::unordered_map<int, float> cur_block_depth;
    for (int i = 0; i < int(layer.tool_changes.size()); ++i) {
        const WipeTowerInfo::ToolChange &tool_change         = layer.tool_changes[i];
        size_t                           old_filament        = tool_change.old_tool;
        size_t                           new_filament        = tool_change.new_tool;
        float                            spacing             = m_layer_info->extra_spacing;
        if (has_tpu_filament() && m_layer_info->extra_spacing < m_tpu_fixed_spacing) spacing = 1;
        float nozzle_change_depth = tool_change.nozzle_change_depth * spacing;
        //float                            nozzle_change_depth = tool_change.nozzle_change_depth * (has_tpu_filament() ? m_tpu_fixed_spacing : layer.extra_spacing);
        auto* block = get_block_by_category(m_filpar[new_filament].category, false);
        if (!block)
            continue;
        //float wipe_depth    = tool_change.required_depth - nozzle_change_depth;
        float wipe_depth    = ceil(tool_change.wipe_length / (m_wipe_tower_width - 2 * m_perimeter_width)) * m_perimeter_width*layer.extra_spacing;
        float                            process_depth       = 0.f;
        if (!cur_block_depth.count(m_filpar[new_filament].category))
            cur_block_depth[m_filpar[new_filament].category] = block->start_depth;
        process_depth = cur_block_depth[m_filpar[new_filament].category];
        if (!m_filament_map.empty() && new_filament < m_filament_map.size() && m_filament_map[old_filament] != m_filament_map[new_filament]) {
            if (m_filament_categories[new_filament] == m_filament_categories[old_filament])
                process_depth += nozzle_change_depth;
            else {
                if (!cur_block_depth.count(m_filpar[old_filament].category)) {
                    auto* old_block = get_block_by_category(m_filpar[old_filament].category, false);
                    if (!old_block)
                        continue;
                    cur_block_depth[m_filpar[old_filament].category] = old_block->start_depth;
                }
                cur_block_depth[m_filpar[old_filament].category] += nozzle_change_depth;
            }
        }
            {
            Vec2f res;
            int   index = m_cur_layer_id % 4;
            switch (index % 4) {
            case 0: res = Vec2f(0, process_depth); break;
            case 1: res = Vec2f(m_wipe_tower_width, process_depth + wipe_depth - layer.extra_spacing*m_perimeter_width); break;
            case 2: res = Vec2f(m_wipe_tower_width, process_depth); break;
            case 3: res = Vec2f(0, process_depth + wipe_depth - layer.extra_spacing * m_perimeter_width); break;
            default: break;
            }

            m_wall_skip_points.emplace_back(res);
        }
        cur_block_depth[m_filpar[new_filament].category] = process_depth + tool_change.required_depth - tool_change.nozzle_change_depth * layer.extra_spacing;
    }
    }

WipeTower::ToolChangeResult WipeTower::tool_change_new(size_t new_tool, bool solid_toolchange,bool solid_nozzlechange)
{
    m_nozzle_change_result.gcode.clear();
    if (!m_filament_map.empty() && new_tool < m_filament_map.size() && m_filament_map[m_current_tool] != m_filament_map[new_tool]) {
        m_nozzle_change_result = nozzle_change_new(m_current_tool, new_tool, solid_nozzlechange);
    }

    size_t old_tool = m_current_tool;
    float wipe_depth          = 0.f;
    float wipe_length         = 0.f;
    float purge_volume        = 0.f;
    float nozzle_change_depth = 0.f;
    int   nozzle_change_line_count = 0;

    if (new_tool != (unsigned int) (-1)) {
        for (const auto &b : m_layer_info->tool_changes)
            if (b.new_tool == new_tool) {
                wipe_length         = b.wipe_length;
                wipe_depth          = b.required_depth;
                purge_volume        = b.purge_volume;
                nozzle_change_depth = b.nozzle_change_depth;
                if (has_tpu_filament())
                    nozzle_change_line_count = ((b.nozzle_change_depth + WT_EPSILON) / m_nozzle_change_perimeter_width) / 2;
                else
                    nozzle_change_line_count = (b.nozzle_change_depth + WT_EPSILON) / m_nozzle_change_perimeter_width;
                break;
            }
    }

    WipeTowerBlock* block = get_block_by_category(m_filpar[new_tool].category, false);
    if (!block) {
        assert(block != nullptr);
        return WipeTower::ToolChangeResult();
    }
    m_cur_block = block;
    box_coordinates cleaning_box(Vec2f(m_perimeter_width, block->cur_depth), m_wipe_tower_width - 2 * m_perimeter_width, wipe_depth-m_layer_info->extra_spacing*nozzle_change_depth);

    WipeTowerWriter writer(m_layer_height, m_perimeter_width, m_gcode_flavor, m_filpar);
    writer.set_extrusion_flow(m_extrusion_flow)
        .set_z(m_z_pos)
        .set_initial_tool(m_current_tool)
        .set_y_shift(m_y_shift + (new_tool != (unsigned int) (-1) && (m_current_shape == SHAPE_REVERSED) ? m_layer_info->depth - m_layer_info->toolchanges_depth() : 0.f))
        .append(";--------------------\n"
                "; CP TOOLCHANGE START\n")
        .comment_with_value(" toolchange #", m_num_tool_changes + 1); // the number is zero-based

    if (new_tool != (unsigned) (-1))
        writer.append( std::string("; material : " + (m_current_tool < m_filpar.size() ? m_filpar[m_current_tool].material : "(NONE)") + " -> " + m_filpar[new_tool].material + "\n").c_str())
            .append(";--------------------\n");

    writer.speed_override_backup();
    writer.speed_override(100);

    // Ram the hot material out of the melt zone, retract the filament into the cooling tubes and let it cool.
    if (new_tool != (unsigned int) -1) { // This is not the last change.
        Vec2f initial_position = get_next_pos(cleaning_box, wipe_length);
        writer.set_initial_position(initial_position, m_wipe_tower_width, m_wipe_tower_depth, m_internal_rotation);

        writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_Start) + "\n");
        toolchange_Unload(writer, cleaning_box, m_filpar[m_current_tool].material,
                          is_first_layer() ? m_filpar[new_tool].nozzle_temperature_initial_layer : m_filpar[new_tool].nozzle_temperature);
        toolchange_Change(writer, new_tool, m_filpar[new_tool].material); // Change the tool, set a speed override for soluble and flex materials.
        toolchange_Load(writer, cleaning_box);

        if (m_is_multi_extruder && is_tpu_filament(new_tool)) {
            float dy = m_layer_info->extra_spacing * m_nozzle_change_perimeter_width;
            if (m_layer_info->extra_spacing < m_tpu_fixed_spacing) {
                dy = m_tpu_fixed_spacing * m_nozzle_change_perimeter_width;
            }

            float nozzle_change_speed = 60.0f * m_filpar[new_tool].max_e_speed / m_extrusion_flow;
            nozzle_change_speed *= 0.25;

            const float &xl = cleaning_box.ld.x();
            const float &xr = cleaning_box.rd.x();

            Vec2f  start_pos         = m_nozzle_change_result.origin_start_pos + Vec2f(0, m_nozzle_change_perimeter_width);
            bool   left_to_right     = true;
            int tpu_line_count = (nozzle_change_line_count + 2 - 1) / 2; // nozzle_change_line_count / 2 round up

            writer.travel(start_pos);

            for (int i = 0; true; ++i) {
                if (left_to_right)
                    writer.travel(xr - m_perimeter_width, writer.y(), nozzle_change_speed);
                else
                    writer.travel(xl + m_perimeter_width, writer.y(), nozzle_change_speed);

                if (i == tpu_line_count - 1) break;

                writer.travel(writer.x(), writer.y() + dy);
                left_to_right = !left_to_right;
            }
            writer.travel(initial_position);
        }

        toolchange_wipe_new(writer, cleaning_box, wipe_length, solid_toolchange);

        writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_End) + "\n");
        ++m_num_tool_changes;
    } else
        toolchange_Unload(writer, cleaning_box, m_filpar[m_current_tool].material, m_filpar[m_current_tool].nozzle_temperature);

    block->cur_depth += (wipe_depth - nozzle_change_depth * m_layer_info->extra_spacing);
    block->last_filament_change_id = new_tool;

    // BBS
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

    return construct_tcr(writer, false, old_tool, false, true, purge_volume);
}

WipeTower::NozzleChangeResult WipeTower::nozzle_change_new(int old_filament_id, int new_filament_id, bool solid_infill)
{
    int   nozzle_change_line_count = 0;
    if (new_filament_id != (unsigned int) (-1)) {
        for (const auto &b : m_layer_info->tool_changes)
            if (b.new_tool == new_filament_id) {
                if (has_tpu_filament())
                    nozzle_change_line_count = ((b.nozzle_change_depth + WT_EPSILON) / m_nozzle_change_perimeter_width) / 2;
                else
                    nozzle_change_line_count = (b.nozzle_change_depth + WT_EPSILON) / m_nozzle_change_perimeter_width;
                break;
            }
    }

    float nz_extrusion_flow = nozzle_change_extrusion_flow(m_layer_height);
    float nozzle_change_speed = 60.0f * m_filpar[m_current_tool].max_e_speed / nz_extrusion_flow;
    nozzle_change_speed       = solid_infill ? 40.f * 60.f : nozzle_change_speed;//If the contact layers belong to different categories, then reduce the speed.

    if (is_tpu_filament(m_current_tool)) {
        nozzle_change_speed *= 0.25;
    }
    float bridge_speed = std::min(60.0f * m_filpar[m_current_tool].max_e_speed / nozzle_change_extrusion_flow(0.2), nozzle_change_speed); // limit the bridge speed by add flow

    WipeTowerWriter writer(m_layer_height, m_nozzle_change_perimeter_width, m_gcode_flavor, m_filpar);
    writer.set_extrusion_flow(nz_extrusion_flow)
        .set_z(m_z_pos)
        .set_initial_tool(m_current_tool)
        .set_y_shift(m_y_shift + (new_filament_id != (unsigned int) (-1) && (m_current_shape == SHAPE_REVERSED) ? m_layer_info->depth - m_layer_info->toolchanges_depth() : 0.f))
        .append("; Nozzle change start\n");

    WipeTowerBlock* block = get_block_by_category(m_filpar[old_filament_id].category, false);
    if (!block) {
        assert(false);
        return WipeTower::NozzleChangeResult();
    }
    m_cur_block           = block;
    float dy = m_layer_info->extra_spacing * m_nozzle_change_perimeter_width;
    if (has_tpu_filament() && m_extra_spacing < m_tpu_fixed_spacing)
        dy = m_tpu_fixed_spacing * m_nozzle_change_perimeter_width;

    float x_offset = m_perimeter_width + (m_nozzle_change_perimeter_width - m_perimeter_width) / 2;
    box_coordinates cleaning_box(Vec2f(x_offset,block->cur_depth + (m_nozzle_change_perimeter_width - m_perimeter_width) / 2),
                                 m_wipe_tower_width - 2 * x_offset,
                                 nozzle_change_line_count * dy - (m_nozzle_change_perimeter_width - m_perimeter_width) / 2);

    Vec2f initial_position = cleaning_box.ld;
    writer.set_initial_position(initial_position, m_wipe_tower_width, m_wipe_tower_depth, m_internal_rotation);

    const float &xl          = cleaning_box.ld.x();
    const float &xr          = cleaning_box.rd.x();
    dy              = solid_infill ? m_nozzle_change_perimeter_width : dy;
    nozzle_change_line_count = solid_infill ? std::numeric_limits<int>::max() : nozzle_change_line_count;
    m_left_to_right = true;
    int real_nozzle_change_line_count = 0;
    bool need_change_flow              = false;
    for (int i = 0; true; ++i) {
        if (need_thick_bridge_flow(writer.pos().y())) {
            writer.set_extrusion_flow(nozzle_change_extrusion_flow(0.2));
            writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height) + std::to_string(0.2) + "\n");
            need_change_flow = true;
        }
        if (m_left_to_right)
            writer.extrude(xr + wipe_tower_wall_infill_overlap * m_perimeter_width, writer.y(), need_change_flow ? bridge_speed : nozzle_change_speed);
        else
            writer.extrude(xl - wipe_tower_wall_infill_overlap * m_perimeter_width, writer.y(), need_change_flow ? bridge_speed : nozzle_change_speed);
        real_nozzle_change_line_count++;
        if (i == nozzle_change_line_count - 1)
            break;
        if ((writer.y() + dy - cleaning_box.ru.y()+(m_nozzle_change_perimeter_width+m_perimeter_width)/2) > (float)EPSILON) break;
        if (need_change_flow) {
            writer.set_extrusion_flow(nozzle_change_extrusion_flow(m_layer_height));
            writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height) + std::to_string(m_layer_height) + "\n");
            need_change_flow = false;
        }
        writer.extrude(writer.x(), writer.y() + dy, nozzle_change_speed);
        m_left_to_right = !m_left_to_right;
    }
    if (need_change_flow) {
        writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height) + std::to_string(m_layer_height) + "\n");
    }
    writer.set_extrusion_flow(nz_extrusion_flow); // Reset the extrusion flow.
    block->cur_depth += real_nozzle_change_line_count * dy;
    block->last_nozzle_change_id = old_filament_id;

    NozzleChangeResult result;
    if (is_tpu_filament(m_current_tool)) {
        bool   left_to_right     = !m_left_to_right;
        int  tpu_line_count = (real_nozzle_change_line_count + 2 - 1) / 2; // nozzle_change_line_count / 2 round up
        nozzle_change_speed *= 2;
        writer.travel(writer.x(), writer.y() - m_nozzle_change_perimeter_width);

        for (int i = 0; true; ++i) {
            if (left_to_right)
                writer.travel(xr - m_perimeter_width, writer.y(), nozzle_change_speed);
            else
                writer.travel(xl + m_perimeter_width, writer.y(), nozzle_change_speed);

            if (i == tpu_line_count - 1)
                break;

            writer.travel(writer.x(), writer.y() - dy);
            left_to_right = !left_to_right;
        }
    } else {
        result.wipe_path.push_back(writer.pos_rotated());
        if (m_left_to_right) {
            result.wipe_path.push_back(Vec2f(0, writer.pos_rotated().y()));
        } else {
            result.wipe_path.push_back(Vec2f(m_wipe_tower_width, writer.pos_rotated().y()));
        }
    }

    writer.append("; Nozzle change end\n");

    result.start_pos = writer.start_pos_rotated();
    result.origin_start_pos = initial_position;
    result.end_pos   = writer.pos_rotated();
    result.gcode     = writer.gcode();
    return result;
}

WipeTower::ToolChangeResult WipeTower::finish_layer_new(bool extrude_perimeter, bool extrude_fill, bool extrude_fill_wall)
{
    assert(!this->layer_finished());
    m_current_layer_finished = true;

    WipeTowerWriter writer(m_layer_height, m_perimeter_width, m_gcode_flavor, m_filpar);
    writer.set_extrusion_flow(m_extrusion_flow)
        .set_z(m_z_pos)
        .set_initial_tool(m_current_tool)
        .set_y_shift(m_y_shift - (m_current_shape == SHAPE_REVERSED ? m_layer_info->toolchanges_depth() : 0.f));

    writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_Start) + "\n");

    // Slow down on the 1st layer.
    bool first_layer = is_first_layer();
    // BBS: speed up perimeter speed to 90mm/s for non-first layer
    float           feedrate   = first_layer ? std::min(m_first_layer_speed * 60.f, 5400.f) : std::min(60.0f * m_filpar[m_current_tool].max_e_speed / m_extrusion_flow, 5400.f);

    float fill_box_depth = m_wipe_tower_depth - 2 * m_perimeter_width;
    if (m_wipe_tower_blocks.size() == 1) {
        fill_box_depth = m_layer_info->depth - 2 * m_perimeter_width;
    }
    box_coordinates fill_box(Vec2f(m_perimeter_width, m_perimeter_width), m_wipe_tower_width - 2 * m_perimeter_width, fill_box_depth);

    writer.set_initial_position((m_left_to_right ? fill_box.ru : fill_box.lu), m_wipe_tower_width, m_wipe_tower_depth, m_internal_rotation);

    bool toolchanges_on_layer = m_layer_info->toolchanges_depth() > WT_EPSILON;

    std::vector<Vec2f> finish_rect_wipe_path;
    if (extrude_fill_wall) {
        // inner perimeter of the sparse section, if there is space for it:
        if (fill_box.ru.y() - fill_box.rd.y() > WT_EPSILON) {
            writer.rectangle_fill_box(this, fill_box, finish_rect_wipe_path, feedrate);
        }
    }

    // Extrude infill to support the material to be printed above.
    const float        dy    = (fill_box.lu.y() - fill_box.ld.y() - m_perimeter_width);
    float              left  = fill_box.lu.x() + 2 * m_perimeter_width;
    float              right = fill_box.ru.x() - 2 * m_perimeter_width;
    if (extrude_fill && dy > m_perimeter_width) {
        writer.travel(fill_box.ld + Vec2f(m_perimeter_width * 2, 0.f))
            .append(";--------------------\n"
                    "; CP EMPTY GRID START\n")
            .comment_with_value(" layer #", m_num_layer_changes + 1);

        // Is there a soluble filament wiped/rammed at the next layer?
        // If so, the infill should not be sparse.
        bool solid_infill = m_layer_info + 1 == m_plan.end() ?
                                false :
                                std::any_of((m_layer_info + 1)->tool_changes.begin(), (m_layer_info + 1)->tool_changes.end(),
                                            [this](const WipeTowerInfo::ToolChange &tch) { return m_filpar[tch.new_tool].is_soluble || m_filpar[tch.old_tool].is_soluble; });
        solid_infill |= first_layer && m_adhesion;

        if (solid_infill) {
            float sparse_factor = 1.5f; // 1=solid, 2=every other line, etc.
            if (first_layer) {          // the infill should touch perimeters
                left -= m_perimeter_width;
                right += m_perimeter_width;
                sparse_factor = 1.f;
            }
            float y       = fill_box.ld.y() + m_perimeter_width;
            int   n       = dy / (m_perimeter_width * sparse_factor);
            float spacing = (dy - m_perimeter_width) / (n - 1);
            int   i       = 0;
            for (i = 0; i < n; ++i) {
                writer.extrude(writer.x(), y, feedrate).extrude(i % 2 ? left : right, y);
                y = y + spacing;
            }
            writer.extrude(writer.x(), fill_box.lu.y());
        } else {
            // Extrude an inverse U at the left of the region and the sparse infill.
            writer.extrude(fill_box.lu + Vec2f(m_perimeter_width * 2, 0.f), feedrate);

            const int   n  = 1 + int((right - left) / m_bridging);
            const float dx = (right - left) / n;
            for (int i = 1; i <= n; ++i) {
                float x = left + dx * i;
                writer.travel(x, writer.y());
                writer.extrude(x, i % 2 ? fill_box.rd.y() : fill_box.ru.y());
            }

            finish_rect_wipe_path.clear();
            // BBS: add wipe_path for this case: only with finish rectangle
            finish_rect_wipe_path.emplace_back(writer.pos());
            finish_rect_wipe_path.emplace_back(Vec2f(left + dx * n, n % 2 ? fill_box.ru.y() : fill_box.rd.y()));
        }

        writer.append("; CP EMPTY GRID END\n"
                      ";------------------\n\n\n\n\n\n\n");
    }

    // outer perimeter (always):
    // BBS
    float wipe_tower_depth = m_wipe_tower_depth;
    if (m_wipe_tower_blocks.size() == 1) {
        wipe_tower_depth = m_layer_info->depth + m_perimeter_width;
    }
    box_coordinates wt_box(Vec2f(0.f, 0.f), m_wipe_tower_width, wipe_tower_depth);
    wt_box = align_perimeter(wt_box);

    //if (extrude_perimeter && !m_use_rib_wall) {
    //    if (!m_use_gap_wall)
    //        writer.rectangle(wt_box, feedrate);
    //    else
    //        generate_support_wall(writer, wt_box, feedrate, first_layer);
    //}
    Polygon outer_wall;
    outer_wall = generate_support_wall_new(writer, wt_box, feedrate, first_layer, m_use_rib_wall, extrude_perimeter, m_use_gap_wall);
    if (extrude_perimeter) {
        Polyline shift_polyline = to_polyline(outer_wall);
        shift_polyline.translate(0, scaled(m_y_shift));
        m_outer_wall[m_z_pos].push_back(shift_polyline);
    }
    // brim chamfer
    float spacing = m_perimeter_width - m_layer_height * float(1. - M_PI_4);
    // How many perimeters shall the brim have?
    int loops_num = (m_wipe_tower_brim_width + spacing / 2.f) / spacing;
    const float max_chamfer_width = 3.f;
    if (!first_layer) {
        // stop print chamfer if depth changes
        if (m_layer_info->depth != m_plan.front().depth) {
            loops_num = 0;
        } else {
            // limit max chamfer width to 3 mm
            int chamfer_loops_num = (int) (max_chamfer_width / spacing);
            int dist_to_1st       = m_layer_info - m_plan.begin() - m_first_layer_idx;
            loops_num             = std::min(loops_num, chamfer_loops_num) - dist_to_1st;
        }
    }

    if (loops_num > 0) {
        //box_coordinates box = wt_box;
        for (size_t i = 0; i < loops_num; ++i) {
            outer_wall = offset(outer_wall, scaled(spacing)).front();
            writer.polygon(outer_wall, feedrate);
            m_outer_wall[m_z_pos].push_back(to_polyline(outer_wall));
        }

            /*for (size_t i = 0; i < loops_num; ++i) {
                box.expand(spacing);
                writer.rectangle(box, feedrate);
            }*/

        if (first_layer) {
            // Save actual brim width to be later passed to the Print object, which will use it
            // for skirt calculation and pass it to GLCanvas for precise preview box
            m_wipe_tower_brim_width_real = loops_num * spacing + spacing / 2.f;
            //m_wipe_tower_brim_width_real = wt_box.ld.x() - box.ld.x() + spacing / 2.f;
        }
        //wt_box = box;
    }

    if (extrude_perimeter || loops_num > 0) {
        writer.add_wipe_path(outer_wall, m_filpar[m_current_tool].wipe_dist);
    }
    else {
        // Now prepare future wipe. box contains rectangle that was extruded last (ccw).
        Vec2f target = (writer.pos() == wt_box.ld ? wt_box.rd : (writer.pos() == wt_box.rd ? wt_box.ru : (writer.pos() == wt_box.ru ? wt_box.lu : wt_box.ld)));

        // BBS: add wipe_path for this case: only with finish rectangle
        if (finish_rect_wipe_path.size() == 2 && finish_rect_wipe_path[0] == writer.pos()) target = finish_rect_wipe_path[1];

        writer.add_wipe_point(writer.pos()).add_wipe_point(target);
    }
    writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_End) + "\n");

    // Ask our writer about how much material was consumed.
    // Skip this in case the layer is sparse and config option to not print sparse layers is enabled.
    if (!m_no_sparse_layers || toolchanges_on_layer)
        if (m_current_tool < m_used_filament_length.size())
            m_used_filament_length[m_current_tool] += writer.get_and_reset_used_filament_length();

    m_nozzle_change_result.gcode.clear();
    return construct_tcr(writer, false, m_current_tool, true, false, 0.f);
}

WipeTower::ToolChangeResult WipeTower::finish_block(const WipeTowerBlock &block, int filament_id, bool extrude_fill)
{
    WipeTowerWriter writer(m_layer_height, m_perimeter_width, m_gcode_flavor, m_filpar);
    writer.set_extrusion_flow(m_extrusion_flow)
        .set_z(m_z_pos)
        .set_initial_tool(filament_id)
        .set_y_shift(m_y_shift - (m_current_shape == SHAPE_REVERSED ? m_layer_info->toolchanges_depth() : 0.f));

    writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_Start) + "\n");

    // Slow down on the 1st layer.
    bool first_layer = is_first_layer();
    // BBS: speed up perimeter speed to 90mm/s for non-first layer
    float feedrate = first_layer ? std::min(m_first_layer_speed * 60.f, 5400.f) : std::min(60.0f * m_filpar[filament_id].max_e_speed / m_extrusion_flow, 5400.f);

    box_coordinates fill_box(Vec2f(0, 0), 0, 0);
    fill_box = box_coordinates(Vec2f(m_perimeter_width, block.cur_depth), m_wipe_tower_width - 2 * m_perimeter_width, block.start_depth + block.layer_depths[m_cur_layer_id] - block.cur_depth - m_perimeter_width);

    writer.set_initial_position((m_left_to_right ? fill_box.ru : fill_box.lu), m_wipe_tower_width, m_wipe_tower_depth, m_internal_rotation);

    bool toolchanges_on_layer = m_layer_info->toolchanges_depth() > WT_EPSILON;

    std::vector<Vec2f> finish_rect_wipe_path;
    // inner perimeter of the sparse section, if there is space for it:
    if (fill_box.ru.y() - fill_box.rd.y() > WT_EPSILON) {
        writer.rectangle_fill_box(this, fill_box, finish_rect_wipe_path, feedrate);
    }

    // Extrude infill to support the material to be printed above.
    const float        dy    = (fill_box.lu.y() - fill_box.ld.y() - m_perimeter_width);
    float              left  = fill_box.lu.x() + 2 * m_perimeter_width;
    float              right = fill_box.ru.x() - 2 * m_perimeter_width;
    if (extrude_fill && dy > m_perimeter_width) {
        writer.travel(fill_box.ld + Vec2f(m_perimeter_width * 2, 0.f))
            .append(";--------------------\n"
                    "; CP EMPTY GRID START\n")
            .comment_with_value(" layer #", m_num_layer_changes + 1);

        // Is there a soluble filament wiped/rammed at the next layer?
        // If so, the infill should not be sparse.
        bool solid_infill = m_layer_info + 1 == m_plan.end() ?
                                false :
                                std::any_of((m_layer_info + 1)->tool_changes.begin(), (m_layer_info + 1)->tool_changes.end(),
                                            [this](const WipeTowerInfo::ToolChange &tch) { return m_filpar[tch.new_tool].is_soluble || m_filpar[tch.old_tool].is_soluble; });
        solid_infill |= first_layer && m_adhesion;

        if (solid_infill) {
            float sparse_factor = 1.5f; // 1=solid, 2=every other line, etc.
            if (first_layer) {          // the infill should touch perimeters
                left -= m_perimeter_width;
                right += m_perimeter_width;
                sparse_factor = 1.f;
            }
            float y       = fill_box.ld.y() + m_perimeter_width;
            int   n       = dy / (m_perimeter_width * sparse_factor);
            float spacing = (dy - m_perimeter_width) / (n - 1);
            int   i       = 0;
            for (i = 0; i < n; ++i) {
                writer.extrude(writer.x(), y, feedrate).extrude(i % 2 ? left : right, y);
                y = y + spacing;
            }
            writer.extrude(writer.x(), fill_box.lu.y());
        } else {
            // Extrude an inverse U at the left of the region and the sparse infill.
            writer.extrude(fill_box.lu + Vec2f(m_perimeter_width * 2, 0.f), feedrate);

            const int   n  = 1 + int((right - left) / m_bridging);
            const float dx = (right - left) / n;
            for (int i = 1; i <= n; ++i) {
                float x = left + dx * i;
                writer.travel(x, writer.y());
                writer.extrude(x, i % 2 ? fill_box.rd.y() : fill_box.ru.y());
            }
            finish_rect_wipe_path.clear();
            // BBS: add wipe_path for this case: only with finish rectangle
            finish_rect_wipe_path.emplace_back(writer.pos());
            finish_rect_wipe_path.emplace_back(Vec2f(left + dx * n, n % 2 ? fill_box.ru.y() : fill_box.rd.y()));
        }

        writer.append("; CP EMPTY GRID END\n"
                      ";------------------\n\n\n\n\n\n\n");
    }

    // outer perimeter (always):
    // BBS
    box_coordinates wt_box(Vec2f(0.f, 0.f), m_wipe_tower_width, m_layer_info->depth + m_perimeter_width);
    wt_box = align_perimeter(wt_box);

    // Now prepare future wipe. box contains rectangle that was extruded last (ccw).
    Vec2f target = (writer.pos() == wt_box.ld ? wt_box.rd : (writer.pos() == wt_box.rd ? wt_box.ru : (writer.pos() == wt_box.ru ? wt_box.lu : wt_box.ld)));

    // BBS: add wipe_path for this case: only with finish rectangle
    if (finish_rect_wipe_path.size() == 2 && finish_rect_wipe_path[0] == writer.pos()) target = finish_rect_wipe_path[1];

    writer.add_wipe_point(writer.pos()).add_wipe_point(target);

    writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_End) + "\n");

    // Ask our writer about how much material was consumed.
    // Skip this in case the layer is sparse and config option to not print sparse layers is enabled.
    if (!m_no_sparse_layers || toolchanges_on_layer)
        if (filament_id < m_used_filament_length.size())
            m_used_filament_length[filament_id] += writer.get_and_reset_used_filament_length();

    return construct_block_tcr(writer, false, filament_id, true, 0.f);
}

WipeTower::ToolChangeResult WipeTower::finish_block_solid(const WipeTowerBlock &block, int filament_id, bool extrude_fill, bool interface_solid)
{
    float layer_height = m_layer_height;
    float e_flow = m_extrusion_flow;
    if (m_cur_layer_id > 1 && !block.solid_infill[m_cur_layer_id - 1] && m_extrusion_flow < extrusion_flow(0.2)) {
        layer_height = 0.2;
        e_flow = extrusion_flow(0.2);
    }

    WipeTowerWriter writer(layer_height, m_perimeter_width, m_gcode_flavor, m_filpar);
    writer.set_extrusion_flow(e_flow)
        .set_z(m_z_pos)
        .set_initial_tool(filament_id)
        .set_y_shift(m_y_shift - (m_current_shape == SHAPE_REVERSED ? m_layer_info->toolchanges_depth() : 0.f));

    writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_Start) + "\n");

    // Slow down on the 1st layer.
    bool first_layer = is_first_layer();
    // BBS: speed up perimeter speed to 90mm/s for non-first layer
    float feedrate = first_layer ? std::min(m_first_layer_speed * 60.f, 5400.f) : std::min(60.0f * m_filpar[filament_id].max_e_speed / m_extrusion_flow, 5400.f);
    feedrate       = interface_solid ? 20.f * 60.f : feedrate;
    box_coordinates fill_box(Vec2f(0, 0), 0, 0);
    fill_box = box_coordinates(Vec2f(m_perimeter_width, block.cur_depth), m_wipe_tower_width - 2 * m_perimeter_width,
                               block.start_depth + block.layer_depths[m_cur_layer_id] - block.cur_depth - m_perimeter_width);

    writer.set_initial_position((m_left_to_right ? fill_box.rd : fill_box.ld), m_wipe_tower_width, m_wipe_tower_depth, m_internal_rotation);
    m_left_to_right = !m_left_to_right;
    bool toolchanges_on_layer = m_layer_info->toolchanges_depth() > WT_EPSILON;

    // Extrude infill to support the material to be printed above.
    const float        dy    = (fill_box.lu.y() - fill_box.ld.y());
    float              left  = fill_box.lu.x();
    float              right = fill_box.ru.x();
    std::vector<Vec2f> finish_rect_wipe_path;
    {
        writer.append(";--------------------\n"
                    "; CP EMPTY GRID START\n")
            .comment_with_value(" layer #", m_num_layer_changes + 1);

        float y             = fill_box.ld.y();
        int   n             = (dy + 0.25 * m_perimeter_width) / m_perimeter_width + 1;
        float spacing       = m_perimeter_width;
        int   i             = 0;
        for (i = 0; i < n; ++i) {
            writer.extrude(m_left_to_right ? right : left, writer.y(), feedrate);
            if (i == n - 1) {
                writer.add_wipe_point(writer.pos()).add_wipe_point(Vec2f(m_left_to_right ? left : right, writer.y()));
                break;
            }
            m_left_to_right = !m_left_to_right;
            y = y + spacing;
            writer.extrude(writer.x(), y, feedrate);
        }

        writer.append("; CP EMPTY GRID END\n"
                      ";------------------\n\n\n\n\n\n\n");
    }

    writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_End) + "\n");

    // Ask our writer about how much material was consumed.
    // Skip this in case the layer is sparse and config option to not print sparse layers is enabled.
    if (!m_no_sparse_layers || toolchanges_on_layer)
        if (filament_id < m_used_filament_length.size())
            m_used_filament_length[filament_id] += writer.get_and_reset_used_filament_length();

    return construct_block_tcr(writer, false, filament_id, true, 0.f);
}

void WipeTower::toolchange_wipe_new(WipeTowerWriter &writer, const box_coordinates &cleaning_box, float wipe_length,bool solid_tool_toolchange)
{
    writer.set_extrusion_flow(m_extrusion_flow * (is_first_layer() ? 1.15f : 1.f)).append("; CP TOOLCHANGE WIPE\n");

    if (!m_nozzle_change_result.gcode.empty())
        writer.change_analyzer_line_width(m_perimeter_width);

    // BBS: add the note for gcode-check, when the flow changed, the width should follow the change
    if (is_first_layer()) {
        writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Width) + std::to_string(1.15 * m_perimeter_width) + "\n");
    }
    float        retract_length = m_filpar[m_current_tool].retract_length;
    float        retract_speed  = m_filpar[m_current_tool].retract_speed * 60;

    const float &xl = cleaning_box.ld.x();
    const float &xr = cleaning_box.rd.x();

    float x_to_wipe = wipe_length;
    float dy        = solid_tool_toolchange ? m_perimeter_width :m_layer_info->extra_spacing * m_perimeter_width;
    x_to_wipe                = solid_tool_toolchange ? std::numeric_limits<float>::max(): x_to_wipe;
    float target_speed = is_first_layer() ? std::min(m_first_layer_speed * 60.f, 4800.f) : 4800.f;
    target_speed             = solid_tool_toolchange ? 20.f * 60.f : target_speed;
    float       wipe_speed   = 0.33f * target_speed;

    m_left_to_right = ((m_cur_layer_id + 3) % 4 >= 2);

    bool is_from_up = (m_cur_layer_id % 2 == 1);

    // now the wiping itself:
    for (int i = 0; true; ++i) {
        if (i != 0) {
            if (wipe_speed < 0.34f * target_speed)
                wipe_speed = 0.375f * target_speed;
            else if (wipe_speed < 0.377 * target_speed)
                wipe_speed = 0.458f * target_speed;
            else if (wipe_speed < 0.46f * target_speed)
                wipe_speed = 0.875f * target_speed;
            else
                wipe_speed = std::min(target_speed, wipe_speed + 50.f);
        }

        bool need_change_flow = need_thick_bridge_flow(writer.y());
        // BBS: check the bridging area and use the bridge flow
        if (need_change_flow) {
            writer.set_extrusion_flow(extrusion_flow(0.2));
            writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height) + std::to_string(0.2) + "\n");
        }

        float ironing_length = 3.;
        if (i == 0 && m_use_gap_wall) { // BBS: add ironing after extruding start
            if (m_left_to_right) {
                float dx = xr + wipe_tower_wall_infill_overlap * m_perimeter_width - writer.pos().x();
                if (abs(dx) < ironing_length) ironing_length = abs(dx);
                writer.extrude(writer.x() + ironing_length, writer.y(), wipe_speed);
                writer.retract(retract_length, retract_speed);
                writer.travel(writer.x() - 1.5 * ironing_length, writer.y(), 600.);
                if (m_flat_ironing) {
                    writer.travel(writer.x() + 0.5f * ironing_length, writer.y(), 240.);
                    Vec2f pos{writer.x() + 1.f * ironing_length, writer.y()};
                    writer.spiral_flat_ironing(writer.pos(), flat_iron_area, m_perimeter_width, flat_iron_speed);
                    writer.travel(pos, wipe_speed);
                } else
                    writer.travel(writer.x() + 1.5 * ironing_length, writer.y(), 240.);
                writer.retract(-retract_length, retract_speed);
                writer.extrude(xr + wipe_tower_wall_infill_overlap * m_perimeter_width, writer.y(), wipe_speed);
            } else {
                float dx = xl - wipe_tower_wall_infill_overlap * m_perimeter_width - writer.pos().x();
                if (abs(dx) < ironing_length) ironing_length = abs(dx);
                writer.extrude(writer.x() - ironing_length, writer.y(), wipe_speed);
                writer.retract(retract_length, retract_speed);
                writer.travel(writer.x() + 1.5 * ironing_length, writer.y(), 600.);
                if (m_flat_ironing) {
                    writer.travel(writer.x() - 0.5f * ironing_length, writer.y(), 240.);
                    Vec2f pos{writer.x() - 1.0f * ironing_length, writer.y()};
                    writer.spiral_flat_ironing(writer.pos(), flat_iron_area, m_perimeter_width, flat_iron_speed);
                    writer.travel(pos, wipe_speed);
                }else
                    writer.travel(writer.x() - 1.5 * ironing_length, writer.y(), 240.);
                writer.retract(-retract_length, retract_speed);
                writer.extrude(xl - wipe_tower_wall_infill_overlap * m_perimeter_width, writer.y(), wipe_speed);
            }
        } else {
            if (m_left_to_right)
                writer.extrude(xr + wipe_tower_wall_infill_overlap * m_perimeter_width, writer.y(), wipe_speed);
            else
                writer.extrude(xl - wipe_tower_wall_infill_overlap * m_perimeter_width, writer.y(), wipe_speed);
        }

        // BBS: recover the flow in non-bridging area
        if (need_change_flow) {
            writer.set_extrusion_flow(m_extrusion_flow);
            writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height) + std::to_string(m_layer_height) + "\n");
        }

        if (!is_from_up && (writer.y() + dy - float(EPSILON) >cleaning_box.lu.y() - m_perimeter_width))
            break; // in case next line would not fit

        if (is_from_up && (writer.y() - dy+ float(EPSILON))<cleaning_box.ld.y()) // Because the top of the clean box cannot have wiring, but the bottom can have wiring.
            break;

        x_to_wipe -= (xr - xl);
        if (x_to_wipe < WT_EPSILON) {
            // BBS: Delete some unnecessary travel
            // writer.travel(m_left_to_right ? xl + 1.5f*m_perimeter_width : xr - 1.5f*m_perimeter_width, writer.y(), 7200);
            break;
        }
        // stepping to the next line:
        if (is_from_up)
            writer.extrude(writer.x(), writer.y() - dy);
        else
            writer.extrude(writer.x(), writer.y() + dy);

        m_left_to_right = !m_left_to_right;
    }

    writer.add_wipe_point(writer.x(), writer.y()).add_wipe_point(!m_left_to_right ? m_wipe_tower_width : 0.f, writer.y());

    if (m_layer_info != m_plan.end() && m_current_tool != m_layer_info->tool_changes.back().new_tool) m_left_to_right = !m_left_to_right;

    writer.set_extrusion_flow(m_extrusion_flow); // Reset the extrusion flow.
    // BBS: add the note for gcode-check when the flow changed
    if (is_first_layer()) { writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Width) + std::to_string(m_perimeter_width) + "\n"); }
}

WipeTower::WipeTowerBlock * WipeTower::get_block_by_category(int filament_adhesiveness_category, bool create)
{
    auto iter = std::find_if(m_wipe_tower_blocks.begin(), m_wipe_tower_blocks.end(), [&filament_adhesiveness_category](const WipeTower::WipeTowerBlock &item) {
        return item.filament_adhesiveness_category == filament_adhesiveness_category;
    });

    if (iter != m_wipe_tower_blocks.end()) {
        return &(*iter);
    }

    if (create) {
        WipeTower::WipeTowerBlock new_block;
        new_block.block_id = m_wipe_tower_blocks.size();
        new_block.filament_adhesiveness_category = filament_adhesiveness_category;
        m_wipe_tower_blocks.emplace_back(new_block);
        return &m_wipe_tower_blocks.back();
    }

    return nullptr;
}

void WipeTower::add_depth_to_block(int filament_id, int filament_adhesiveness_category, float depth, bool is_nozzle_change)
{
    std::vector<WipeTower::BlockDepthInfo> &layer_depth = m_all_layers_depth[m_cur_layer_id];
    auto iter = std::find_if(layer_depth.begin(), layer_depth.end(), [&filament_adhesiveness_category](const WipeTower::BlockDepthInfo &item) {
        return item.category == filament_adhesiveness_category;
    });

    if (iter != layer_depth.end()) {
        iter->depth += depth;
        if (is_nozzle_change)
            iter->nozzle_change_depth += depth;
    }
    else {
        WipeTower::BlockDepthInfo new_block;
        new_block.category = filament_adhesiveness_category;
        new_block.depth = depth;
        if (is_nozzle_change)
            new_block.nozzle_change_depth += depth;
        layer_depth.emplace_back(std::move(new_block));
    }
}

int WipeTower::get_filament_category(int filament_id)
{
    if (filament_id >= m_filament_categories.size())
        return 0;
    return m_filament_categories[filament_id];
}

bool WipeTower::is_in_same_extruder(int filament_id_1, int filament_id_2)
{
    if (filament_id_1 >= m_filament_map.size() || filament_id_2 >= m_filament_map.size())
        return true;

    return m_filament_map[filament_id_1] == m_filament_map[filament_id_2];
}

void WipeTower::reset_block_status()
{
    for (auto &block : m_wipe_tower_blocks) {
        block.cur_depth = block.start_depth;
        block.last_filament_change_id = -1;
        block.last_nozzle_change_id   = -1;
    }
}

void WipeTower::update_all_layer_depth(float wipe_tower_depth)
{
    m_wipe_tower_depth = 0.f;
    float start_offset = m_perimeter_width;
    float start_depth = start_offset;
    for (auto& block : m_wipe_tower_blocks) {
        block.depth *= m_extra_spacing;
        block.start_depth = start_depth;
        start_depth += block.depth;
        m_wipe_tower_depth += block.depth;

        for (auto& layer_depth : block.layer_depths) {
            layer_depth *= m_extra_spacing;
        }

        for (WipeTowerInfo& plan_info : m_plan) {
            plan_info.depth *= m_extra_spacing;
        }
    }
    if (m_wipe_tower_depth > 0)
        m_wipe_tower_depth += start_offset;

    if (m_enable_wrapping_detection || m_enable_timelapse_print) {
        if (is_approx(m_wipe_tower_depth, 0.f))
            m_wipe_tower_depth = wipe_tower_depth;
        for (WipeTowerInfo &plan_info : m_plan) {
            plan_info.depth = m_wipe_tower_depth;
        }
    }
}

void WipeTower::generate_wipe_tower_blocks()
{
    // 1. generate all layer depth
    m_all_layers_depth.clear();
    m_all_layers_depth.resize(m_plan.size());
    m_cur_layer_id = 0;
    for (auto& info : m_plan) {
        for (const WipeTowerInfo::ToolChange &tool_change : info.tool_changes) {
            if (is_in_same_extruder(tool_change.old_tool, tool_change.new_tool)) {
                int filament_adhesiveness_category = get_filament_category(tool_change.new_tool);
                add_depth_to_block(tool_change.new_tool, filament_adhesiveness_category, tool_change.required_depth);
            }
            else {
                int old_filament_category = get_filament_category(tool_change.old_tool);
                add_depth_to_block(tool_change.old_tool, old_filament_category, tool_change.nozzle_change_depth, true);
                int new_filament_category = get_filament_category(tool_change.new_tool);
                add_depth_to_block(tool_change.new_tool, new_filament_category, tool_change.required_depth - tool_change.nozzle_change_depth);

            }
        }
        ++m_cur_layer_id;
    }

    // 2. generate all layer depth
    std::vector<std::unordered_map<int, float>> all_layer_category_to_depth(m_plan.size());
    for (size_t layer_id = 0; layer_id < m_all_layers_depth.size(); ++layer_id) {
        const auto& layer_blocks = m_all_layers_depth[layer_id];
        std::unordered_map<int, float> &category_to_depth = all_layer_category_to_depth[layer_id];
        for (auto block : layer_blocks) {
            category_to_depth[block.category] = block.depth;
        }
    }

    // 3. generate wipe tower block
    m_wipe_tower_blocks.clear();
    for (int layer_id = 0; layer_id < all_layer_category_to_depth.size(); ++layer_id) {
        const auto &layer_category_depths = all_layer_category_to_depth[layer_id];
        for (auto iter = layer_category_depths.begin(); iter != layer_category_depths.end(); ++iter) {
            auto* block = get_block_by_category(iter->first, true);
            if (block->layer_depths.empty()) {
                block->layer_depths.resize(all_layer_category_to_depth.size(), 0);
                block->solid_infill.resize(all_layer_category_to_depth.size(), false);
                block->finish_depth.resize(all_layer_category_to_depth.size(), 0);
            }
            block->depth = std::max(block->depth, iter->second);
            block->layer_depths[layer_id] = iter->second;
        }
    }

    // add solid infill flag
    int solid_infill_layer = 4;
    for (WipeTowerBlock& block : m_wipe_tower_blocks) {
        for (int layer_id = 0; layer_id < all_layer_category_to_depth.size(); ++layer_id) {
            std::unordered_map<int, float> &category_to_depth = all_layer_category_to_depth[layer_id];
            if (is_approx(category_to_depth[block.filament_adhesiveness_category], 0.f)) {
                int layer_count = solid_infill_layer;
                while (layer_count > 0) {
                    if (layer_id + layer_count < all_layer_category_to_depth.size()) {
                        std::unordered_map<int, float>& up_layer_depth = all_layer_category_to_depth[layer_id + layer_count];
                        if (!is_approx(up_layer_depth[block.filament_adhesiveness_category], 0.f)) {
                            block.solid_infill[layer_id] = true;
                            break;
                        }
                    }
                    --layer_count;
                }
            }
        }
    }

    // 4. get real depth for every layer
    for (int layer_id = m_plan.size() - 1; layer_id >= 0; --layer_id) {
        m_plan[layer_id].depth = 0;
        for (auto& block : m_wipe_tower_blocks) {
            if (layer_id < m_plan.size() - 1)
                block.layer_depths[layer_id] = std::max(block.layer_depths[layer_id], block.layer_depths[layer_id + 1]);
            m_plan[layer_id].depth += block.layer_depths[layer_id];
        }
    }

    if (m_tower_framework) {
        for (int layer_id = 1; layer_id < m_plan.size(); ++layer_id) {
            m_plan[layer_id].depth = 0;
            for (auto &block : m_wipe_tower_blocks) {
                block.layer_depths[layer_id] = block.layer_depths[0];
                m_plan[layer_id].depth += block.layer_depths[layer_id];
            }
        }
    }
}

void WipeTower::plan_tower_new()
{
    if (m_wipe_tower_brim_width < 0) m_wipe_tower_brim_width = get_auto_brim_by_height(m_wipe_tower_height);
    if (m_use_rib_wall) {
        // recalculate wipe_tower_with and layer's depth
        generate_wipe_tower_blocks();
        float max_depth    = std::accumulate(m_wipe_tower_blocks.begin(), m_wipe_tower_blocks.end(), 0.f, [](float a, const auto &t) { return a + t.depth; }) + m_perimeter_width;
        float square_width = align_ceil(std::sqrt(max_depth * m_wipe_tower_width * m_extra_spacing), m_perimeter_width);
        //std::cout << " before  m_wipe_tower_width = " << m_wipe_tower_width << "  max_depth = " << max_depth << std::endl;
        m_wipe_tower_width = square_width;
        float width        = m_wipe_tower_width - 2 * m_perimeter_width;
        for (int idx = 0; idx < m_plan.size(); idx++) {
            for (auto &toolchange : m_plan[idx].tool_changes) {
                float length_to_extrude   = toolchange.wipe_length;
                float depth               = std::ceil(length_to_extrude / width) * m_perimeter_width;
                float nozzle_change_depth = 0;
                if (!m_filament_map.empty() && m_filament_map[toolchange.old_tool] != m_filament_map[toolchange.new_tool]) {
                    double e_flow                   = nozzle_change_extrusion_flow(m_plan[idx].height);
                    double length                   = m_filaments_change_length[toolchange.old_tool] / e_flow;
                    int    nozzle_change_line_count = length / (m_wipe_tower_width - 2*m_nozzle_change_perimeter_width) + 1;
                    if (has_tpu_filament())
                        nozzle_change_depth = m_tpu_fixed_spacing * nozzle_change_line_count * m_nozzle_change_perimeter_width;
                    else
                        nozzle_change_depth = nozzle_change_line_count * m_nozzle_change_perimeter_width;
                    depth += nozzle_change_depth;
                }
                toolchange.nozzle_change_depth = nozzle_change_depth;
                toolchange.required_depth      = depth;
            }
        }
    }

    generate_wipe_tower_blocks();

    float max_depth = 0.f;
    for (const auto &block : m_wipe_tower_blocks) {
        max_depth += block.depth;
    }
    //std::cout << " after square " << m_wipe_tower_width << "  depth  " << max_depth << std::endl;

    float min_wipe_tower_depth = get_limit_depth_by_height(m_wipe_tower_height);

    // only for get m_extra_spacing
    {
        if (m_enable_wrapping_detection && max_depth < EPSILON) {
            max_depth = wrapping_wipe_tower_depth;
            if (m_use_rib_wall) {
                m_wipe_tower_width = max_depth;
            }
        }

        if (m_enable_timelapse_print && max_depth < EPSILON) {
            max_depth = min_wipe_tower_depth;
            if (m_use_rib_wall) { m_wipe_tower_width = max_depth; }
        }

        if (max_depth + EPSILON < min_wipe_tower_depth) {
            //if enable rib_wall, there is no need to set extra_spacing
            if (m_use_rib_wall)
                m_rib_length = std::max(m_rib_length, min_wipe_tower_depth * (float) std::sqrt(2));
            else
                m_extra_spacing = std::max(min_wipe_tower_depth / max_depth, m_extra_spacing);
        }

        for (int idx = 0; idx < m_plan.size(); idx++) {
            auto &info = m_plan[idx];
            if (idx == 0 && m_extra_spacing > 1.f + EPSILON) {
                // apply solid fill for the first layer
                info.extra_spacing = 1.f;
                for (auto &toolchange : info.tool_changes) {
                    float x_to_wipe     = volume_to_length(toolchange.wipe_volume, m_perimeter_width, info.height);
                    float line_len      = m_wipe_tower_width - 2 * m_perimeter_width;
                    float x_to_wipe_new = x_to_wipe * m_extra_spacing;
                    x_to_wipe_new       = std::floor(x_to_wipe_new / line_len) * line_len;
                    x_to_wipe_new       = std::max(x_to_wipe_new, x_to_wipe);

                    int line_count = std::ceil((x_to_wipe_new - WT_EPSILON) / line_len);
                    // nozzle change length
                    int nozzle_change_line_count = (toolchange.nozzle_change_depth + WT_EPSILON) / m_nozzle_change_perimeter_width;

                    toolchange.required_depth = line_count * m_perimeter_width + nozzle_change_line_count * m_nozzle_change_perimeter_width;
                    toolchange.wipe_volume    = x_to_wipe_new / x_to_wipe * toolchange.wipe_volume;
                    toolchange.wipe_length    = x_to_wipe_new;
                }
            } else {
                info.extra_spacing = m_extra_spacing;
                for (auto &toolchange : info.tool_changes) {
                    toolchange.required_depth *= m_extra_spacing;
                    toolchange.wipe_length = volume_to_length(toolchange.wipe_volume, m_perimeter_width, info.height);
                }
            }
        }
    }

    update_all_layer_depth(max_depth);
    float diagonal = sqrt(m_wipe_tower_depth * m_wipe_tower_depth + m_wipe_tower_width * m_wipe_tower_width);
    m_rib_length    = std::max({m_rib_length, diagonal});
    m_rib_length += m_extra_rib_length;
    m_rib_length = std::max(diagonal, m_rib_length);
    m_rib_width  = std::min(m_rib_width, std::min(m_wipe_tower_depth, m_wipe_tower_width) / 2.f); // Ensure that the rib wall of the wipetower are attached to the infill.

}

int WipeTower::get_wall_filament_for_all_layer()
{
    std::map<int, int> category_counts;
    std::map<int, int> filament_counts;
    int current_tool = m_current_tool;
    for (const auto &layer : m_plan) {
        if (layer.tool_changes.empty()){
            filament_counts[current_tool]++;
            category_counts[get_filament_category(current_tool)]++;
            continue;
        }
        std::unordered_set<int> used_tools;
        std::unordered_set<int> used_category;
        for (size_t i = 0; i < layer.tool_changes.size(); ++i) {
            if (i == 0) {
                filament_counts[layer.tool_changes[i].old_tool]++;
                category_counts[get_filament_category(layer.tool_changes[i].old_tool)]++;
                used_tools.insert(layer.tool_changes[i].old_tool);
                used_category.insert(get_filament_category(layer.tool_changes[i].old_tool));
            }
            if (!used_category.count(get_filament_category(layer.tool_changes[i].new_tool)))
                category_counts[get_filament_category(layer.tool_changes[i].new_tool)]++;
            if (!used_tools.count(layer.tool_changes[i].new_tool))
                filament_counts[layer.tool_changes[i].new_tool]++;
            used_tools.insert(layer.tool_changes[i].new_tool);
            used_category.insert(get_filament_category(layer.tool_changes[i].new_tool));
        }
        current_tool = layer.tool_changes.empty()?current_tool:layer.tool_changes.back().new_tool;
    }

    // std::vector<std::pair<int, int>> category_counts_vec;
    int selected_category = -1;
    int selected_count    = 0;

    for (auto iter = category_counts.begin(); iter != category_counts.end(); ++iter) {
        if (iter->second > selected_count) {
            selected_category = iter->first;
            selected_count    = iter->second;
        }
    }

    // std::sort(category_counts_vec.begin(), category_counts_vec.end(), [](const std::pair<int, int> &left, const std::pair<int, int>& right) {
    //     return left.second > right.second;
    // });

    int filament_id    = -1;
    int filament_count = 0;
    for (auto iter = filament_counts.begin(); iter != filament_counts.end(); ++iter) {
        if (m_filament_categories[iter->first] == selected_category && iter->second > filament_count) {
            filament_id    = iter->first;
            filament_count = iter->second;
        }
    }
    return filament_id;
}

void WipeTower::generate_new(std::vector<std::vector<WipeTower::ToolChangeResult>> &result)
{
    if (m_plan.empty())
        return;
    //m_extra_spacing = 1.f;
    m_wipe_tower_height = m_plan.back().z;//real wipe_tower_height
    plan_tower_new();

    m_layer_info = m_plan.begin();

    for (const auto &layer : m_plan) {
        if (!layer.tool_changes.empty()) {
            m_current_tool = layer.tool_changes.front().old_tool;
            break;
        }
    }

    for (auto &used : m_used_filament_length) // reset used filament stats
        used = 0.f;

    int wall_filament = get_wall_filament_for_all_layer();

    std::vector<WipeTower::ToolChangeResult> layer_result;
    int index = 0;
    std::unordered_set<int> solid_blocks_id;// The contact surface of different bonded materials is solid.
    for (auto layer : m_plan) {
        reset_block_status();
        m_cur_layer_id = index++;
        set_layer(layer.z, layer.height, 0, false, layer.z == m_plan.back().z);

        if (m_layer_info->depth < m_perimeter_width) continue;

        if (m_wipe_tower_blocks.size() == 1) {
            if (m_layer_info->depth < m_wipe_tower_depth - m_perimeter_width) {
                // align y shift to perimeter width
                float dy  = m_extra_spacing * m_perimeter_width;
                m_y_shift = (m_wipe_tower_depth - m_layer_info->depth) / 2.f;
                m_y_shift = align_round(m_y_shift, dy);
            }
        }

        get_wall_skip_points(layer);

        ToolChangeResult finish_layer_tcr;
        ToolChangeResult timelapse_wall;

        auto get_wall_filament_for_this_layer = [this, &layer, &wall_filament]() -> int {
            if (layer.tool_changes.size() == 0)
                return -1;

            int candidate_id = -1;
            for (size_t idx = 0; idx < layer.tool_changes.size(); ++idx) {
                if (idx == 0) {
                    if (layer.tool_changes[idx].old_tool == wall_filament)
                        return wall_filament;
                    else if (m_filpar[layer.tool_changes[idx].old_tool].category == m_filpar[wall_filament].category) {
                        candidate_id = layer.tool_changes[idx].old_tool;
                    }
                }
                if (layer.tool_changes[idx].new_tool == wall_filament) {
                    return wall_filament;
                }

                if ((candidate_id == -1) && (m_filpar[layer.tool_changes[idx].new_tool].category == m_filpar[wall_filament].category))
                    candidate_id = layer.tool_changes[idx].new_tool;
            }
            return candidate_id == -1 ? layer.tool_changes[0].new_tool : candidate_id;
        };
        int wall_idx = get_wall_filament_for_this_layer();

        bool only_generate_wall = m_enable_timelapse_print || (m_enable_wrapping_detection && m_slice_used_filaments <= 1);
        // this layer has no tool_change
        if (wall_idx == -1) {
            bool need_insert_solid_infill = false;
            for (const WipeTowerBlock &block : m_wipe_tower_blocks) {
                if (block.solid_infill[m_cur_layer_id] && (block.filament_adhesiveness_category != m_filament_categories[m_current_tool])) {
                    need_insert_solid_infill = true;
                    break;
                }
            }

            if (need_insert_solid_infill) {
                wall_idx = m_current_tool;
            } else {
                if (only_generate_wall) {
                    timelapse_wall = only_generate_out_wall(true);
                }
                finish_layer_tcr = finish_layer_new(only_generate_wall ? false : true, layer.extruder_fill);
                std::for_each(m_wipe_tower_blocks.begin(), m_wipe_tower_blocks.end(), [this](WipeTowerBlock &block) {
                    block.finish_depth[this->m_cur_layer_id] = block.start_depth;
                });
            }
        }

        // generate tool change
        bool insert_wall = false;
        int  insert_finish_layer_idx = -1;
        if (wall_idx != -1 && only_generate_wall) {
            timelapse_wall = only_generate_out_wall(true);
        }
        for (int i = 0; i < int(layer.tool_changes.size()); ++i) {
            ToolChangeResult wall_gcode;
            if (i == 0 && (layer.tool_changes[i].old_tool == wall_idx)) {
                finish_layer_tcr = finish_layer_new(only_generate_wall ? false : true, false, false);
            }
            const auto * block = get_block_by_category(m_filpar[layer.tool_changes[i].new_tool].category, false);
            int         id    = std::find_if(m_wipe_tower_blocks.begin(), m_wipe_tower_blocks.end(), [&](const WipeTowerBlock &b) { return &b == block; }) - m_wipe_tower_blocks.begin();
            bool        solid_toolchange = solid_blocks_id.count(id);

            const auto * block2 = get_block_by_category(m_filpar[layer.tool_changes[i].old_tool].category, false);
            id = std::find_if(m_wipe_tower_blocks.begin(), m_wipe_tower_blocks.end(), [&](const WipeTowerBlock &b) { return &b == block2; }) - m_wipe_tower_blocks.begin();
            bool solid_nozzlechange = solid_blocks_id.count(id);
            layer_result.emplace_back(tool_change_new(layer.tool_changes[i].new_tool, solid_toolchange,solid_nozzlechange));

            if (i == 0 && (layer.tool_changes[i].old_tool == wall_idx)) {

            }
            else if (layer.tool_changes[i].new_tool == wall_idx) {
                finish_layer_tcr = finish_layer_new(only_generate_wall ? false : true, false, false);
                insert_finish_layer_idx = i;
            }
        }

        std::unordered_set<int> next_solid_blocks_id;
        // insert finish block
        if (wall_idx != -1) {
            if (layer.tool_changes.empty()) {
                finish_layer_tcr = finish_layer_new(only_generate_wall ? false : true, false, false);
            }

            for (WipeTowerBlock& block : m_wipe_tower_blocks) {
                block.finish_depth[m_cur_layer_id] = block.start_depth + block.depth;
                if (block.cur_depth + EPSILON >= block.start_depth + block.layer_depths[m_cur_layer_id]-m_perimeter_width) {
                    continue;
                }
                int id = std::find_if(m_wipe_tower_blocks.begin(), m_wipe_tower_blocks.end(), [&](const WipeTowerBlock &b) { return &b == &block; }) - m_wipe_tower_blocks.begin();
                bool interface_solid     = solid_blocks_id.count(id);
                int finish_layer_filament = -1;
                if (block.last_filament_change_id != -1) {
                    finish_layer_filament = block.last_filament_change_id;
                } else if (block.last_nozzle_change_id != -1) {
                    finish_layer_filament = block.last_nozzle_change_id;
                }

                if (!layer.tool_changes.empty()) {
                    WipeTowerBlock * last_layer_finish_block = get_block_by_category(get_filament_category(layer.tool_changes.front().old_tool), false);
                    if (last_layer_finish_block && last_layer_finish_block->block_id == block.block_id && finish_layer_filament == -1)
                        finish_layer_filament = layer.tool_changes.front().old_tool;
                }

                if (finish_layer_filament == -1) {
                    finish_layer_filament = wall_idx;
                }

                ToolChangeResult finish_block_tcr;
                if (interface_solid || (block.solid_infill[m_cur_layer_id] && block.filament_adhesiveness_category != m_filament_categories[finish_layer_filament])) {
                    interface_solid  = interface_solid && !((block.solid_infill[m_cur_layer_id] && block.filament_adhesiveness_category != m_filament_categories[finish_layer_filament]));//noly reduce speed when
                    if (!interface_solid) {
                        int tmp_id = std::find_if(m_wipe_tower_blocks.begin(), m_wipe_tower_blocks.end(), [&](const WipeTowerBlock &b) { return &b == &block; }) -
                                    m_wipe_tower_blocks.begin();
                        next_solid_blocks_id.insert(tmp_id);
                    }
                    finish_block_tcr = finish_block_solid(block, finish_layer_filament, layer.extruder_fill, interface_solid);
                    block.finish_depth[m_cur_layer_id] = block.start_depth + block.depth;
                }
                else {
                    finish_block_tcr = finish_block(block, finish_layer_filament, layer.extruder_fill);
                    block.finish_depth[m_cur_layer_id] = block.cur_depth;
                }

                bool has_inserted = false;
                {
                    auto fc_iter = std::find_if(layer_result.begin(), layer_result.end(),
                                                [&finish_layer_filament](const WipeTower::ToolChangeResult &item) { return item.new_tool == finish_layer_filament; });
                    if (fc_iter != layer_result.end()) {
                        *fc_iter = merge_tcr(*fc_iter, finish_block_tcr);
                        has_inserted = true;
                    }
                }

                if (block.last_filament_change_id == -1 && !has_inserted) {
                    auto nc_iter = std::find_if(layer_result.begin(), layer_result.end(),
                                                [&finish_layer_filament](const WipeTower::ToolChangeResult &item) { return item.initial_tool == finish_layer_filament; });
                    if (nc_iter != layer_result.end()) {
                        *nc_iter = merge_tcr(finish_block_tcr, *nc_iter);
                        has_inserted = true;
                    }
                }

                if (!has_inserted) {
                    if (finish_block_tcr.gcode.empty())
                        finish_block_tcr = finish_block_tcr;
                    else
                        finish_layer_tcr = merge_tcr(finish_layer_tcr, finish_block_tcr);
                }
            }
        }
        // record the contact layers of different categories
        solid_blocks_id = next_solid_blocks_id;
        if (layer_result.empty()) {
            // there is nothing to merge finish_layer with
            layer_result.emplace_back(std::move(finish_layer_tcr));
        }
        else if (is_valid_gcode(finish_layer_tcr.gcode)) {
            if (insert_finish_layer_idx == -1)
                layer_result[0] = merge_tcr(finish_layer_tcr, layer_result[0]);
            else
                layer_result[insert_finish_layer_idx] = merge_tcr(layer_result[insert_finish_layer_idx], finish_layer_tcr);
        }

        if (only_generate_wall && !timelapse_wall.gcode.empty()) {
            layer_result.insert(layer_result.begin(), std::move(timelapse_wall));
        }
        result.emplace_back(std::move(layer_result));
    }
    assert(m_outer_wall.size() == m_plan.size());
}


// Processes vector m_plan and calls respective functions to generate G-code for the wipe tower
// Resulting ToolChangeResults are appended into vector "result"
void WipeTower::generate(std::vector<std::vector<WipeTower::ToolChangeResult>> &result)
{
	if (m_plan.empty())
        return;

    m_extra_spacing = 1.f;

	plan_tower();
    // BBS
#if 0
    for (int i=0;i<5;++i) {
        save_on_last_wipe();
        plan_tower();
    }
#endif

    m_layer_info = m_plan.begin();

    // we don't know which extruder to start with - we'll set it according to the first toolchange
    for (const auto& layer : m_plan) {
        if (!layer.tool_changes.empty()) {
            m_current_tool = layer.tool_changes.front().old_tool;
            break;
        }
    }

    for (auto& used : m_used_filament_length) // reset used filament stats
        used = 0.f;

    m_old_temperature = -1; // reset last temperature written in the gcode

    std::vector<WipeTower::ToolChangeResult> layer_result;
    int index = 0;
	for (auto layer : m_plan)
	{
        m_cur_layer_id = index++;
        set_layer(layer.z, layer.height, 0, false/*layer.z == m_plan.front().z*/, layer.z == m_plan.back().z);
        // BBS
        //m_internal_rotation += 180.f;

        if (m_layer_info->depth < m_perimeter_width)
            continue;

        if (m_layer_info->depth < m_wipe_tower_depth - m_perimeter_width) {
            // align y shift to perimeter width
            float dy = m_extra_spacing * m_perimeter_width;
            m_y_shift = (m_wipe_tower_depth - m_layer_info->depth) / 2.f;
            m_y_shift = align_round(m_y_shift, dy);
        }

        // BBS: consider both soluable and support properties
        int idx = first_toolchange_to_nonsoluble_nonsupport (layer.tool_changes);
        ToolChangeResult finish_layer_tcr;
        ToolChangeResult timelapse_wall;

        if (idx == -1) {
            // if there is no toolchange switching to non-soluble, finish layer
            // will be called at the very beginning. That's the last possibility
            // where a nonsoluble tool can be.
            if (m_enable_timelapse_print) {
                timelapse_wall = only_generate_out_wall();
            }
            finish_layer_tcr = finish_layer(m_enable_timelapse_print ? false : true, layer.extruder_fill);
        }

        for (int i=0; i<int(layer.tool_changes.size()); ++i) {
            if (i == 0 && m_enable_timelapse_print) {
                timelapse_wall = only_generate_out_wall();
            }

            if (i == idx) {
                layer_result.emplace_back(tool_change(layer.tool_changes[i].new_tool, m_enable_timelapse_print ? false : true));
                // finish_layer will be called after this toolchange
                finish_layer_tcr = finish_layer(false, layer.extruder_fill);
            }
            else {
                if (idx == -1 && i == 0) {
                    layer_result.emplace_back(tool_change(layer.tool_changes[i].new_tool, false, true));
                } else {
                    layer_result.emplace_back(tool_change(layer.tool_changes[i].new_tool));
                }
            }
        }

        if (layer_result.empty()) {
            // there is nothing to merge finish_layer with
            layer_result.emplace_back(std::move(finish_layer_tcr));
        }
        else {
            if (idx == -1)
                layer_result[0] = merge_tcr(finish_layer_tcr, layer_result[0]);
            else if (is_valid_gcode(finish_layer_tcr.gcode))
                layer_result[idx] = merge_tcr(layer_result[idx], finish_layer_tcr);
        }

        if (m_enable_timelapse_print) {
            layer_result.insert(layer_result.begin(), std::move(timelapse_wall));
        }

		result.emplace_back(std::move(layer_result));
	}
}

WipeTower::ToolChangeResult WipeTower::only_generate_out_wall(bool is_new_mode)
{
    size_t old_tool = m_current_tool;

    WipeTowerWriter writer(m_layer_height, m_perimeter_width, m_gcode_flavor, m_filpar);
    writer.set_extrusion_flow(m_extrusion_flow)
        .set_z(m_z_pos)
        .set_initial_tool(m_current_tool)
        .set_y_shift(m_y_shift - (m_current_shape == SHAPE_REVERSED ? m_layer_info->toolchanges_depth() : 0.f));

    // Slow down on the 1st layer.
    bool first_layer = is_first_layer();
    // BBS: speed up perimeter speed to 90mm/s for non-first layer
    float           feedrate   = first_layer ? std::min(m_first_layer_speed * 60.f, 5400.f) : std::min(60.0f * m_filpar[m_current_tool].max_e_speed / m_extrusion_flow, 5400.f);
    float           fill_box_y = m_layer_info->toolchanges_depth() + m_perimeter_width;
    box_coordinates fill_box(Vec2f(m_perimeter_width, fill_box_y), m_wipe_tower_width - 2 * m_perimeter_width, m_layer_info->depth - fill_box_y);

    writer.set_initial_position((m_left_to_right ? fill_box.ru : fill_box.lu), // so there is never a diagonal travel
                                m_wipe_tower_width, m_wipe_tower_depth, m_internal_rotation);

    bool toolchanges_on_layer = m_layer_info->toolchanges_depth() > WT_EPSILON;

    // we are in one of the corners, travel to ld along the perimeter:
    // BBS: Delete some unnecessary travel
    //if (writer.x() > fill_box.ld.x() + EPSILON) writer.travel(fill_box.ld.x(), writer.y());
    //if (writer.y() > fill_box.ld.y() + EPSILON) writer.travel(writer.x(), fill_box.ld.y());
    writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_Start) + "\n");
    // outer perimeter (always):
    // BBS

    float wipe_tower_depth = m_layer_info->depth + m_perimeter_width;
    if (is_new_mode && (m_enable_timelapse_print || m_enable_wrapping_detection))
        wipe_tower_depth = m_wipe_tower_depth;
    box_coordinates wt_box(Vec2f(0.f, (m_current_shape == SHAPE_REVERSED ? m_layer_info->toolchanges_depth() : 0.f)), m_wipe_tower_width, wipe_tower_depth);
    wt_box = align_perimeter(wt_box);
    Polygon outer_wall;
    //if (m_use_gap_wall)
    //    generate_support_wall(writer, wt_box, feedrate, first_layer);
    //else
    //    writer.rectangle(wt_box, feedrate);
    outer_wall = generate_support_wall_new(writer, wt_box, feedrate, first_layer, m_use_rib_wall, true, m_use_gap_wall);
    m_outer_wall[m_z_pos].push_back(to_polyline(outer_wall));
    // Now prepare future wipe. box contains rectangle that was extruded last (ccw).

    // Vec2f target = (writer.pos() == wt_box.ld ? wt_box.rd : (writer.pos() == wt_box.rd ? wt_box.ru : (writer.pos() == wt_box.ru ? wt_box.lu : wt_box.ld)));
    //writer.add_wipe_point(writer.pos()).add_wipe_point(target);

    writer.add_wipe_path(outer_wall, m_filpar[m_current_tool].wipe_dist);
    writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_End) + "\n");

    // Ask our writer about how much material was consumed.
    // Skip this in case the layer is sparse and config option to not print sparse layers is enabled.
    if (!m_no_sparse_layers || toolchanges_on_layer)
        if (m_current_tool < m_used_filament_length.size()) m_used_filament_length[m_current_tool] += writer.get_and_reset_used_filament_length();

    return construct_tcr(writer, false, old_tool, true, false, 0.f);
}

Polygon WipeTower::generate_rib_polygon(const box_coordinates &wt_box)
{
    auto    get_current_layer_rib_len = [](float cur_height, float max_height, float max_len) -> float { return std::abs(max_height - cur_height) / max_height * max_len; };
    coord_t diagonal_width            = scaled(m_rib_width)/2;
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

    Polygons           p_1_2    = union_({poly_1, poly_2, poly});
    //Polygon            res_poly = p_1_2.front();
    //for (auto &p : res_poly.points) res.push_back(unscale(p).cast<float>());
    /*if (p_1_2.front().points.size() != 16)
        std::cout << "error " << std::endl;*/
    return p_1_2.front();
};

Polygon WipeTower::generate_support_wall_new(WipeTowerWriter &writer, const box_coordinates &wt_box, double feedrate, bool first_layer,bool rib_wall, bool extrude_perimeter, bool skip_points)
{
    auto get_closet_idx = [this, &writer](Polylines &pls) -> std::pair<int,int> {
        Vec2f anchor{writer.x(), writer.y()};
        int   closestIndex = -1;
        int   closestPl = -1;
        float minDistance  = std::numeric_limits<float>::max();
        for (int i = 0; i < pls.size(); ++i) {
            for (int j = 0; j < pls[i].size(); ++j) {
                float distance = (unscaled<float>(pls[i][j]) - anchor).squaredNorm();
                if (distance < minDistance) {
                    minDistance  = distance;
                    closestPl    = i;
                    closestIndex = j;
                }
            }
        }
        return {closestPl, closestIndex};
    };

    float retract_length = m_filpar[m_current_tool].retract_length;
    float retract_speed  = m_filpar[m_current_tool].retract_speed * 60;
    Polygon wall_polygon   = rib_wall ? generate_rib_polygon(wt_box) : generate_rectange_polygon(wt_box.ld, wt_box.ru);
    Polylines result_wall;
    Polygon   insert_skip_polygon;
    if (m_used_fillet) {
        if (!rib_wall && m_y_shift > EPSILON)// do nothing because the fillet will cause it to be suspended.
        {
        } else {
            wall_polygon           = rib_wall ? rounding_polygon(wall_polygon) : wall_polygon; // rectangle_wall do nothing
            Polygon wt_box_polygon = generate_rectange_polygon(wt_box.ld, wt_box.ru);
            wall_polygon           = union_({wall_polygon, wt_box_polygon}).front();
        }
    }
    if (!extrude_perimeter) return wall_polygon;

    if (skip_points) { result_wall = contrust_gap_for_skip_points(wall_polygon,m_wall_skip_points,m_wipe_tower_width,2.5*m_perimeter_width,insert_skip_polygon); }
    else {
        result_wall.push_back(to_polyline(wall_polygon));
        insert_skip_polygon = wall_polygon;
    }
    writer.generate_path(result_wall, feedrate, retract_length, retract_speed,m_used_fillet);
    if (m_cur_layer_id == 0) {
        BoundingBox bbox = get_extents(result_wall);
        m_rib_offset     = Vec2f(-unscaled<float>(bbox.min.x()), -unscaled<float>(bbox.min.y()));
    }

    return insert_skip_polygon;
}

Polygon WipeTower::generate_support_wall(WipeTowerWriter &writer, const box_coordinates &wt_box, double feedrate, bool first_layer)
{
    float retract_length = m_filpar[m_current_tool].retract_length;
    float retract_speed  = m_filpar[m_current_tool].retract_speed *60 ;
    bool is_left  = false;
    bool is_right = false;
    for (auto pt : m_wall_skip_points) {
        if (abs(pt.x()) < EPSILON) {
            is_left = true;
        } else if (abs(pt.x() - m_wipe_tower_width) < EPSILON) {
            is_right = true;
        }
    }

    if (is_left && is_right) {
        Vec2f *p = nullptr;
        p->x();
    }

    if (!is_left && !is_right) {
        Vec2f *p = nullptr;
        p->x();
    }

    //  3 -------------  2
    //    |           |
    //    |           |
    //  0 -------------  1

    int   index   = 0;
    Vec2f cur_pos = writer.pos();
    if (abs(cur_pos.x() - wt_box.ld.x()) > abs(cur_pos.x() - wt_box.rd.x())) {
        if (abs(cur_pos.y() - wt_box.ld.y()) > abs(cur_pos.y() - wt_box.lu.y())) {
            index = 2;
        } else {
            index = 1;
        }
    } else {
        if (abs(cur_pos.y() - wt_box.ld.y()) > abs(cur_pos.y() - wt_box.lu.y())) {
            index = 3;
        } else {
            index = 0;
        }
    }

    std::vector<Vec2f> points;
    points.emplace_back(wt_box.ld);
    points.emplace_back(wt_box.rd);
    points.emplace_back(wt_box.ru);
    points.emplace_back(wt_box.lu);

    writer.travel(points[index]);
    int extruded_nums = 0;
    while (extruded_nums < 4) {
        index = (index + 1) % 4;
        if (index == 2) {
            if (is_right) {
                std::vector<Segment> break_segments = remove_points_from_segment(Segment(wt_box.rd, wt_box.ru), m_wall_skip_points, 2.5 * m_perimeter_width);
                for (auto iter = break_segments.begin(); iter != break_segments.end(); ++iter) {
                    float dx  = iter->start.x() - writer.pos().x();
                    float dy  = iter->start.y() - writer.pos().y();
                    float len = std::sqrt(dx * dx + dy * dy);
                    if (len > 0) {
                        writer.retract(retract_length, retract_speed);
                        writer.travel(iter->start, 600.);
                        writer.retract(-retract_length, retract_speed);
                    } else
                        writer.travel(iter->start, 600.);

                    writer.extrude(iter->end, feedrate);
                }
                writer.travel(wt_box.ru, feedrate);
            } else {
                writer.extrude(wt_box.ru, feedrate);
            }
        } else if (index == 0) {
            if (is_left) {
                std::vector<Segment> break_segments = remove_points_from_segment(Segment(wt_box.ld, wt_box.lu), m_wall_skip_points, 2.5 * m_perimeter_width);
                for (auto iter = break_segments.rbegin(); iter != break_segments.rend(); ++iter) {
                    float dx  = iter->end.x() - writer.pos().x();
                    float dy  = iter->end.y() - writer.pos().y();
                    float len = std::sqrt(dx * dx + dy * dy);
                    if (len > 0) {
                        writer.retract(retract_length, retract_speed);
                        writer.travel(iter->end, 600.);
                        writer.retract(-retract_length, retract_speed);
                    } else
                        writer.travel(iter->end, 600.);

                    writer.extrude(iter->start, feedrate);
                }
                writer.travel(wt_box.ld, feedrate);
            } else {
                writer.extrude(wt_box.ld, feedrate);
            }
        } else {
            writer.extrude(points[index], feedrate);
        }
        extruded_nums++;
    }

    return Polygon();
}


bool WipeTower::get_floating_area(float &start_pos_y, float &end_pos_y) const {
    if (m_layer_info == m_plan.begin() || (m_layer_info - 1) == m_plan.begin())
        return false;

    if (!m_cur_block)
        return false;

    end_pos_y = m_cur_block->start_depth + m_cur_block->depth - m_perimeter_width;
    start_pos_y = m_cur_block->finish_depth[m_cur_layer_id - 1];

#if 0
    float last_layer_fill_box_y = (m_layer_info - 1)->toolchanges_depth() + m_perimeter_width;
    float last_layer_wipe_depth = (m_layer_info - 1)->depth;
    if (last_layer_wipe_depth - last_layer_fill_box_y <= 2 * m_perimeter_width)
        return false;

    start_pos_y = last_layer_fill_box_y + m_perimeter_width;
    end_pos_y   = last_layer_wipe_depth - m_perimeter_width;
#endif
    return true;
}

bool WipeTower::need_thick_bridge_flow(float pos_y) const {
    if (m_layer_height >= 0.2)
        return false;

    float y_min = 0., y_max = 0.;
    if (get_floating_area(y_min, y_max)) {
        return pos_y > y_min && pos_y < y_max;
    }
    return false;
}

} // namespace Slic3r
