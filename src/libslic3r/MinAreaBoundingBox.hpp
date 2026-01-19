#ifndef MINAREABOUNDINGBOX_HPP
#define MINAREABOUNDINGBOX_HPP

#include "libslic3r/Point.hpp"

namespace Slic3r {

class Polygon;
class ExPolygon;

void remove_collinear_points(Polygon& p);
void remove_collinear_points(ExPolygon& p);

/// A class that holds a rotated bounding box. If instantiated with a polygon
/// type it will hold the minimum area bounding box for the given polygon.
/// If the input polygon is convex, the complexity is linear to the number of 
/// points. Otherwise a convex hull of O(n*log(n)) has to be performed.
class MinAreaBoundigBox {
    Point m_axis;    
    long double m_bottom = 0.0l, m_right = 0.0l;
public:
    
    // Polygons can be convex or simple (convex or concave with possible holes)
    enum PolygonLevel {
        pcConvex, pcSimple
    };
   
    // Constructors with various types of geometry data used in Slic3r.
    // If the convexity is known apriory, pcConvex can be used to skip
    // convex hull calculation.
    explicit MinAreaBoundigBox(const Polygon&, PolygonLevel = pcSimple);
    explicit MinAreaBoundigBox(const ExPolygon&, PolygonLevel = pcSimple);
    explicit MinAreaBoundigBox(const Points&, PolygonLevel = pcSimple);
    
    // Returns the angle in radians needed for the box to be aligned with the 
    // X axis. Rotate the polygon by this angle and it will be aligned.
    double angle_to_X()  const;
    
    // The box width
    long double width()  const;
    
    // The box height
    long double height() const;
    
    // The box area
    long double area()   const;
    
    // The axis of the rotated box. If the angle_to_X is not sufficiently 
    // precise, use this unnormalized direction vector.
    const Point& axis()  const { return m_axis; }
};

}

#endif // MINAREABOUNDINGBOX_HPP
