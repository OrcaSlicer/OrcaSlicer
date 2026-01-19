#include "Circle.hpp"

#include <cmath>
#include <cassert>
#include "Geometry.hpp"


//BBS: Refer to ArcWelderLib for the arc fitting functions

namespace Slic3r {

//BBS: threshold used to judge collineation
static const double Parallel_area_threshold = 0.0001;

bool Circle::try_create_circle(const Point& p1, const Point& p2, const Point& p3, const double max_radius, Circle& new_circle)
{
    double x1 = p1.x();
    double y1 = p1.y();
    double x2 = p2.x();
    double y2 = p2.y();
    double x3 = p3.x();
    double y3 = p3.y();

    //BBS: use area of triangle to judge whether three points are almostly on one line
    //Because the point is scale_ once, so area should scale_ twice.
    if (fabs((y1 - y2) * (x1 - x3) - (y1 - y3) * (x1 - x2)) <= scale_(scale_(Parallel_area_threshold)))
        return false;

    double a = x1 * (y2 - y3) - y1 * (x2 - x3) + x2 * y3 - x3 * y2;
    //BBS: take out to figure out how we handle very small values
    if (fabs(a) < SCALED_EPSILON)
        return false;

    double b = (x1 * x1 + y1 * y1) * (y3 - y2)
        + (x2 * x2 + y2 * y2) * (y1 - y3)
        + (x3 * x3 + y3 * y3) * (y2 - y1);

    double c = (x1 * x1 + y1 * y1) * (x2 - x3)
        + (x2 * x2 + y2 * y2) * (x3 - x1)
        + (x3 * x3 + y3 * y3) * (x1 - x2);

    double center_x = -b / (2.0 * a);
    double center_y = -c / (2.0 * a);

    double delta_x = center_x - x1;
    double delta_y = center_y - y1;
    double radius = sqrt(delta_x * delta_x + delta_y * delta_y);
    if (radius > max_radius)
        return false;

    new_circle.center = Point(center_x, center_y);
    new_circle.radius = radius;

    return true;
}

bool Circle::try_create_circle(const Points& points, const double max_radius, const double tolerance, Circle& new_circle)
{
    size_t count = points.size();
    size_t middle_index = count / 2;
    // BBS: the middle point will almost always produce the best arcs with high possibility.
    if (count == 3) {
        return (Circle::try_create_circle(points[0], points[middle_index], points[count - 1], max_radius, new_circle)
                && !new_circle.is_over_deviation(points, tolerance));
    } else {
        Point middle_point = (count % 2 == 0) ? (points[middle_index] + points[middle_index - 1]) / 2 :
                                                (points[middle_index - 1] + points[middle_index + 1]) / 2;
        if (Circle::try_create_circle(points[0], middle_point, points[count - 1], max_radius, new_circle)
            && !new_circle.is_over_deviation(points, tolerance))
            return true;
    }

    // BBS: Find the circle with the least deviation, if one exists.
    Circle test_circle;
    double least_deviation;
    bool found_circle = false;
    double current_deviation;
    for (int index = 1; index < count - 1; index++)
    {
        if (index == middle_index)
            // BBS: We already checked this one, and it failed. don't need to do again
            continue;

        if (Circle::try_create_circle(points[0], points[index], points[count - 1], max_radius, test_circle) && test_circle.get_deviation_sum_squared(points, tolerance, current_deviation))
        {
            if (!found_circle || current_deviation < least_deviation)
            {
                found_circle = true;
                least_deviation = current_deviation;
                new_circle = test_circle;
            }
        }
    }
    return found_circle;
}

double Circle::get_polar_radians(const Point& p1) const
{
    double polar_radians = atan2(p1.y() - center.y(), p1.x() - center.x());
    if (polar_radians < 0)
        polar_radians = (2.0 * PI) + polar_radians;
    return polar_radians;
}

bool Circle::is_over_deviation(const Points& points, const double tolerance)
{
    Point closest_point;
    Point temp;
    double distance_from_center;
    // BBS: skip the first and last points since they has fit perfectly.
    for (size_t index = 0; index < points.size() - 1; index++)
    {
        if (index != 0)
        {
            //BBS: check fitting tolerance
            temp = points[index] - center;
            distance_from_center = sqrt((double)temp.x() * (double)temp.x() + (double)temp.y() * (double)temp.y());
            if (std::fabs(distance_from_center - radius) > tolerance)
                return true;
        }

        //BBS: Check the point perpendicular from the segment to the circle's center
        if (get_closest_perpendicular_point(points[index], points[(size_t)index + 1], center, closest_point)) {
            temp = closest_point - center;
            distance_from_center = sqrt((double)temp.x() * (double)temp.x() + (double)temp.y() * (double)temp.y());
            if (std::fabs(distance_from_center - radius) > tolerance)
                return true;
        }
    }
    return false;
}

bool Circle::get_closest_perpendicular_point(const Point& p1, const Point& p2, const Point& c, Point& out)
{
    double x1 = p1.x();
    double y1 = p1.y();
    double x2 = p2.x();
    double y2 = p2.y();
    double x_dif = x2 - x1;
    double y_dif = y2 - y1;
    //BBS: [(Cx - Ax)(Bx - Ax) + (Cy - Ay)(By - Ay)] / [(Bx - Ax) ^ 2 + (By - Ay) ^ 2]
    double num = (c[0] - x1) * x_dif + (c[1] - y1) * y_dif;
    double denom = (x_dif * x_dif) + (y_dif * y_dif);
    double t = num / denom;

    //BBS: Considering this a failure if t == 0 or t==1 within tolerance.  In that case we hit the endpoint, which is OK.
    if (Circle::less_than_or_equal(t, 0) || Circle::greater_than_or_equal(t, 1))
        return false;

    out[0] = x1 + t * (x2 - x1);
    out[1] = y1 + t * (y2 - y1);
    return true;
}

bool Circle::get_deviation_sum_squared(const Points& points, const double tolerance, double& total_deviation)
{
    total_deviation = 0;
    Point temp;
    double distance_from_center,  deviation;
    // BBS: skip the first and last points since they are on the circle
    for (int index = 1; index < points.size() - 1; index++)
    {
        //BBS: make sure the length from the center of our circle to the test point is 
        // at or below our max distance.
        temp = points[index] - center;
        distance_from_center = sqrt((double)temp.x() * (double)temp.x() + (double)temp.y() * (double)temp.y());
        deviation = std::fabs(distance_from_center - radius);
        total_deviation += deviation * deviation;
        if (deviation > tolerance)
            return false;

    }
    Point closest_point;
    //BBS: check the point perpendicular from the segment to the circle's center
    for (int index = 0; index < points.size() - 1; index++)
    {
        if (get_closest_perpendicular_point(points[index], points[(size_t)index + 1], center, closest_point)) {
            temp = closest_point - center;
            distance_from_center = sqrt((double)temp.x() * (double)temp.x() + (double)temp.y() * (double)temp.y());
            deviation = std::fabs(distance_from_center - radius);
            total_deviation += deviation * deviation;
            if (deviation > tolerance)
                return false;
        }
    }
    return true;
}

//BBS: only support calculate on X-Y plane, Z is useless
Vec3f Circle::calc_tangential_vector(const Vec3f& pos, const Vec3f& center_pos, const bool is_ccw)
{
    Vec3f dir = center_pos - pos;
    dir(2,0) = 0;
    dir.normalize();
    Vec3f res;
    if (is_ccw)
        res = { dir(1, 0), -dir(0, 0), 0.0f };
    else
        res = { -dir(1, 0), dir(0, 0), 0.0f };
    return res;
}

bool ArcSegment::reverse()
{
    if (!is_valid())
        return false;
    std::swap(start_point, end_point);
    direction = (direction == ArcDirection::Arc_Dir_CCW) ? ArcDirection::Arc_Dir_CW : ArcDirection::Arc_Dir_CCW;
    angle_radians *= -1.0;
    std::swap(polar_start_theta, polar_end_theta);
    return true;
}

bool ArcSegment::clip_start(const Point &point)
{
    if (!is_valid() || point == center || !is_point_inside(point))
        return false;
    start_point = get_closest_point(point);
    update_angle_and_length();
    return true;
}

bool ArcSegment::clip_end(const Point &point)
{
    if (!is_valid() || point == center || !is_point_inside(point))
        return false;
    end_point = get_closest_point(point);
    update_angle_and_length();
    return true;
}

bool ArcSegment::split_at(const Point &point, ArcSegment& p1, ArcSegment& p2)
{
    if (!is_valid() || point == center || !is_point_inside(point))
        return false;
    Point segment_point = get_closest_point(point);
    p1 = ArcSegment(center, radius, this->start_point, segment_point, this->direction);
    p2 = ArcSegment(center, radius, segment_point, this->end_point,  this->direction);
    return true;
}

bool ArcSegment::is_point_inside(const Point& point) const
{
    double polar_theta = get_polar_radians(point);
    double radian_delta = polar_theta - polar_start_theta;
    if (radian_delta > 0 && direction == ArcDirection::Arc_Dir_CW)
        radian_delta = radian_delta - 2 * M_PI;
    else if (radian_delta < 0 && direction == ArcDirection::Arc_Dir_CCW)
        radian_delta = radian_delta + 2 * M_PI;

    return (direction == ArcDirection::Arc_Dir_CCW ?
            radian_delta > 0.0 && radian_delta < angle_radians :
            radian_delta < 0.0 && radian_delta > angle_radians);
}

void ArcSegment::update_angle_and_length()
{
    polar_start_theta = get_polar_radians(start_point);
    polar_end_theta = get_polar_radians(end_point);
    angle_radians = polar_end_theta - polar_start_theta;
    if (angle_radians < 0 && direction == ArcDirection::Arc_Dir_CCW)
        angle_radians = angle_radians + 2 * M_PI;
    else if (angle_radians > 0 && direction == ArcDirection::Arc_Dir_CW)
        angle_radians = angle_radians - 2 * M_PI;
    length = fabs(angle_radians) * radius;
    is_arc = true;
}

bool ArcSegment::try_create_arc(
    const Points& points,
    ArcSegment& target_arc,
    double approximate_length,
    double max_radius,
    double tolerance,
    double path_tolerance_percent)
{
    Circle test_circle = (Circle)target_arc;
    if (!Circle::try_create_circle(points, max_radius, tolerance, test_circle))
        return false;

    int mid_point_index = ((points.size() - 2) / 2) + 1;
    ArcSegment test_arc;
    if (!ArcSegment::try_create_arc(test_circle, points[0], points[mid_point_index], points[points.size() - 1], test_arc, approximate_length, path_tolerance_percent))
        return false;

    if (ArcSegment::are_points_within_slice(test_arc, points))
    {
        target_arc = test_arc;
        return true;
    }
    return false;
}

bool ArcSegment::try_create_arc(
    const Circle& c,
    const Point& start_point,
    const Point& mid_point,
    const Point& end_point,
    ArcSegment& target_arc,
    double approximate_length,
    double path_tolerance_percent)
{
    double polar_start_theta = c.get_polar_radians(start_point);
    double polar_mid_theta = c.get_polar_radians(mid_point);
    double polar_end_theta = c.get_polar_radians(end_point);

    double angle_radians = 0;
    ArcDirection direction = ArcDirection::Arc_Dir_unknow;
    //BBS: calculate the direction of the arc
    if (polar_end_theta > polar_start_theta) {
        if (polar_start_theta < polar_mid_theta && polar_mid_theta < polar_end_theta) {
            direction = ArcDirection::Arc_Dir_CCW;
            angle_radians = polar_end_theta - polar_start_theta;
        } else if ((0.0 <= polar_mid_theta && polar_mid_theta < polar_start_theta) ||
                   (polar_end_theta < polar_mid_theta && polar_mid_theta < (2.0 * PI))) {
            direction = ArcDirection::Arc_Dir_CW;
            angle_radians = polar_start_theta + ((2.0 * PI) - polar_end_theta);
        }
    } else if (polar_start_theta > polar_end_theta) {
        if ((polar_start_theta < polar_mid_theta && polar_mid_theta < (2.0 * PI)) ||
            (0.0 < polar_mid_theta && polar_mid_theta < polar_end_theta)) {
            direction = ArcDirection::Arc_Dir_CCW;
            angle_radians = polar_end_theta + ((2.0 * PI) - polar_start_theta);
        } else if (polar_end_theta < polar_mid_theta && polar_mid_theta < polar_start_theta) {
            direction = ArcDirection::Arc_Dir_CW;
            angle_radians = polar_start_theta - polar_end_theta;
        }
    }

    // BBS: this doesn't always work.. in rare situations, the angle may be backward
    if (direction == ArcDirection::Arc_Dir_unknow || std::fabs(angle_radians) < EPSILON)
        return false;

    // BBS: Check the length against the original length.
    // This can trigger simply due to the differing path lengths
    // but also could indicate that the vector calculation above
    // got wrong direction
    double arc_length = c.radius * angle_radians;
    double difference = (arc_length - approximate_length) / approximate_length;
    if (std::fabs(difference) >= path_tolerance_percent)
    {
        // BBS: So it's possible that vector calculation above got wrong direction.
        // This can happen if there is a crazy arrangement of points
        // extremely close to eachother. They have to be close enough to
        // break our other checks.  However, we may be able to salvage this.
        // see if an arc moving in the opposite direction had the correct length.

        //BBS: Find the rest of the angle across the circle
        double test_radians = std::fabs(angle_radians - 2 * PI);
        // Calculate the length of that arc
        double test_arc_length = c.radius * test_radians;
        difference = (test_arc_length - approximate_length) / approximate_length;
        if (std::fabs(difference) >= path_tolerance_percent)
            return false;
        //BBS: Set the new length and flip the direction (but not the angle)!
        arc_length = test_arc_length;
        direction = direction == ArcDirection::Arc_Dir_CCW ? ArcDirection::Arc_Dir_CW : ArcDirection::Arc_Dir_CCW;
    }

    if (direction == ArcDirection::Arc_Dir_CW)
        angle_radians *= -1.0;

    target_arc.is_arc = true;
    target_arc.direction = direction;
    target_arc.center = c.center;
    target_arc.radius = c.radius;
    target_arc.start_point = start_point;
    target_arc.end_point = end_point;
    target_arc.length = arc_length;
    target_arc.angle_radians = angle_radians;
    target_arc.polar_start_theta = polar_start_theta;
    target_arc.polar_end_theta = polar_end_theta;

    return true;
}

bool ArcSegment::are_points_within_slice(const ArcSegment& test_arc, const Points& points)
{
    //BBS: Check all the points and see if they fit inside of the angles
    double previous_polar = test_arc.polar_start_theta;
    bool will_cross_zero = false;
    bool crossed_zero = false;
    const int point_count = points.size();

    Vec2d start_norm(((double)test_arc.start_point.x() - (double)test_arc.center.x()) / test_arc.radius,
                     ((double)test_arc.start_point.y() - (double)test_arc.center.y()) / test_arc.radius);
    Vec2d end_norm(((double)test_arc.end_point.x() - (double)test_arc.center.x()) / test_arc.radius,
                   ((double)test_arc.end_point.y() - (double)test_arc.center.y()) / test_arc.radius);

    if (test_arc.direction == ArcDirection::Arc_Dir_CCW)
        will_cross_zero = test_arc.polar_start_theta > test_arc.polar_end_theta;
    else
        will_cross_zero = test_arc.polar_start_theta < test_arc.polar_end_theta;

    //BBS: check if point 1 to point 2 cross zero
    double polar_test;
    for (int index = point_count - 2; index < point_count; index++)
    {
        if (index < point_count - 1)
          polar_test = test_arc.get_polar_radians(points[index]);
        else
          polar_test = test_arc.polar_end_theta;

        //BBS: First ensure the test point is within the arc
        if (test_arc.direction == ArcDirection::Arc_Dir_CCW)
        {
            //BBS: Only check to see if we are within the arc if this isn't the endpoint
            if (index < point_count - 1) {
                if (will_cross_zero) {
                    if (!(polar_test > test_arc.polar_start_theta || polar_test < test_arc.polar_end_theta))
                        return false;
                } else if (!(test_arc.polar_start_theta < polar_test && polar_test < test_arc.polar_end_theta))
                    return false;
            }
            //BBS: check the angles are increasing
            if (previous_polar > polar_test) {
                if (!will_cross_zero)
                    return false;

                //BBS: Allow the angle to cross zero once
                if (crossed_zero)
                    return false;
                crossed_zero = true;
            }
        } else {
            if (index < point_count - 1) {
                if (will_cross_zero) {
                    if (!(polar_test < test_arc.polar_start_theta || polar_test > test_arc.polar_end_theta))
                        return false;
                } else if (!(test_arc.polar_start_theta > polar_test && polar_test > test_arc.polar_end_theta))
                    return false;
            }
            //BBS: Now make sure the angles are decreasing
            if (previous_polar < polar_test)
            {
                if (!will_cross_zero)
                    return false;
                //BBS: Allow the angle to cross zero once
                if (crossed_zero)
                    return false;
                crossed_zero = true;
            }
        }

        // BBS: check if the segment intersects either of the vector from the center of the circle to the endpoints of the arc
        Line segmemt(points[index - 1], points[index]);
        if ((index != 1 && ray_intersects_segment(test_arc.center, start_norm, segmemt)) ||
            (index != point_count - 1 && ray_intersects_segment(test_arc.center, end_norm, segmemt)))
            return false;
        previous_polar = polar_test;
    }
    //BBS: Ensure that all arcs that cross zero
    if (will_cross_zero != crossed_zero)
        return false;
    return true;
}

// BBS: this function is used to detect whether a ray cross the segment
bool ArcSegment::ray_intersects_segment(const Point &rayOrigin, const Vec2d &rayDirection, const Line& segment)
{
    Vec2d v1 = Vec2d(rayOrigin.x() - segment.a.x(), rayOrigin.y() - segment.a.y());
    Vec2d v2 = Vec2d(segment.b.x() - segment.a.x(), segment.b.y() - segment.a.y());
    Vec2d v3 = Vec2d(-rayDirection(1), rayDirection(0));

    double dot = v2(0) * v3(0) + v2(1) * v3(1);
    if (std::fabs(dot) < SCALED_EPSILON)
        return false;

    double t1 = (v2(0) * v1(1) - v2(1) * v1(0)) / dot;
    double t2 = (v1(0) * v3(0) + v1(1) * v3(1)) / dot;

    if (t1 >= 0.0 && (t2 >= 0.0 && t2 <= 1.0))
        return true;

    return false;
}

// BBS: new function to calculate arc radian in X-Y plane
float ArcSegment::calc_arc_radian(Vec3f start_pos, Vec3f end_pos, Vec3f center_pos, bool is_ccw)
{
    Vec3f delta1 = center_pos - start_pos;
    Vec3f delta2 = center_pos - end_pos;
    // only consider arc in x-y plane, so clean z distance
    delta1(2,0) = 0;
    delta2(2,0) = 0;

    float radian;
    if ((delta1 - delta2).norm() < 1e-6) {
        // start_pos is same with end_pos, we think it's a full circle
        radian = 2 * M_PI;
    } else {
        double dot = delta1.dot(delta2);
        double cross = (double)delta1(0, 0) * (double)delta2(1, 0) - (double)delta1(1, 0) * (double)delta2(0, 0);
        radian = atan2(cross, dot);
        if (is_ccw)
            radian = (radian < 0) ? 2 * M_PI + radian : radian;
        else
            radian = (radian < 0) ? abs(radian) : 2 * M_PI - radian;
    }
    return radian;
}

float ArcSegment::calc_arc_radius(Vec3f start_pos, Vec3f center_pos)
{
    Vec3f delta1 = center_pos - start_pos;
    delta1(2,0) = 0;
    return delta1.norm();
}

// BBS: new function to calculate arc length in X-Y plane
float ArcSegment::calc_arc_length(Vec3f start_pos, Vec3f end_pos, Vec3f center_pos, bool is_ccw)
{
    return calc_arc_radius(start_pos, center_pos) * calc_arc_radian(start_pos, end_pos, center_pos, is_ccw);
}

}