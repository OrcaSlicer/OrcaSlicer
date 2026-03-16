#include "../ClipperUtils.hpp"
#include "../Clipper2Utils.hpp"
#include "../ClipperZUtils.hpp"
#include "../ExPolygon.hpp"
#include "../Surface.hpp"
#include "../VariableWidth.hpp"
#include "FillFloatingConcentric.hpp"
#include <boost/log/trivial.hpp>
#include "../EdgeGrid.hpp"

namespace Slic3r {

using ZPath  = ClipperLib_Z::Path;
using ZPaths = ClipperLib_Z::Paths;

FloatingPolyline FloatingPolyline::rebase_at(size_t idx)
{
    if (!this->is_closed())
       return  {};

    FloatingPolyline ret = *this;
    static_cast<Polyline&>(ret) = Polyline::rebase_at(idx);
    size_t n = this->points.size();
    ret.is_floating.resize(n);
    for (size_t j = 0; j < n - 1; ++j) {
        ret.is_floating[j] = this->is_floating[(idx + j) % (n-1)];
    }
    ret.is_floating.emplace_back(ret.is_floating.front());
    return ret;
}

FloatingThickPolyline FloatingThickPolyline::rebase_at(size_t idx)
{
    if (!this->is_closed())
        return {};
    FloatingThickPolyline ret = *this;
    static_cast<ThickPolyline&>(ret) = ThickPolyline::rebase_at(idx);
    size_t n = this->points.size();
    ret.is_floating.resize(n);
    for (size_t j = 0; j < n - 1; ++j) {
        ret.is_floating[j] = this->is_floating[(idx + j) % (n - 1)];
    }
    ret.is_floating.emplace_back(ret.is_floating.front());
    return ret;
}

FloatingThicklines FloatingThickPolyline::floating_thicklines() const
{
    FloatingThicklines lines;
    if (this->points.size() >= 2) {
        lines.reserve(this->points.size() - 1);
        for (size_t i = 0; i + 1 < this->points.size(); ++i)
            lines.emplace_back(this->points[i], this->points[i + 1], this->width[2 * i], this->width[2 * i + 1], this->is_floating[i], this->is_floating[i + 1]);
    }
    return lines;
}


//BBS: new function to filter width to avoid too fragmented segments
static ExtrusionPaths floating_thick_polyline_to_extrusion_paths(const FloatingThickPolyline& floating_polyline, ExtrusionRole role, const Flow& flow, const float tolerance)
{
    ExtrusionPaths paths;
    ExtrusionPath path(role);
    FloatingThicklines lines = floating_polyline.floating_thicklines();

    size_t start_index = 0;
    double max_width, min_width;

    auto set_flow_for_path = [&flow](ExtrusionPath& path, double width) {
        Flow new_flow = flow.with_width(unscale<float>(width) + flow.height() * float(1. - 0.25 * PI));
        path.mm3_per_mm = new_flow.mm3_per_mm();
        path.width = new_flow.width();
        path.height = new_flow.height();
        };

    auto append_path_and_reset = [set_flow_for_path, role, &paths](double& length, double& sum, ExtrusionPath& path){
            length = sum = 0;
            paths.emplace_back(std::move(path));
            path = ExtrusionPath(role);
        };

    for (int i = 0; i < (int)lines.size(); ++i) {
        const FloatingThickline& line = lines[i];

        if (i == 0) {
            max_width = min_width = line.a_width;
        }
    
        const coordf_t line_len = line.length();
        if (line_len < SCALED_EPSILON) continue;

        double thickness_delta = std::max(fabs(max_width - line.b_width), fabs(min_width - line.b_width));
        //BBS: has large difference in width
        if (thickness_delta > tolerance) {
            //BBS: 1 generate path from start_index to i(not included)
            if (start_index != i) {
                path = ExtrusionPath(role);
                double length = 0, sum = 0;
                bool is_floating = false;
                for (int idx = start_index; idx < i; idx++) {
                    bool curr_floating = lines[idx].is_a_floating && lines[idx].is_b_floating;
                    if (curr_floating != is_floating && length != 0) {
                        path.polyline.append(lines[idx].a);
                        if (is_floating)
                            path.set_customize_flag(CustomizeFlag::cfFloatingVerticalShell);
                        set_flow_for_path(path, sum / length);
                        append_path_and_reset(length, sum, path);
                    }
                    is_floating = curr_floating;

                    double line_length = lines[idx].length();
                    length += line_length;
                    sum += line_length * (lines[idx].a_width + lines[idx].b_width) * 0.5;
                    path.polyline.append(lines[idx].a);
                }
                path.polyline.append(lines[i].a);
                if (length > SCALED_EPSILON) {
                    if (lines[i].is_a_floating && lines[i].is_b_floating)
                        path.set_customize_flag(CustomizeFlag::cfFloatingVerticalShell);
                    set_flow_for_path(path, sum / length);
                    paths.emplace_back(std::move(path));
                }
            }

            start_index = i;
            max_width = line.a_width;
            min_width = line.a_width;

            //BBS: 2 handle the i-th segment
            thickness_delta = fabs(line.a_width - line.b_width);
            if (thickness_delta > tolerance) {
                const unsigned int segments = (unsigned int)ceil(thickness_delta / tolerance);
                const coordf_t seg_len = line_len / segments;
                Points pp;
                std::vector<coordf_t> width;
                {
                    pp.push_back(line.a);
                    width.push_back(line.a_width);
                    for (size_t j = 1; j < segments; ++j) {
                        pp.push_back((line.a.cast<double>() + (line.b - line.a).cast<double>().normalized() * (j * seg_len)).cast<coord_t>());

                        coordf_t w = line.a_width + (j * seg_len) * (line.b_width - line.a_width) / line_len;
                        width.push_back(w);
                        width.push_back(w);
                    }
                    pp.push_back(line.b);
                    width.push_back(line.b_width);

                    assert(pp.size() == segments + 1u);
                    assert(width.size() == segments * 2);
                }

                // delete this line and insert new ones
                lines.erase(lines.begin() + i);
                for (size_t j = 0; j < segments; ++j) {
                    FloatingThickline new_line(pp[j], pp[j + 1],width[2 * j],width[2 * j+1], line.is_a_floating,line.is_b_floating);
                    lines.insert(lines.begin() + i + j, new_line);
                }
                --i;
                continue;
            }
        }
        //BBS: just update the max and min width and continue
        else {
            max_width = std::max(max_width, std::max(line.a_width, line.b_width));
            min_width = std::min(min_width, std::min(line.a_width, line.b_width));
        }
    }
    //BBS: handle the remaining segment
    size_t final_size = lines.size();
    if (start_index < final_size) {
        path = ExtrusionPath(role);
        double length = 0, sum = 0;
        bool is_floating = false;
        for (int idx = start_index; idx < final_size; idx++) {
            bool curr_floating = lines[idx].is_a_floating && lines[idx].is_b_floating;
            if (curr_floating!= is_floating && length != 0) {
                path.polyline.append(lines[idx].a);
                if(is_floating)
                    path.set_customize_flag(CustomizeFlag::cfFloatingVerticalShell);
                set_flow_for_path(path, sum / length);
                append_path_and_reset(length, sum, path);
            }
            is_floating = curr_floating;
            double line_length = lines[idx].length();
            length += line_length;
            sum += line_length * (lines[idx].a_width + lines[idx].b_width) * 0.5;
            path.polyline.append(lines[idx].a);

        }
        path.polyline.append(lines[final_size - 1].b);
        if (length > SCALED_EPSILON) {
            if (lines[final_size - 1].is_a_floating && lines[final_size - 1].is_b_floating)
                path.set_customize_flag(CustomizeFlag::cfFloatingVerticalShell);
            set_flow_for_path(path, sum / length);
            paths.emplace_back(std::move(path));
        }
    }

    return paths;
}

double interpolate_width(const ZPath& path,
    const ThickPolyline& line,
    const int subject_idx_range,
    const int default_width,
    size_t idx)
{
    int prev_idx = idx;
    while (prev_idx >= 0 && (path[prev_idx].z() < 0 || path[prev_idx].z() >= subject_idx_range))
        --prev_idx;

    int next_idx = idx;
    while (next_idx < path.size() && (path[next_idx].z() < 0 || path[next_idx].z() >= subject_idx_range))
        ++next_idx;

    double width_prev;
    double width_next;
    if (prev_idx < 0) {
        width_prev = default_width;
    }
    else {
        size_t prev_z_idx = path[prev_idx].z();
        width_prev = line.get_width_at(prev_z_idx);
    }

    if (next_idx >= path.size()) {
        width_next = default_width;
    }
    else {
        size_t next_z_idx = path[next_idx].z();
        width_next = line.get_width_at(next_z_idx);
    }
    Point prev(path[prev_idx].x(), path[prev_idx].y());
    Point next(path[next_idx].x(), path[next_idx].y());
    Point curr(path[idx].x(), path[idx].y());
    double d_total = (next - prev).cast<double>().norm();
    double d_curr = (curr - prev).cast<double>().norm();
    double t = (d_total > 0) ? (d_curr / d_total) : 0.0;
    return (1 - t) * width_prev + t * width_next;
}

FloatingThickPolyline merge_lines(ZPaths lines, const std::vector<bool>& mark_flags, const ThickPolyline& line, const int subject_idx_range ,const int default_width)
{
    using PathFlag = std::vector<bool>;
    using PathFlags = std::vector<PathFlag>;

    std::vector<bool>used(lines.size(), false);
    ZPaths merged_paths;
    PathFlags merged_marks;

    auto update_path_flag = [](PathFlag& mark_flags, const ZPath& path, bool mark) {
        for (auto p : path)
            mark_flags.emplace_back(mark);
        };

    std::unordered_map<int64_t, std::unordered_set<size_t>> start_z_map;
    std::unordered_map<int64_t, std::unordered_set<size_t>> end_z_map;

    for (size_t idx = 0; idx < lines.size(); ++idx) {
        if (lines[idx].empty()) {
            used[idx] = true;
            continue;
        }
        start_z_map[lines[idx].front().z()].insert(idx);
        end_z_map[lines[idx].back().z()].insert(idx);
    }

    auto remove_from_map = [&start_z_map, &end_z_map, &lines](size_t idx) {
        if (lines[idx].empty())
            return;
        int64_t start_z = lines[idx].front().z();
        int64_t end_z = lines[idx].back().z();
        start_z_map[start_z].erase(idx);
        if (start_z_map[start_z].empty())
            start_z_map.erase(start_z);
        end_z_map[end_z].erase(idx);
        if (end_z_map[end_z].empty())
            end_z_map.erase(end_z);
        };

    for (size_t idx = 0; idx < lines.size(); ++idx) {
        if (used[idx])
            continue;
        ZPath curr_path = lines[idx];
        PathFlag curr_mark;
        update_path_flag(curr_mark, curr_path, mark_flags[idx]);
        used[idx] = true;
        remove_from_map(idx);

        bool merged;
        do {
            merged = false;
            int64_t curr_end = curr_path.back().z();
            int64_t curr_start = curr_path.front().z();

            // search after
            {
                if (auto start_iter = start_z_map.find(curr_end);start_iter != start_z_map.end()) {
                    size_t j = *start_iter->second.begin();
                    remove_from_map(j);
                    curr_path.insert(curr_path.end(), lines[j].begin(), lines[j].end());
                    update_path_flag(curr_mark, lines[j], mark_flags[j]);
                    used[j] = true;
                    merged = true;
                }
                else if (auto end_iter = end_z_map.find(curr_end); end_iter != end_z_map.end()) {
                    size_t j = *end_iter->second.begin();
                    remove_from_map(j);
                    std::reverse(lines[j].begin(), lines[j].end());
                    curr_path.insert(curr_path.end(), lines[j].begin(), lines[j].end());
                    update_path_flag(curr_mark, lines[j], mark_flags[j]);
                    used[j] = true;
                    merged = true;
                }
            }

            if (merged)
                continue;

            //search before
            {
                if (auto end_iter = end_z_map.find(curr_start);end_iter != end_z_map.end()) {
                    size_t j = *end_iter->second.begin();
                    remove_from_map(j);
                    ZPath new_path = lines[j];
                    PathFlag new_mark;
                    update_path_flag(new_mark, new_path, mark_flags[j]);

                    new_path.insert(new_path.end(), curr_path.begin(), curr_path.end());
                    new_mark.insert(new_mark.end(), curr_mark.begin(), curr_mark.end());
                    curr_path = std::move(new_path);
                    curr_mark = std::move(new_mark);
                    used[j] = true;
                    merged = true;
                }
                else if (auto start_iter = start_z_map.find(curr_start); start_iter != start_z_map.end()) {
                    size_t j = *start_iter->second.begin();
                    remove_from_map(j);
                    ZPath new_path = lines[j];
                    std::reverse(new_path.begin(), new_path.end());
                    PathFlag new_mark;
                    update_path_flag(new_mark, new_path, mark_flags[j]);

                    new_path.insert(new_path.end(), curr_path.begin(), curr_path.end());
                    new_mark.insert(new_mark.end(), curr_mark.begin(), curr_mark.end());
                    curr_path = std::move(new_path);
                    curr_mark = std::move(new_mark);
                    used[j] = true;
                    merged = true;
                }
            }

        } while (merged);

        merged_paths.emplace_back(curr_path);
        merged_marks.emplace_back(curr_mark);
    }

    assert(merged_marks.size() == 1);

    FloatingThickPolyline res;

    auto& valid_path = merged_paths.front();
    auto& valid_mark = merged_marks.front();

    for (size_t idx = 0; idx < valid_path.size(); ++idx) {
        int zvalue = valid_path[idx].z();
        res.points.emplace_back(valid_path[idx].x(), valid_path[idx].y());
        res.is_floating.emplace_back(valid_mark[idx]);
        if (0 <= zvalue && zvalue < subject_idx_range) {
            res.width.emplace_back(line.get_width_at(prev_idx_modulo(zvalue, line.points)));
            res.width.emplace_back(line.get_width_at(zvalue));
        }
        else {
            double width = interpolate_width(valid_path, line, subject_idx_range, default_width, idx);
            res.width.emplace_back(width);
            res.width.emplace_back(width);
        }
    }
    res.width = std::vector<coordf_t>(res.width.begin() + 1, res.width.end()-1);
    assert(res.width.size() == 2 * res.points.size() - 2);

    return res;
}

FloatingThickPolyline detect_floating_line(const ThickPolyline& line, const ExPolygons& floating_areas, const double default_width ,bool force_no_detect)
{
    {
        Polyline polyline = line;
        auto bbox_line = get_extents(polyline);
        auto bbox_area = get_extents(floating_areas);
        if (force_no_detect || !bbox_area.overlap(bbox_line) || intersection_pl(polyline, floating_areas).empty()) {
            FloatingThickPolyline res;
            res.width = line.width;
            res.points = line.points;
            res.is_floating.resize(res.points.size(), false);
            return res;
        }
    }

    using ZPoint = ClipperLib_Z::IntPoint;
    auto hash_function = [](const int a1, const int b1, const int a2, const int b2)->int32_t {
        int32_t hash_val = 1000 * (a1 * 13 + b1) + (a2 * 17 + b2) + 1;
        hash_val &= 0x7fffffff;
        return hash_val;
        };

    int idx = 0;
    int subject_idx_range;
    ZPaths subject_paths;
    {
        subject_paths.emplace_back();
        for (auto p : line)
            subject_paths.back().emplace_back(p.x(), p.y(), idx++);
    }

    subject_idx_range = idx;
    ZPaths clip_paths;
    {
        Polygons floating_polygons = to_polygons(floating_areas);
        for (auto& poly : floating_polygons) {
            clip_paths.emplace_back();
            for (const auto& p : poly)
                clip_paths.back().emplace_back(p.x(), p.y(), idx++);
        }
    }

    ClipperLib_Z::ZFillCallback z_filler = [hash_function, subject_idx_range](const ZPoint& e1_a, const ZPoint& e1_b, const ZPoint& e2_a, const ZPoint& e2_b, ZPoint& d) {
        if (e1_a.z() < subject_idx_range && e1_b.z() < subject_idx_range && e2_a.z() < subject_idx_range && e2_b.z() < subject_idx_range) {
            BOOST_LOG_TRIVIAL(error) << Slic3r::format("ZFiller: both point in subject : %d, %d, %d, %d ", e1_a.z(), e1_b.z(), e2_a.z(), e2_b.z());
        }
        if (e1_a.z() >= subject_idx_range && e1_b.z() >= subject_idx_range && e2_a.z() >= subject_idx_range && e2_b.z() >= subject_idx_range) {
            BOOST_LOG_TRIVIAL(error) << Slic3r::format("ZFiller: both point in clip : %d, %d, %d, %d ", e1_a.z(), e1_b.z(), e2_a.z(), e2_b.z());
        }
        if (e1_a.z() < 0 || e1_b.z() < 0 || e2_a.z() < 0 || e2_b.z() < 0)
            BOOST_LOG_TRIVIAL(error) << Slic3r::format("ZFiller: Encounter negative z : %d, %d, %d, %d ", e1_a.z(), e1_b.z(), e2_a.z(), e2_b.z());
        if (e1_a.z() == e1_b.z() || e2_a.z() == e2_b.z()) {
            BOOST_LOG_TRIVIAL(error) << Slic3r::format("ZFiller: Encounter same z in one line : %d, %d, %d, %d ", e1_a.z(), e1_b.z(), e2_a.z(), e2_b.z());
        }
        if (e1_a.z() == e1_b.z() && e1_b.z() == e2_a.z() && e2_a.z() == e2_b.z()) {
            BOOST_LOG_TRIVIAL(error) << Slic3r::format("ZFiller: Encounter same z in both line : %d, %d, %d, %d ", e1_a.z(), e1_b.z(), e2_a.z(), e2_b.z());
            // the intersect is generated by two lines in subject
            d.z() = e1_a.z();
            return;
        }   // the intersect is generate by two line from subject and clip
        d.z() = -hash_function(e1_a.z(), e1_b.z(), e2_a.z(), e2_b.z());
        if (d.z() >= 0)
            BOOST_LOG_TRIVIAL(error) << Slic3r::format("ZFiller: hash function generate postive value : %d", d.z());
    };


    ZPaths intersect_out;
    {
        ClipperLib_Z::Clipper c;
        ClipperLib_Z::PolyTree polytree;
        c.ZFillFunction(z_filler);
        c.AddPaths(subject_paths, ClipperLib_Z::ptSubject, false);
        c.AddPaths(clip_paths, ClipperLib_Z::ptClip, true);
        c.Execute(ClipperLib_Z::ctIntersection, polytree, ClipperLib_Z::pftNonZero);
        ClipperLib_Z::PolyTreeToPaths(std::move(polytree), intersect_out);
    }

    ZPaths diff_out;
    {
        ClipperLib_Z::Clipper c;
        ClipperLib_Z::PolyTree polytree;
        c.ZFillFunction(z_filler);
        c.AddPaths(subject_paths, ClipperLib_Z::ptSubject, false);
        c.AddPaths(clip_paths, ClipperLib_Z::ptClip, true);
        c.Execute(ClipperLib_Z::ctDifference, polytree, ClipperLib_Z::pftNonZero);
        ClipperLib_Z::PolyTreeToPaths(std::move(polytree), diff_out);
    }


    ZPaths  to_merge = diff_out;
    to_merge.insert(to_merge.end(), intersect_out.begin(), intersect_out.end());
    std::vector<bool>floating_flags(to_merge.size(), false);
    for (size_t idx = diff_out.size(); idx < diff_out.size() + intersect_out.size(); ++idx)
        floating_flags[idx] = true;

    for (size_t idx = 0; idx < to_merge.size(); ++idx) {
        for (auto iter = to_merge[idx].begin(); iter != to_merge[idx].end();++iter) {
            if (iter->z() >= subject_idx_range) {
                BOOST_LOG_TRIVIAL(error) << Slic3r::format("ZFiller: encounter idx from clip: %d",iter->z());
            }
        }
    }

    return merge_lines(to_merge,floating_flags,line,subject_idx_range,default_width);
}

int start_none_floating_idx(int idx, const std::vector<int>& none_floating_count)
{
    int backtrace_idx = idx - none_floating_count[idx] + 1;
    if (backtrace_idx >= 0)
        return backtrace_idx;
    else
        return none_floating_count.size() + backtrace_idx;
}

template<typename PointContainer>
void get_none_floating_prefix(const PointContainer& container, const ExPolygons& floating_areas, const Polygons& sparse_polys, std::vector<double>& none_floating_length, std::vector<int>& none_floating_count)
{
    std::vector<double>(container.points.size(), 0).swap(none_floating_length);
    std::vector<int>(container.points.size(), 0).swap(none_floating_count);

    std::vector<BoundingBox> floating_bboxs;
    for (size_t idx = 0; idx < floating_areas.size(); ++idx)
        floating_bboxs.emplace_back(get_extents(floating_areas[idx]));
    std::vector<BoundingBox> sparse_bboxs;
    for (size_t idx = 0; idx < sparse_polys.size(); ++idx)
        sparse_bboxs.emplace_back(get_extents(sparse_polys[idx]));

    auto point_in_floating_area = [&floating_bboxs, &sparse_bboxs, &floating_areas, &sparse_polys](const Point& p)->bool {
        for (size_t idx = 0; idx < sparse_polys.size(); ++idx) {
            if (!sparse_bboxs[idx].contains(p))
                continue;
            if (sparse_polys[idx].contains(p))
                return false;
        }
        for (size_t idx = 0; idx < floating_areas.size(); ++idx) {
            if (!floating_bboxs[idx].contains(p))
                continue;
            if (floating_areas[idx].contains(p))
                return true;
        }

        return false;
    };

    for (size_t idx = 0; idx < container.points.size(); ++idx) {
        const Point& p = container.points[idx];
        if (!point_in_floating_area(p)) {
            if (idx == 0)
                none_floating_count[idx] = 1;
            else
                none_floating_count[idx] = none_floating_count[idx - 1] + 1;
            if (none_floating_count[idx] > 1)
                none_floating_length[idx] = none_floating_length[idx - 1] + ((Point)(container.points[prev_idx_modulo(idx, container.points)] - p)).cast<double>().norm();
            else
                none_floating_length[idx] = 0;
        }
        else {
            none_floating_length[idx] = 0;
            none_floating_count[idx] = 0;
        }
    }

    if (none_floating_count.back() > 0) {
        for (size_t idx = 0; idx < container.points.size(); ++idx) {
            if (none_floating_count[idx] == 0)
                break;
            none_floating_count[idx] = none_floating_count[prev_idx_modulo(idx, container.points)] + 1;
            none_floating_length[idx] = none_floating_length[prev_idx_modulo(idx, container.points)] + ((Point)(container.points[prev_idx_modulo(idx, container.points)] - container.points[idx])).cast<double>().norm();
        }
    }
}

template<typename PointContainer>
int get_best_loop_start(const PointContainer& container, const ExPolygons& floating_areas, const Polygons& sparse_polys) {
    std::vector<double> none_floating_length;
    std::vector<int> none_floating_count;

    BoundingBox floating_bbox = get_extents(floating_areas);
    BoundingBox poly_bbox(container.points);

    if (!poly_bbox.overlap(floating_bbox))
        return 0;

    Polygons clipped_sparse_polys = ClipperUtils::clip_clipper_polygons_with_subject_bbox(sparse_polys, poly_bbox);
    get_none_floating_prefix(container, floating_areas, clipped_sparse_polys, none_floating_length, none_floating_count);
    int best_idx = std::distance(none_floating_length.begin(), std::max_element(none_floating_length.begin(), none_floating_length.end()));
    return start_none_floating_idx(best_idx, none_floating_count);
}

template<typename PointContainer>
std::vector<int> get_loop_start_candidates(const PointContainer& container, const ExPolygons& floating_areas, const Polygons& sparse_polys)
{
    std::vector<double> none_floating_length;
    std::vector<int> none_floating_count;

    BoundingBox floating_bbox = get_extents(floating_areas);
    BoundingBox poly_bbox(container.points);
    std::vector<int> candidate_list;

    if (!poly_bbox.overlap(floating_bbox)) {
        candidate_list.resize(container.points.size());
        std::iota(candidate_list.begin(), candidate_list.end(), 0);
        return candidate_list;
    }
    Polygons clipped_sparse_polys = ClipperUtils::clip_clipper_polygons_with_subject_bbox(sparse_polys, poly_bbox);
    get_none_floating_prefix(container, floating_areas, clipped_sparse_polys, none_floating_length, none_floating_count);
    for (size_t idx = 0; idx < none_floating_length.size(); ++idx) {
        if (none_floating_length[idx] > 0)
            candidate_list.emplace_back(start_none_floating_idx(idx, none_floating_count));
    }
    return candidate_list;
}


void smooth_floating_line(FloatingThickPolyline& line,coord_t max_gap_threshold, coord_t min_floating_threshold)
{
    if (line.empty())
        return;
    struct LineParts {
        int start;
        int end;
        bool is_floating;
    };

    auto build_line_parts = [&](const FloatingThickPolyline& line)->std::vector<LineParts> {
        std::vector<LineParts> line_parts;
        bool current_val = line.is_floating.front();
        int start = 0;
        for (size_t idx = 1; idx < line.is_floating.size(); ++idx) {
            if (line.is_floating[idx] != current_val) {
                line_parts.push_back({ start,(int)(idx - 1),current_val });
                current_val = line.is_floating[idx];
                start = idx;
            }
        }
        line_parts.push_back({ start,(int)(line.is_floating.size() - 1),current_val });
        return line_parts;
    };

    std::vector<double> distance_prefix(line.points.size(),0);
    for (size_t idx = 0; idx < line.points.size();++idx) {
        if (idx == 0)
            distance_prefix[idx] = 0;
        else {
            distance_prefix[idx] = distance_prefix[idx - 1] + (line.points[idx] - line.points[idx - 1]).cast<double>().norm();
        }
    }
    {
        // remove too small gaps
        std::vector<LineParts> line_parts = build_line_parts(line);
        std::vector<std::pair<int, int>> gaps_to_merge;

        for (size_t i = 1; i + 1 < line_parts.size(); ++i) {
            const auto& curr = line_parts[i];
            if (!curr.is_floating) {
                const auto& prev = line_parts[i - 1];
                const auto& next = line_parts[i + 1];
                if (prev.is_floating && next.is_floating) {
                    double total_length = distance_prefix[next.start] - distance_prefix[prev.end];
                    if (total_length < max_gap_threshold) {
                        gaps_to_merge.emplace_back(curr.start, curr.end);
                    }
                }
            }
        }

        for (const auto& gap : gaps_to_merge) {
            for (int i = gap.first; i <= gap.second; ++i) {
                line.is_floating[i] = true;
            }
        }
    }

    {
        std::vector<LineParts> line_parts = build_line_parts(line);
        std::vector<std::pair<int, int>> segments_to_remove;

        for (auto& part : line_parts) {
            if (part.is_floating && distance_prefix[part.end] - distance_prefix[part.start] < min_floating_threshold) {
                segments_to_remove.emplace_back(part.start, part.end);
            }
        }

        for (const auto& seg : segments_to_remove) {
            for (int i = seg.first; i <= seg.second; ++i) {
                line.is_floating[i] = false;
            }
        }
    }
}

// nearest neibour排序，但是取点时，只能取get_loop_start_candidates得到的点
FloatingThickPolylines FillFloatingConcentric::resplit_order_loops(Point curr_point, std::vector<const Arachne::ExtrusionLine*> all_extrusions, const ExPolygons& floating_areas, const Polygons& sparse_polys, const coord_t default_width)
{
    FloatingThickPolylines result;

    for (size_t idx = 0; idx < all_extrusions.size(); ++idx) {
        if (all_extrusions[idx]->empty())
            continue;
        ThickPolyline thick_polyline = Arachne::to_thick_polyline(*all_extrusions[idx]);
        bool is_self_intersect = false;
        if(print_object_config->detect_floating_vertical_shell.value)
        {
            Polyline polyline = thick_polyline;
            auto bbox_line = get_extents(polyline);

            EdgeGrid::Grid grid;
            grid.set_bbox(bbox_line);
            grid.create({ polyline.points }, scaled<coord_t>(10.), !all_extrusions[idx]->is_closed);
            if (grid.has_intersecting_edges())
                is_self_intersect = true;
        }
        FloatingThickPolyline thick_line_with_floating = detect_floating_line(thick_polyline, floating_areas, default_width, !print_object_config->detect_floating_vertical_shell.value || is_self_intersect);
        smooth_floating_line(thick_line_with_floating, scale_(2), scale_(2));
        int split_idx = 0;
        if (!floating_areas.empty() && all_extrusions[idx]->is_closed && thick_line_with_floating.points.front() == thick_line_with_floating.points.back()) {
            if (idx == 0)
                split_idx = get_best_loop_start(thick_line_with_floating, floating_areas,sparse_polys);
            else {
                auto candidates = get_loop_start_candidates(thick_line_with_floating, floating_areas,sparse_polys);
                double min_dist = std::numeric_limits<double>::max();
                for (auto candidate : candidates) {
                    double dist = (curr_point - thick_line_with_floating.points[candidate]).cast<double>().norm();
                    if (min_dist > dist) {
                        min_dist = dist;
                        split_idx = candidate;
                    }
                }
            }
            FloatingThickPolyline new_line = thick_line_with_floating.rebase_at(split_idx);
            assert(new_line.width.size() == 2 * new_line.points.size() - 2);
            result.emplace_back(thick_line_with_floating.rebase_at(split_idx));
        }
        else {
            assert(thick_line_with_floating.width.size() == 2 * thick_line_with_floating.points.size() - 2);
            result.emplace_back(thick_line_with_floating);
        }
        curr_point = result.back().last_point();
    }
    return result;
}

#if 0
Polylines FillFloatingConcentric::resplit_order_loops(Point curr_point, Polygons loops, const ExPolygons& floating_areas)
{
    Polylines result;
    for (size_t idx = 0; idx < loops.size(); ++idx) {
        const Polygon& loop = loops[idx];
        int split_idx = 0;
        if (!floating_areas.empty()) {
            if (idx == 0)
                split_idx = get_best_loop_start(loop, floating_areas);
            else {
                auto candidates = get_loop_start_candidates(loop, floating_areas);
                double min_dist = std::numeric_limits<double>::max();
                for (auto candidate : candidates) {
                    double dist = (curr_point - loop.points[candidate]).cast<double>().norm();
                    if (min_dist > dist) {
                        min_dist = dist;
                        split_idx = candidate;
                    }
                }
            }
            result.emplace_back(loop.split_at_index(split_idx));
        }
        else {
            result.emplace_back(loop.split_at_index(curr_point.nearest_point_index(loop.points)));
        }
        curr_point = result.back().last_point();
    }
    return result;
};

void FillFloatingConcentric::_fill_surface_single(
    const FillParams& params,
    unsigned int thickness_layers,
    const std::pair<float, Point>& direction,
    ExPolygon   expolygon,
    FloatingLines& polylines_out
)
{
    auto expoly_bbox = get_extents(expolygon);
    coord_t min_spacing = scale_(this->spacing);
    coord_t distance = coord_t(min_spacing / params.density);
    distance = this->_adjust_solid_spacing(expoly_bbox.size()(0), distance);
    this->spacing = unscale<double>(distance);

    Polygons loops = to_polygons(expolygon);
    ExPolygons offseted_expolys{ std::move(expolygon) };
    while (!offseted_expolys.empty()) {
        offseted_expolys = offset2_ex(offseted_expolys, -(distance + min_spacing / 2), min_spacing / 2);
        append(loops, to_polygons(offseted_expolys));
    }
    // generate paths from outermost to the inner most
    loops = union_pt_chained_outside_in(loops);

    auto reordered_polylines = resplit_order_loops({ 0,0 }, loops, lower_layer_unsupport_areas);
    size_t i = polylines_out.size();
    for (auto& polyline : reordered_polylines) {
        polylines_out.emplace_back(std::move(polyline));
    }

    size_t j = polylines_out.size();
    for (; i < polylines_out.size(); ++i) {
        polylines_out[i].clip_end(this->loop_clipping);
        if (polylines_out[i].is_valid()) {
            if (j < i)
                polylines_out[j] = std::move(polylines_out[i]);
            ++j;
        }
    }
    if (j < polylines_out.size())
        polylines_out.erase(polylines_out.begin() + j, polylines_out.end());
}
#endif

void FillFloatingConcentric::_fill_surface_single(const FillParams& params,
    unsigned int                   thickness_layers,
    const std::pair<float, Point>& direction,
    ExPolygon                      expolygon,
    FloatingThickPolylines& thick_polylines_out)
{
    Point   bbox_size = expolygon.contour.bounding_box().size();
    coord_t min_spacing = params.flow.scaled_spacing();

    coord_t                loops_count = std::max(bbox_size.x(), bbox_size.y()) / min_spacing + 1;
    Polygons               polygons = to_polygons(expolygon);

    double min_nozzle_diameter = *std::min_element(print_config->nozzle_diameter.values.begin(), print_config->nozzle_diameter.values.end());
    Arachne::WallToolPathsParams input_params;
    input_params.min_bead_width = 0.85 * min_nozzle_diameter;
    input_params.min_feature_size = 0.25 * min_nozzle_diameter;
    input_params.wall_transition_length = 0.4;
    input_params.wall_transition_angle = 10;
    input_params.wall_transition_filter_deviation = 0.25 * min_nozzle_diameter;
    input_params.wall_distribution_count = 1;

    Arachne::WallToolPaths wallToolPaths(polygons, min_spacing, min_spacing, loops_count, 0, params.layer_height, input_params);

    std::vector<Arachne::VariableWidthLines>    loops = wallToolPaths.getToolPaths();
    std::vector<const Arachne::ExtrusionLine*> all_extrusions;
    for (Arachne::VariableWidthLines& loop : loops) {
        if (loop.empty())
            continue;
        for (const Arachne::ExtrusionLine& wall : loop)
            all_extrusions.emplace_back(&wall);
    }

    // Split paths using a nearest neighbor search.
    size_t firts_poly_idx = thick_polylines_out.size();
    auto thick_polylines = resplit_order_loops({ 0,0 }, all_extrusions, this->lower_layer_unsupport_areas, this->lower_sparse_polys,min_spacing);
    append(thick_polylines_out, thick_polylines);


    // clip the paths to prevent the extruder from getting exactly on the first point of the loop
    // Keep valid paths only.
    size_t j = firts_poly_idx;
    for (size_t i = firts_poly_idx; i < thick_polylines_out.size(); ++i) {
        thick_polylines_out[i].clip_end(this->loop_clipping);
        if (thick_polylines_out[i].is_valid()) {
            if (j < i)
                thick_polylines_out[j] = std::move(thick_polylines_out[i]);
            ++j;
        }
    }
    if (j < thick_polylines_out.size())
        thick_polylines_out.erase(thick_polylines_out.begin() + int(j), thick_polylines_out.end());
}


FloatingThickPolylines FillFloatingConcentric::fill_surface_arachne_floating(const Surface* surface, const FillParams& params)
{
    // Create the infills for each of the regions.
    FloatingThickPolylines floating_thick_polylines_out;
    for (ExPolygon& expoly : no_overlap_expolygons)
        _fill_surface_single(params, surface->thickness_layers, _infill_direction(surface), std::move(expoly), floating_thick_polylines_out);
    return floating_thick_polylines_out;
}

void FillFloatingConcentric::fill_surface_extrusion(const Surface* surface, const FillParams& params, ExtrusionEntitiesPtr& out)
{
    FloatingThickPolylines floating_lines = this->fill_surface_arachne_floating(surface, params);
    if (floating_lines.empty())
        return;
    Flow new_flow = params.flow.with_spacing(this->spacing);
    double flow_mm3_per_mm = new_flow.mm3_per_mm();
    double flow_width = new_flow.width();

    ExtrusionEntityCollection* ecc = new ExtrusionEntityCollection();
    ecc->no_sort = true;
    out.push_back(ecc);
    size_t idx = ecc->entities.size();

    const float tolerance = float(scale_(0.05));
    for (const auto& line : floating_lines) { 
        ExtrusionPaths paths = floating_thick_polyline_to_extrusion_paths(line, params.extrusion_role, new_flow, tolerance);
        // Append paths to collection.
        assert(!paths.empty());
        if (!paths.empty()) {
            if (paths.front().first_point() == paths.back().last_point())
                ecc->entities.emplace_back(new ExtrusionLoop(std::move(paths)));
            else {
                for (ExtrusionPath& path : paths) {
                    assert(!path.empty());
                    ecc->entities.emplace_back(new ExtrusionPath(std::move(path)));
                }
            }
        }
    }
}


} // namespace Slic3r
