#include "BoundingBox.hpp"
#include "Polyline.hpp"
#include "Exception.hpp"
#include "ExPolygon.hpp"
#include "Line.hpp"
#include "Polygon.hpp"
#include <iostream>
#include <utility>

namespace Slic3r {

const Point& Polyline::leftmost_point() const
{
    const Point *p = &this->points.front();
    for (Points::const_iterator it = this->points.begin() + 1; it != this->points.end(); ++ it) {
        if (it->x() < p->x()) 
        	p = &(*it);
    }
    return *p;
}

Lines Polyline::lines() const
{
    Lines lines;
    if (this->points.size() >= 2) {
        lines.reserve(this->points.size() - 1);
        for (Points::const_iterator it = this->points.begin(); it != this->points.end()-1; ++it) {
            lines.push_back(Line(*it, *(it + 1)));
        }
    }
    return lines;
}

void Polyline::reverse()
{
    //BBS: reverse points
    MultiPoint::reverse();
    //BBS: reverse the fitting_result
    if (!this->fitting_result.empty()) {
        for (size_t i = 0; i < this->fitting_result.size(); i++) {
            std::swap(fitting_result[i].start_point_index, fitting_result[i].end_point_index);
            fitting_result[i].start_point_index = MultiPoint::size() - 1 - fitting_result[i].start_point_index;
            fitting_result[i].end_point_index = MultiPoint::size() - 1 - fitting_result[i].end_point_index;
            if (fitting_result[i].is_arc_move())
                fitting_result[i].reverse_arc_path();
        }
        std::reverse(this->fitting_result.begin(), this->fitting_result.end());
    }
}

// removes the given distance from the end of the polyline
void Polyline::clip_end(double distance)
{
    bool last_point_inserted = false;
    size_t remove_after_index = MultiPoint::size();
    while (distance > 0) {
        Vec2d  last_point = this->last_point().cast<double>();
        this->points.pop_back();
        remove_after_index--;
        if (this->points.empty()) {
            this->fitting_result.clear();
            return;
        }
        Vec2d  v    = this->last_point().cast<double>() - last_point;
        double lsqr = v.squaredNorm();
        if (lsqr > distance * distance) {
            this->points.emplace_back((last_point + v * (distance / sqrt(lsqr))).cast<coord_t>());
            last_point_inserted = true;
            break;
        }
        distance -= sqrt(lsqr);
    }

    //BBS: don't need to clip fitting result if it's empty
    if (fitting_result.empty())
        return;
    while (!fitting_result.empty() && fitting_result.back().start_point_index >= remove_after_index)
        fitting_result.pop_back();
    if (!fitting_result.empty()) {
        //BBS: last remaining segment is arc move, then clip the arc at last point
        if (fitting_result.back().path_type == EMovePathType::Arc_move_ccw
            || fitting_result.back().path_type == EMovePathType::Arc_move_cw) {
            if (fitting_result.back().arc_data.clip_end(this->last_point()))
                //BBS: succeed to clip arc, then update the last point
                this->points.back() = fitting_result.back().arc_data.end_point;
            else
                //BBS: Failed to clip arc, then back to linear move
                fitting_result.back().path_type = EMovePathType::Linear_move;
        }
        fitting_result.back().end_point_index = this->points.size() - 1;
    }
}

// removes the given distance from the start of the polyline
void Polyline::clip_start(double distance)
{
    this->reverse();
    this->clip_end(distance);
    if (this->points.size() >= 2)
        this->reverse();
}

void Polyline::extend_end(double distance)
{
    //BBS: append a new last point by extending the last segment by the specified length
    Vec2d v = (this->points.back() - *(this->points.end() - 2)).cast<double>().normalized();
    Point new_last_point = this->points.back() + (v * distance).cast<coord_t>();
    this->append(new_last_point);
}

void Polyline::extend_start(double distance)
{
    this->reverse();
    this->extend_end(distance);
    this->reverse();
}

/* this method returns a collection of points picked on the polygon contour
   so that they are evenly spaced according to the input distance */
Points Polyline::equally_spaced_points(double distance) const
{
    Points points;
    points.emplace_back(this->first_point());
    double len = 0;
    
    for (Points::const_iterator it = this->points.begin() + 1; it != this->points.end(); ++it) {
        Vec2d  p1 = (it-1)->cast<double>();
        Vec2d  v  = it->cast<double>() - p1;
        double segment_length = v.norm();
        len += segment_length;
        if (len < distance)
            continue;
        if (len == distance) {
            points.emplace_back(*it);
            len = 0;
            continue;
        }
        double take = segment_length - (len - distance);  // how much we take of this segment
        points.emplace_back((p1 + v * (take / v.norm())).cast<coord_t>());
        -- it;
        len = - take;
    }
    return points;
}

void Polyline::simplify(double tolerance)
{
    this->points = MultiPoint::_douglas_peucker(this->points, tolerance);
    this->fitting_result.clear();
}

void Polyline::simplify_by_fitting_arc(double tolerance)
{
    //BBS: do arc fit first, then use DP simplify to handle the straight part to reduce point.
    ArcFitter::do_arc_fitting_and_simplify(this->points, this->fitting_result, tolerance);
}

Polylines Polyline::equally_spaced_lines(double distance) const
{
    Polylines lines;
    Polyline line;
    line.append(this->first_point());
    double len = 0;

    for (Points::const_iterator it = this->points.begin() + 1; it != this->points.end(); ++it) {
        Vec2d  p1 = line.points.back().cast<double>();
        Vec2d  v = it->cast<double>() - p1;
        double segment_length = v.norm();
        len += segment_length;
        if (len < distance)
            continue;
        if (len == distance) {
            line.append(*it);
            lines.emplace_back(line);
            
            line.clear();
            line.append(*it);
            len = 0;
            continue;
        }
        double take = distance;  // how much we take of this segment
        line.append((p1 + v * (take / v.norm())).cast<coord_t>());
        lines.emplace_back(line);

        line.clear();
        line.append(lines.back().last_point());
        --it;
        len = -take;
    }
    // add the last reminder
    if (line.size() == 1) {
        line.append(this->last_point());
        if(line.first_point()!=line.last_point())
            lines.emplace_back(line);
    }
    return lines;
}

#if 0
// This method simplifies all *lines* contained in the supplied area
template <class T>
void Polyline::simplify_by_visibility(const T &area)
{
    Points &pp = this->points;
    
    size_t s = 0;
    bool did_erase = false;
    for (size_t i = s+2; i < pp.size(); i = s + 2) {
        if (area.contains(Line(pp[s], pp[i]))) {
            pp.erase(pp.begin() + s + 1, pp.begin() + i);
            did_erase = true;
        } else {
            ++s;
        }
    }
    if (did_erase)
        this->simplify_by_visibility(area);
}
template void Polyline::simplify_by_visibility<ExPolygon>(const ExPolygon &area);
template void Polyline::simplify_by_visibility<ExPolygonCollection>(const ExPolygonCollection &area);
#endif

void Polyline::split_at(Point &point, Polyline* p1, Polyline* p2) const
{
    if (this->points.empty()) return;

    //0 judge whether the point is on the polyline
    int index = this->find_point(point);
    if (index != -1) {
        //BBS: the spilit point is on the polyline, then easy
        split_at_index(index, p1, p2);
        point = p1->is_valid()? p1->last_point(): p2->first_point();
        return;
    }
    
    //1 find the line to split at
    size_t line_idx = 0;
    Point p = this->first_point();
    double min = (p - point).cast<double>().norm();
    Lines lines = this->lines();
    for (Lines::const_iterator line = lines.begin(); line != lines.end(); ++line) {
        Point p_tmp = point.projection_onto(*line);
        if ((p_tmp - point).cast<double>().norm() < min) {
	        p = p_tmp;
	        min = (p - point).cast<double>().norm();
	        line_idx = line - lines.begin();
        }
    }

    //2 judge whether the cloest point is one vertex of polyline.
    //  and spilit the polyline at different index
    index = this->find_point(p);
    if (index != -1)
    {
        this->split_at_index(index, p1, p2);
        p1->append(point);
        p2->append_before(point);
    } else {
        Polyline temp;
        this->split_at_index(line_idx, p1, &temp);
        p1->append(point);
        this->split_at_index(line_idx + 1, &temp, p2);
        p2->append_before(point);
    }
}


bool Polyline::split_at_index(const size_t index, Polyline* p1, Polyline* p2) const
{
    if (index > this->size() - 1)
        return false;

    if (index == 0) {
        p1->clear();
        p1->append(this->first_point());
        *p2 = *this;
    } else if (index == this->size() - 1) {
        p2->clear();
        p2->append(this->last_point());
        *p1 = *this;
    } else {
        //BBS: spilit first part
        p1->clear();
        p1->points.reserve(index + 1);
        p1->points.insert(p1->begin(), this->begin(), this->begin() + index + 1);
        Point new_endpoint;
        if (this->split_fitting_result_before_index(index, new_endpoint, p1->fitting_result))
            p1->points.back() = new_endpoint;

        p2->clear();
        p2->points.reserve(this->size() - index);
        p2->points.insert(p2->begin(), this->begin() + index, this->end());
        Point new_startpoint;
        if (this->split_fitting_result_after_index(index, new_startpoint, p2->fitting_result))
            p2->points.front() = new_startpoint;
    }
    return true;
}

bool Polyline::split_at_length(const double length, Polyline* p1, Polyline* p2) const
{
    if (this->points.empty()) return false;
    if (length < 0 || length > this->length()) {
        return false;
    }

    if (length < SCALED_EPSILON) {
        p1->clear();
        p1->append(this->first_point());
        *p2 = *this;
    } else if (is_approx(length, this->length(), SCALED_EPSILON)) {
        p2->clear();
        p2->append(this->last_point());
        *p1 = *this;
    } else {
        // 1 find the line to split at
        size_t line_idx = 0;
        double acc_length = 0;
        Point p = this->first_point();
        for (const auto& l : this->lines()) {
            p = l.b;

            const double current_length = l.length();
            if (acc_length + current_length >= length) {
                p = lerp(l.a, l.b, (length - acc_length) / current_length);
                break;
            }
            acc_length += current_length;
            line_idx++;
        }

        //2 judge whether the cloest point is one vertex of polyline.
        //  and spilit the polyline at different index
        int index = this->find_point(p);
        if (index != -1) {
            this->split_at_index(index, p1, p2);
        } else {
            Polyline temp;
            this->split_at_index(line_idx, p1, &temp);
            p1->append(p);
            this->split_at_index(line_idx + 1, &temp, p2);
            p2->append_before(p);
        }
    }
    return true;
}

bool Polyline::is_straight() const
{
    // Check that each segment's direction is equal to the line connecting
    // first point and last point. (Checking each line against the previous
    // one would cause the error to accumulate.)
    double dir = Line(this->first_point(), this->last_point()).direction();
    for (const auto &line: this->lines())
        if (! line.parallel_to(dir))
            return false;
    return true;
}

void Polyline::append(const Polyline &src)
{
    if (!src.is_valid()) return;

    if (this->points.empty()) {
        this->points = src.points;
        this->fitting_result = src.fitting_result;
    } else {
        //BBS: append the first point to create connection first, update the fitting date as well
        this->append(src.points[0]);
        //BBS: append a polyline which has fitting data to a polyline without fitting data.
        //Then create a fake fitting data first, so that we can keep the fitting data in last polyline
        if (this->fitting_result.empty() &&
            !src.fitting_result.empty()) {
            this->fitting_result.emplace_back(PathFittingData{ 0, this->points.size() - 1, EMovePathType::Linear_move, ArcSegment() });
        }
        //BBS: then append the remain points
        MultiPoint::append(src.points.begin() + 1, src.points.end());
        //BBS: finally append the fitting data
        append_fitting_result_after_append_polyline(src);
    }
}

void Polyline::append(Polyline &&src)
{
    if (!src.is_valid()) return;

    if (this->points.empty()) {
        this->points = std::move(src.points);
        this->fitting_result = std::move(src.fitting_result);
    } else {
        //BBS: append the first point to create connection first, update the fitting date as well
        this->append(src.points[0]);
        //BBS: append a polyline which has fitting data to a polyline without fitting data.
        //Then create a fake fitting data first, so that we can keep the fitting data in last polyline
        if (this->fitting_result.empty() &&
            !src.fitting_result.empty()) {
            this->fitting_result.emplace_back(PathFittingData{ 0, this->points.size() - 1, EMovePathType::Linear_move, ArcSegment() });
        }
        //BBS: then append the remain points
        MultiPoint::append(src.points.begin() + 1, src.points.end());
        //BBS: finally append the fitting data
        append_fitting_result_after_append_polyline(src);
        src.points.clear();
        src.fitting_result.clear();
    }
}

void Polyline::append_fitting_result_after_append_points() {
    if (!fitting_result.empty()) {
        if (fitting_result.back().is_linear_move()) {
            fitting_result.back().end_point_index = this->points.size() - 1;
        } else {
            size_t new_start = fitting_result.back().end_point_index;
            size_t new_end = this->points.size() - 1;
            if (new_start != new_end)
                fitting_result.emplace_back(PathFittingData{ new_start, new_end, EMovePathType::Linear_move, ArcSegment() });
        }
    }
}

void Polyline::append_fitting_result_after_append_polyline(const Polyline& src)
{
    if (!this->fitting_result.empty()) {
        //BBS: offset and save the fitting_result from src polyline
        if (!src.fitting_result.empty()) {
            size_t old_size = this->fitting_result.size();
            size_t index_offset = this->fitting_result.back().end_point_index;
            this->fitting_result.insert(this->fitting_result.end(), src.fitting_result.begin(), src.fitting_result.end());
            for (size_t i = old_size; i < this->fitting_result.size(); i++) {
                this->fitting_result[i].start_point_index += index_offset;
                this->fitting_result[i].end_point_index += index_offset;
            }
        } else {
            //BBS: the append polyline has no fitting data, then append as linear move directly
            size_t new_start = this->fitting_result.back().end_point_index;
            size_t new_end = this->size() - 1;
            if (new_start != new_end)
                this->fitting_result.emplace_back(PathFittingData{ new_start, new_end, EMovePathType::Linear_move, ArcSegment() });
        }
    }
}

void Polyline::reset_to_linear_move()
{
    this->fitting_result.clear();
    fitting_result.emplace_back(PathFittingData{ 0, points.size() - 1, EMovePathType::Linear_move, ArcSegment() });
    this->fitting_result.shrink_to_fit();
}

bool Polyline::split_fitting_result_before_index(const size_t index, Point& new_endpoint, std::vector<PathFittingData>& data) const
{
    data.clear();
    new_endpoint = this->points[index];
    if (!this->fitting_result.empty()) {
        //BBS: max size
        data.reserve(this->fitting_result.size());
        //BBS: save fitting result before index
        for (size_t i = 0; i < this->fitting_result.size(); i++)
        {
            if (this->fitting_result[i].start_point_index < index)
                data.push_back(this->fitting_result[i]);
            else
                break;
        }

        if (!data.empty()) {
            //BBS: need to clip the arc and generate new end point
            if (data.back().is_arc_move() && data.back().end_point_index > index) {
                if (!data.back().arc_data.clip_end(this->points[index]))
                    //BBS: failed to clip arc, then return to be linear move
                    data.back().path_type = EMovePathType::Linear_move;
                else
                    //BBS: succeed to clip arc, then update and return the new end point
                    new_endpoint = data.back().arc_data.end_point;
            }
            data.back().end_point_index = index;
        }
        data.shrink_to_fit();
        return true;
    }
    return false;
}
bool Polyline::split_fitting_result_after_index(const size_t index, Point& new_startpoint, std::vector<PathFittingData>& data) const
{
    data.clear();
    new_startpoint = this->points[index];
    if (!this->fitting_result.empty()) {
        data.reserve(this->fitting_result.size());
        for (size_t i = 0; i < this->fitting_result.size(); i++) {
            if (this->fitting_result[i].end_point_index > index)
                data.push_back(this->fitting_result[i]);
        }
        if (!data.empty()) {
            for (size_t i = 0; i < data.size(); i++) {
                if (i != 0) {
                    data[i].start_point_index -= index;
                    data[i].end_point_index -= index;
                } else {
                    data[i].end_point_index -= index;
                    //BBS: need to clip the arc and generate new start point
                    if (data.front().is_arc_move() && data.front().start_point_index < index) {
                        if (!data.front().arc_data.clip_start(this->points[index]))
                            //BBS: failed to clip arc, then return to be linear move
                            data.front().path_type = EMovePathType::Linear_move;
                        else
                            //BBS: succeed to clip arc, then update and return the new start point
                            new_startpoint = data.front().arc_data.start_point;
                    }
                    data[i].start_point_index = 0;
                }
            }
        }
        data.shrink_to_fit();
        return true;
    }
    return false;
}

BoundingBox get_extents(const Polyline &polyline)
{
    return polyline.bounding_box();
}

BoundingBox get_extents(const Polylines &polylines)
{
    BoundingBox bb;
    if (! polylines.empty()) {
        bb = polylines.front().bounding_box();
        for (size_t i = 1; i < polylines.size(); ++ i)
            bb.merge(polylines[i].points);
    }
    return bb;
}

// Return True when erase some otherwise False.
bool remove_same_neighbor(Polyline &polyline) {
    Points &points = polyline.points;
    if (points.empty())
        return false;
    auto last = std::unique(points.begin(), points.end());

    // no duplicits
    if (last == points.end())
        return false;

    points.erase(last, points.end());
    return true;
}

bool remove_same_neighbor(Polylines &polylines){
    if (polylines.empty())
        return false;
    bool exist = false;
    for (Polyline &polyline : polylines)
        exist |= remove_same_neighbor(polyline);
    // remove empty polylines
    polylines.erase(std::remove_if(polylines.begin(), polylines.end(), [](const Polyline &p) { return p.points.size() <= 1; }), polylines.end());
    return exist;
}


const Point& leftmost_point(const Polylines &polylines)
{
    if (polylines.empty())
        throw Slic3r::InvalidArgument("leftmost_point() called on empty PolylineCollection");
    Polylines::const_iterator it = polylines.begin();
    const Point *p = &it->leftmost_point();
    for (++ it; it != polylines.end(); ++it) {
        const Point *p2 = &it->leftmost_point();
        if (p2->x() < p->x())
            p = p2;
    }
    return *p;
}

bool remove_degenerate(Polylines &polylines)
{
    bool modified = false;
    size_t j = 0;
    for (size_t i = 0; i < polylines.size(); ++ i) {
        if (polylines[i].points.size() >= 2) {
            if (j < i) 
                std::swap(polylines[i].points, polylines[j].points);
            ++ j;
        } else
            modified = true;
    }
    if (j < polylines.size())
        polylines.erase(polylines.begin() + j, polylines.end());
    return modified;
}

std::pair<int, Point> foot_pt(const Points &polyline, const Point &pt)
{
    if (polyline.size() < 2) return std::make_pair(-1, Point(0, 0));

    auto  d2_min = std::numeric_limits<double>::max();
    Point foot_pt_min;
    Point prev    = polyline.front();
    auto  it      = polyline.begin();
    auto  it_proj = polyline.begin();
    for (++it; it != polyline.end(); ++it) {
        Point  foot_pt = pt.projection_onto(Line(prev, *it));
        double d2      = (foot_pt - pt).cast<double>().squaredNorm();
        if (d2 < d2_min) {
            d2_min      = d2;
            foot_pt_min = foot_pt;
            it_proj     = it;
        }
        prev = *it;
    }
    return std::make_pair(int(it_proj - polyline.begin()) - 1, foot_pt_min);
}

ThickLines ThickPolyline::thicklines() const
{
    ThickLines lines;
    if (this->points.size() >= 2) {
        lines.reserve(this->points.size() - 1);
        for (size_t i = 0; i + 1 < this->points.size(); ++ i)
            lines.emplace_back(this->points[i], this->points[i + 1], this->width[2 * i], this->width[2 * i + 1]);
    }
    return lines;
}

void ThickPolyline::start_at_index(int index)
{
    assert(index >= 0 && index < this->points.size());
    assert(this->points.front() == this->points.back() && this->width.front() == this->width.back());
    if (index != 0 && index + 1 != int(this->points.size()) && this->points.front() == this->points.back() && this->width.front() == this->width.back()) {
        this->points.pop_back();
        assert(this->points.size() * 2 == this->width.size());
        std::rotate(this->points.begin(), this->points.begin() + index, this->points.end());
        std::rotate(this->width.begin(), this->width.begin() + 2 * index, this->width.end());
        this->points.emplace_back(this->points.front());
    }
}

Lines3 Polyline3::lines() const
{
    Lines3 lines;
    if (points.size() >= 2)
    {
        lines.reserve(points.size() - 1);
        for (Points3::const_iterator it = points.begin(); it != points.end() - 1; ++it)
        {
            lines.emplace_back(*it, *(it + 1));
        }
    }
    return lines;
}

}
