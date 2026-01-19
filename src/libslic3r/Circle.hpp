#ifndef slic3r_Circle_hpp_
#define slic3r_Circle_hpp_

#include "Point.hpp"
#include "Line.hpp"

namespace Slic3r {

constexpr double ZERO_TOLERANCE = 0.000005;

class Circle {
public:
    Circle() {
        center = Point(0,0);
        radius = 0;
    }
    Circle(Point &p, double r) {
        center = p;
        radius = r;
    }
    Point center;
    double radius;

    Point get_closest_point(const Point& input) {
        Vec2d v = (input - center).cast<double>().normalized();
        return (center + (v * radius).cast<coord_t>());
    }

    static bool try_create_circle(const Point &p1, const Point &p2, const Point &p3, const double max_radius, Circle& new_circle);
    static bool try_create_circle(const Points& points, const double max_radius, const double tolerance, Circle& new_circle);
    double get_polar_radians(const Point& p1) const;
    bool is_over_deviation(const Points& points, const double tolerance);
    bool get_deviation_sum_squared(const Points& points, const double tolerance, double& sum_deviation);

    //BBS: only support calculate on X-Y plane, Z is useless
    static Vec3f calc_tangential_vector(const Vec3f& pos, const Vec3f& center_pos, const bool is_ccw);
    static bool get_closest_perpendicular_point(const Point& p1, const Point& p2, const Point& c, Point& out);
    static bool is_equal(double x, double y, double tolerance = ZERO_TOLERANCE) {
        double abs_difference = std::fabs(x - y);
        return abs_difference < tolerance;
    };
    static bool greater_than(double x, double y, double tolerance = ZERO_TOLERANCE) {
        return x > y && !Circle::is_equal(x, y, tolerance);
    };
    static bool greater_than_or_equal(double x, double y, double tolerance = ZERO_TOLERANCE) {
        return x > y || Circle::is_equal(x, y, tolerance);
    };
    static bool less_than(double x, double y, double tolerance = ZERO_TOLERANCE) {
        return x < y && !Circle::is_equal(x, y, tolerance);
    };
    static bool less_than_or_equal(double x, double y, double tolerance = ZERO_TOLERANCE){
        return x < y || Circle::is_equal(x, y, tolerance);
    };

};

enum class ArcDirection : unsigned char {
    Arc_Dir_unknow,
    Arc_Dir_CCW,
    Arc_Dir_CW,
    Count
};

#define DEFAULT_SCALED_MAX_RADIUS scale_(2000)        // 2000mm
#define DEFAULT_SCALED_RESOLUTION scale_(0.05)        // 0.05mm
#define DEFAULT_ARC_LENGTH_PERCENT_TOLERANCE  0.05    // 5 percent

class ArcSegment: public Circle {
public:
    ArcSegment(): Circle() {}
    ArcSegment(Point center, double radius, Point start, Point end, ArcDirection dir) :
        Circle(center, radius),
        start_point(start),
        end_point(end),
        direction(dir) {
        if (radius == 0.0 ||
            start_point == center ||
            end_point == center ||
            start_point == end_point) {
            is_arc = false;
            return;
        }
        update_angle_and_length();
        is_arc = true;
    }

    bool is_arc = false;
    double length = 0;
    double angle_radians = 0;
    double polar_start_theta = 0;
    double polar_end_theta = 0;
    Point start_point { Point(0,0) };
    Point end_point{ Point(0,0) };
    ArcDirection direction = ArcDirection::Arc_Dir_unknow;

    bool is_valid() const { return is_arc; }
    bool clip_start(const Point& point);
    bool clip_end(const Point& point);
    bool reverse();
    bool split_at(const Point& point, ArcSegment& p1, ArcSegment& p2);
    bool is_point_inside(const Point& point) const;

private:
    void update_angle_and_length();

public:
    static bool try_create_arc(
        const Points &points,
        ArcSegment& target_arc,
        double approximate_length,
        double max_radius = DEFAULT_SCALED_MAX_RADIUS,
        double tolerance = DEFAULT_SCALED_RESOLUTION,
        double path_tolerance_percent = DEFAULT_ARC_LENGTH_PERCENT_TOLERANCE);

    static bool are_points_within_slice(const ArcSegment& test_arc, const Points &points);
    // BBS: this function is used to detect whether a ray cross the segment
    static bool ray_intersects_segment(const Point& rayOrigin, const Vec2d& rayDirection, const Line& segment);
    // BBS: these three functions are used to calculate related arguments of arc in unscale_field.
    static float calc_arc_radian(Vec3f start_pos, Vec3f end_pos, Vec3f center_pos, bool is_ccw);
    static float calc_arc_radius(Vec3f start_pos, Vec3f center_pos);
    static float calc_arc_length(Vec3f start_pos, Vec3f end_pos, Vec3f center_pos, bool is_ccw);
private:
    static bool try_create_arc(
        const Circle& c,
        const Point& start_point,
        const Point& mid_point,
        const Point& end_point,
        ArcSegment& target_arc,
        double approximate_length,
        double path_tolerance_percent = DEFAULT_ARC_LENGTH_PERCENT_TOLERANCE);
};

}

#endif
