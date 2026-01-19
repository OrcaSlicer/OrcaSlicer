#include "MultiPoint.hpp"
#include "BoundingBox.hpp"

namespace Slic3r {

void MultiPoint::scale(double factor)
{
    for (Point &pt : points)
        pt *= factor;
}

void MultiPoint::scale(double factor_x, double factor_y)
{
    for (Point &pt : points)
    {
		pt(0) = coord_t(pt(0) * factor_x);
		pt(1) = coord_t(pt(1) * factor_y);
    }
}

void MultiPoint::translate(const Point &v)
{
    for (Point &pt : points)
        pt += v;
}

void MultiPoint::rotate(double cos_angle, double sin_angle)
{
    for (Point &pt : this->points) {
        double cur_x = double(pt(0));
        double cur_y = double(pt(1));
        pt(0) = coord_t(round(cos_angle * cur_x - sin_angle * cur_y));
        pt(1) = coord_t(round(cos_angle * cur_y + sin_angle * cur_x));
    }
}

void MultiPoint::rotate(double angle, const Point &center)
{
    double s = sin(angle);
    double c = cos(angle);
    for (Point &pt : points) {
        Vec2crd v(pt - center);
        pt(0) = (coord_t)round(double(center(0)) + c * v[0] - s * v[1]);
        pt(1) = (coord_t)round(double(center(1)) + c * v[1] + s * v[0]);
    }
}

double MultiPoint::length() const
{
    const Lines& lines = this->lines();
    double len = 0;
    for (auto it = lines.cbegin(); it != lines.cend(); ++it) {
        len += it->length();
    }
    return len;
}

int MultiPoint::find_point(const Point &point) const
{
    for (const Point &pt : this->points)
        if (pt == point)
            return int(&pt - &this->points.front());
    return -1;  // not found
}

int MultiPoint::find_point(const Point &point, double scaled_epsilon) const
{
    if (scaled_epsilon == 0) return this->find_point(point);

    auto dist2_min = std::numeric_limits<double>::max();
    auto eps2      = scaled_epsilon * scaled_epsilon;
    int  idx_min   = -1;
    for (const Point &pt : this->points) {
        double d2 = (pt - point).cast<double>().squaredNorm();
        if (d2 < dist2_min) {
            idx_min   = int(&pt - &this->points.front());
            dist2_min = d2;
        }
    }
    return dist2_min < eps2 ? idx_min : -1;
}

bool MultiPoint::has_boundary_point(const Point &point) const
{
    double dist = (point.projection_onto(*this) - point).cast<double>().norm();
    return dist < SCALED_EPSILON;
}

BoundingBox MultiPoint::bounding_box() const
{
    return BoundingBox(this->points);
}

bool MultiPoint::has_duplicate_points() const
{
    for (size_t i = 1; i < points.size(); ++i)
        if (points[i-1] == points[i])
            return true;
    return false;
}

bool MultiPoint::remove_duplicate_points()
{
    size_t j = 0;
    for (size_t i = 1; i < points.size(); ++i) {
        if (points[j] == points[i]) {
            // Just increase index i.
        } else {
            ++ j;
            if (j < i)
                points[j] = points[i];
        }
    }
    if (++ j < points.size()) {
        points.erase(points.begin() + j, points.end());
        return true;
    }
    return false;
}

bool MultiPoint::intersection(const Line& line, Point* intersection) const
{
    Lines lines = this->lines();
    for (Lines::const_iterator it = lines.begin(); it != lines.end(); ++it) {
        if (it->intersection(line, intersection)) return true;
    }
    return false;
}

bool MultiPoint::first_intersection(const Line& line, Point* intersection) const
{
    bool   found = false;
    double dmin  = 0.;
    for (const Line &l : this->lines()) {
        Point ip;
        if (l.intersection(line, &ip)) {
            if (! found) {
                found = true;
                dmin = (line.a - ip).cast<double>().norm();
                *intersection = ip;
            } else {
                double d = (line.a - ip).cast<double>().norm();
                if (d < dmin) {
                    dmin = d;
                    *intersection = ip;
                }
            }
        }
    }
    return found;
}

bool MultiPoint::intersections(const Line &line, Points *intersections) const
{
    size_t intersections_size = intersections->size();
    for (const Line &polygon_line : this->lines()) {
        Point intersection;
        if (polygon_line.intersection(line, &intersection))
            intersections->emplace_back(std::move(intersection));
    }
    return intersections->size() > intersections_size;
}

Points MultiPoint::_douglas_peucker(const Points &pts, const double tolerance)
{
    Points result_pts;
	double tolerance_sq = tolerance * tolerance;
    if (! pts.empty()) {
        const Point  *anchor      = &pts.front();
        size_t        anchor_idx  = 0;
        const Point  *floater     = &pts.back();
        size_t        floater_idx = pts.size() - 1;
        result_pts.reserve(pts.size());
        result_pts.emplace_back(*anchor);
        if (anchor_idx != floater_idx) {
            assert(pts.size() > 1);
            std::vector<size_t> dpStack;
            dpStack.reserve(pts.size());
            dpStack.emplace_back(floater_idx);
            for (;;) {
                double max_dist_sq  = 0.0;
                size_t furthest_idx = anchor_idx;
                // find point furthest from line seg created by (anchor, floater) and note it
                for (size_t i = anchor_idx + 1; i < floater_idx; ++ i) {
                    double dist_sq = Line::distance_to_squared(pts[i], *anchor, *floater);
                    if (dist_sq > max_dist_sq) {
                        max_dist_sq  = dist_sq;
                        furthest_idx = i;
                    }
                }
                // remove point if less than tolerance
                if (max_dist_sq <= tolerance_sq) {
                    result_pts.emplace_back(*floater);
                    anchor_idx = floater_idx;
                    anchor     = floater;
                    assert(dpStack.back() == floater_idx);
                    dpStack.pop_back();
                    if (dpStack.empty())
                        break;
                    floater_idx = dpStack.back();
                } else {
                    floater_idx = furthest_idx;
                    dpStack.emplace_back(floater_idx);
                }
                floater = &pts[floater_idx];
            }
        }
        assert(result_pts.front() == pts.front());
        assert(result_pts.back()  == pts.back());

#if 0
        {
            static int iRun = 0;
			BoundingBox bbox(pts);
			BoundingBox bbox2(result_pts);
			bbox.merge(bbox2);
            SVG svg(debug_out_path("douglas_peucker_%d.svg", iRun ++).c_str(), bbox);
            if (pts.front() == pts.back())
                svg.draw(Polygon(pts), "black");
            else
                svg.draw(Polyline(pts), "black");
            if (result_pts.front() == result_pts.back())
                svg.draw(Polygon(result_pts), "green", scale_(0.1));
            else
                svg.draw(Polyline(result_pts), "green", scale_(0.1));
        }
#endif
    }
    return result_pts;
}

// Visivalingam simplification algorithm https://github.com/slic3r/Slic3r/pull/3825
// thanks to @fuchstraumer
/*
     struct - vis_node
     Used with the visivalignam simplification algorithm, which needs to be able to find a points
    successors and predecessors to operate succesfully. Since this struct is only used in one
    location, it could probably be dropped into a namespace to avoid polluting the slic3r namespace.
     Source: https://github.com/shortsleeves/visvalingam_simplify
     ^ Provided original algorithm implementation. I've only changed things a bit to "clean" them up
    (i.e be more like my personal style), and managed to do this without requiring a binheap implementation
 */
struct vis_node{
    vis_node(const size_t& idx, const size_t& _prev_idx, const size_t& _next_idx, const double& _area) : pt_idx(idx), prev_idx(_prev_idx), next_idx(_next_idx), area(_area) {}
    // Indices into a Points container, from which this object was constructed
    size_t pt_idx, prev_idx, next_idx;
    // Effective area of this "node"
    double area;
    // Overloaded operator used to sort the binheap
    // Greater area = "more important" node. So, this node is less than the 
    // other node if it's area is less than the other node's area
    bool operator<(const vis_node& other) { return (this->area < other.area); }
};
Points MultiPoint::visivalingam(const Points& pts, const double tolerance)
{
    // Make sure there's enough points in "pts" to bother with simplification.
    assert(pts.size() >= 2);
     // Result object
    Points results;
     // Lambda to calculate effective area spanned by a point and its immediate 
    // successor + predecessor.
    auto effective_area = [pts](const size_t& curr_pt_idx, const size_t& prev_pt_idx, const size_t& next_pt_idx)->coordf_t {
        const Point& curr = pts[curr_pt_idx];
        const Point& prev = pts[prev_pt_idx];
        const Point& next = pts[next_pt_idx];
        // Use point objects as vector-distances
		const Vec2d curr_to_next = (next - curr).cast<double>();
		const Vec2d prev_to_next = (prev - curr).cast<double>();
        // Take cross product of these two vector distances
		return 0.50 * abs(cross2(curr_to_next, prev_to_next));
    };
     // We store the effective areas for each node
    std::vector<coordf_t> areas;
    areas.reserve(pts.size());
     // Construct the initial set of nodes. We will make a heap out of the "heap" vector using 
    // std::make_heap. node_list is used later.
    std::vector<vis_node*> node_list;
    node_list.resize(pts.size());
    std::vector<vis_node*> heap;
    heap.reserve(pts.size());
    for (size_t i = 1; i < pts.size() - 1; ++ i) {
        // Get effective area of current node.
        coordf_t area = effective_area(i, i - 1, i + 1);
        // If area is greater than some arbitrarily small value, use it.
        node_list[i] = new vis_node(i, i - 1, i + 1, area);
        heap.push_back(node_list[i]);
    }
     // Call std::make_heap, which uses the < operator by default to make "heap" into 
    // a binheap, sorted by the < operator we defind in the vis_node struct
    std::make_heap(heap.begin(), heap.end());
     // Start comparing areas. Set min_area to an outrageous value initially.
    double min_area = -std::numeric_limits<double>::max();
    while (!heap.empty()) {
         // Get current node.
        vis_node* curr = heap.front();
         // Pop node we just retrieved off the heap. pop_heap moves front element in vector
        // to the back, so we can call pop_back()
        std::pop_heap(heap.begin(), heap.end());
        heap.pop_back();
         // Sanity assert check
        assert(curr == node_list[curr->pt_idx]);
         // If the current pt'ss area is less than that of the previous pt's area
        // use the last pt's area instead. This ensures we don't elimate the current
        // point without eliminating the previous 
        min_area = std::max(min_area, curr->area);
         // Update prev
        vis_node* prev = node_list[curr->prev_idx];
        if(prev != nullptr){
            prev->next_idx = curr->next_idx;
            prev->area = effective_area(prev->pt_idx, prev->prev_idx, prev->next_idx);
            // For some reason, std::make_heap() is the fastest way to resort the heap. Probably needs testing.
            std::make_heap(heap.begin(), heap.end());
        }
         // Update next
        vis_node* next = node_list[curr->next_idx];
        if(next != nullptr){
            next->prev_idx = curr->prev_idx;
            next->area = effective_area(next->pt_idx, next->prev_idx, next->next_idx);
            std::make_heap(heap.begin(), heap.end());
        }
         areas[curr->pt_idx] = min_area;
        node_list[curr->pt_idx] = nullptr;
        delete curr;
    }
    // Clear node list and shrink_to_fit() (to free actual memory). Not necessary. Could be removed.
    node_list.clear();
    node_list.shrink_to_fit();
    // This lambda is how we test whether or not to keep a point.
    auto use_point = [areas, tolerance](const size_t& idx)->bool {
        assert(idx < areas.size());
        // Return true at front/back of path/areas
        if(idx == 0 || idx == areas.size() - 1){
            return true;
        }
        // Return true if area at idx is greater than minimum area to consider "valid"
        else{
            return areas[idx] > tolerance;
        }
    };
    // Use previously defined lambda to build results.
    for (size_t i = 0; i < pts.size(); ++i) {
        if (use_point(i)){
            results.push_back(pts[i]);
        }
    }
     // Check that results has at least two points
    assert(results.size() >= 2);
     // Return simplified vector of points
    return results;
}

// Calculate 2D concave hull of polygono within tolerence
Points MultiPoint::concave_hull_2d(const Points& pts, const double tolerence)
{
    Points hull;
    int n = (int)pts.size();
    if (n >= 3) {
        int k = 0;
        hull.resize(n);
        for (int i = 0; i < n; ++i) {
            while (k >= 2 && pts[i].ccw(hull[k - 2], hull[k - 1]) <= 0 && -pts[i].ccw(hull[k - 2], hull[k - 1]) / (pts[i] - hull[k - 1]).norm() < tolerence)
                --k;
            hull[k++] = pts[i];
        }
        hull.resize(k);
        if (hull.front() == hull.back())
            hull.pop_back();
    }
    return hull;
}


//Orca: Distancing function used by IOI wall ordering algorithm for arachne
/**
 * @brief Calculates the squared distance between a point and a line segment defined by two points.
 *
 * @param p The point.
 * @param v The starting point of the line segment.
 * @param w The ending point of the line segment.
 * @return double The squared distance between the point and the line segment.
 */
 double MultiPoint::squaredDistanceToLineSegment(const Point& p, const Point& v, const Point& w) {
    // Calculate the squared length of the line segment
    double l2 = (v - w).squaredNorm();
    // If the segment is a single point, return the squared distance to that point
    if (l2 == 0.0) return (p - v).squaredNorm();
    // Project point p onto the line defined by v and w, and clamp the projection to the segment
    double t = std::max(0.0, std::min(1.0, ((p - v).dot(w - v)) / l2));
    // Compute the projection point
    Point projection{v.x() + t * (w.x() - v.x()), v.y() + t * (w.y() - v.y())};
    // Return the squared distance between the point and the projection
    return (p - projection).squaredNorm();
}

//Orca: Distancing function used by IOI wall ordering algorithm for arachne
/**
 * @brief Calculates the minimum distance between two lines defined by sets of points.
 *
 * @param A The first set of points defining a polyline.
 * @param B The second set of points defining a polyline.
 * @return double The minimum distance between the two polylines.
 */
 double MultiPoint::minimumDistanceBetweenLinesDefinedByPoints(const Points& A, const Points& B) {
    double min_distance = std::numeric_limits<double>::infinity();

    // Calculate the minimum distance between segments in A and points in B
    for (size_t i = 0; i < A.size() - 1; ++i) {
        for (const auto& b : B) {
            double distance = squaredDistanceToLineSegment(b, A[i], A[i + 1]);
            min_distance = std::min(min_distance, std::sqrt(distance));
        }
    }

    // Calculate the minimum distance between segments in B and points in A
    for (size_t i = 0; i < B.size() - 1; ++i) {
        for (const auto& a : A) {
            double distance = squaredDistanceToLineSegment(a, B[i], B[i + 1]);
            min_distance = std::min(min_distance, std::sqrt(distance));
        }
    }

    return min_distance;
}


void MultiPoint3::translate(double x, double y)
{
    for (Vec3crd &p : points) {
        p(0) += coord_t(x);
        p(1) += coord_t(y);
    }
}

void MultiPoint3::translate(const Point& vector)
{
    this->translate(vector(0), vector(1));
}

double MultiPoint3::length() const
{
    double len = 0.0;
    for (const Line3& line : this->lines())
        len += line.length();
    return len;
}

BoundingBox3 MultiPoint3::bounding_box() const
{
    return BoundingBox3(points);
}

bool MultiPoint3::remove_duplicate_points()
{
    size_t j = 0;
    for (size_t i = 1; i < points.size(); ++i) {
        if (points[j] == points[i]) {
            // Just increase index i.
        } else {
            ++ j;
            if (j < i)
                points[j] = points[i];
        }
    }

    if (++j < points.size())
    {
        points.erase(points.begin() + j, points.end());
        return true;
    }

    return false;
}

BoundingBox get_extents(const MultiPoint &mp)
{ 
    return BoundingBox(mp.points);
}

BoundingBox get_extents_rotated(const Points &points, double angle)
{ 
    BoundingBox bbox;
    if (! points.empty()) {
        double s = sin(angle);
        double c = cos(angle);
        Points::const_iterator it = points.begin();
        double cur_x = (double)(*it)(0);
        double cur_y = (double)(*it)(1);
        bbox.min(0) = bbox.max(0) = (coord_t)round(c * cur_x - s * cur_y);
        bbox.min(1) = bbox.max(1) = (coord_t)round(c * cur_y + s * cur_x);
        for (++it; it != points.end(); ++it) {
            double cur_x = (double)(*it)(0);
            double cur_y = (double)(*it)(1);
            coord_t x = (coord_t)round(c * cur_x - s * cur_y);
            coord_t y = (coord_t)round(c * cur_y + s * cur_x);
            bbox.min(0) = std::min(x, bbox.min(0));
            bbox.min(1) = std::min(y, bbox.min(1));
            bbox.max(0) = std::max(x, bbox.max(0));
            bbox.max(1) = std::max(y, bbox.max(1));
        }
        bbox.defined = true;
    }
    return bbox;
}

BoundingBox get_extents_rotated(const MultiPoint &mp, double angle)
{
    return get_extents_rotated(mp.points, angle);
}

void MultiPoint::symmetric_y(const coord_t &x_axis)
{
    for (Point &pt : points) {
        pt(0) = 2 * x_axis - pt(0);
    }
}

}
