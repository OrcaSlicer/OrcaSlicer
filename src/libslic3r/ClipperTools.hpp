#ifndef slic3r_ClipperTools_hpp_
#define slic3r_ClipperTools_hpp_

#include "libslic3r.h"
#include "clipper.hpp"
#include "Point.hpp"
#include "Line.hpp"
#include "Polyline.hpp"
#include "Polygon.hpp"
#include "libslic3r/Arachne/utils/ExtrusionJunction.hpp"

/*
---------------------------------------------------------------------------
Common functions for converting Clipper geometry
The basis has been proposed by @pi-squared-studio for Orca Slicer
They may need to be standardized and assigned to their respective classes.
Some conversion functions work with data loss due to their nature of description.
The reverse conversion does not always give an equivalent result, so be sure that these losses will not affect the one.

Attention: This library may contain some errors or unexpected results, so please check the correctness of these functions by debug drawings!

---------------------------------------------------------------------------

Basic principles of converting lines to polylines and vice versa:

Polyline:                                                    Polylines:
          a      b      c      d                           a      b      c      d
coords -  *------*------*------*--- ...                    *------*      *------*--- ...
          r1     r2     r3    r4                              l1r1          l2r1   l2r2

Lines (without the gaps, it is perceived as a polyline):     Lines (with a gap within end coordinates):
          a1   b1=b2  c1=c2  d1=d2                         a      b      c    d1=d2
coords -  *------**-----**-----**-- ...                    *------*      *-----**--- ...
             l1     l2     l3     l4                         l1            l2     l3


Basic principles of converting lines to polylines with equal thickness and vice versa

ThickPolyline with equal widths:           ThickPolyline with different widths:     ThickPolylines:
          a      b      c      d           a      b      c      d                   a      b      c      d
coords -  *------*------*------*--- ...    *------*------*------*--- ...            *------*      *------*--- ...
widths -  *-----*-*----*-*----*-*--        *-----*-*----*-*----*-*--                *------*      *-----*-*--
          w1   w1=w1  w1=w1  w1=w1         w1   w1 w2  w2 w3  w4=w4                 w1     w1     w1   w1=w1

Lines:                                                                              Lines (with the gaps. Zero width (w0) = 0):
          a1   b1=b2  c1=c2  d1=d2         a1    b1=b2  c1=c2  d1=d2                a1    b1=b2    c1=c2  d1=d2
coords -  *------**-----**-----**-- ...    *------**-----**-----**-- ...            *------**-------**-----**-- ...
widths -  *------**-----**-----**--        *------**-----**-----**--                *------**_______**-----**-- ...
          w1   w1=w1  w1=w1   w1=w1        w1    w1 w2  w2 w3  w4=w4                w1    w1 w0 = w0 w1   w1=w1

Extrusion junctions:                       Extrusion junctions with diff. widths:   Extrusion junctions (with the gaps. Zero width (w0) = 1 
                                                                                                           is a minimal extrusion line):
          a      b      c      d           a    b1=b2  c1=c2  d1=d2                 a1    b1=b2    c1=c2    d1
coords -  *------*------*------*--- ...    *-----*-*----*-*----*-*--- ...           *-----*--*-----*--*-----*-- ...
widths -  *------*------*------*---        *-----*-*----*-*----*-*---               *-----*-_*_____*_-*-----*-- ...
          w1     w1     w1     w1          w1   w1 w2  w2 w3  w4=w4                 w1   w1  w0 = w0  w1    w1

3D polyline copies the Extrusion junctions logic, except that a zero line width (w0) is equal to 0.
The third coordinate in this system represents the width of the line.
This conversion is quite useful, for ex. for simplifying thick lines.
You can use the z_factor (width_factor) parameter to filter points by width more accurately.

---------------------------------------------------------------------------

Introducing an extrusion line representation allows you to lossless transform the path, for example, when reversing.
All closed lines in all represent systems have the same coordinates of endpoints.
No additional sending paraneters is required if this condition is met.
Parameter 'close': You can force the line to become closed without creating unnecessary elements.
Parameter 'separate_lines': Forcibly splits (thick)lines into polylines if necessary and according to the above rule.

---------------------------------------------------------------------------
    
*** Contours ***
The concept of contour is also introduced.
This is a closed polyline or a set of them (polylines) that always describe the outer shell.
When converting, it is possible to created multiple contortus from a single one, but these are will be treated as a single
contour when used later. Contours can have a rotation direction that defines the outer and inner (hole) shapes, but often
requires positive logic for Clipper conversion. Since the definition of area rotation can vary between systems (for example,
Clipper and Slicer Model's Path), a clear understanding of how such a procedure will work is required.
Contour conversion functions must always restore the original traversal direction for their results.

---------------------------------------------------------------------------
*/

using Slic3r::ClipperLib::jtMiter;
using Slic3r::ClipperLib::jtRound;
using Slic3r::ClipperLib::jtSquare;
using Lines = std::vector<Slic3r::Line>;
using ThickLine = std::vector<Slic3r::ThickLines>;
using ThickPolylines = std::vector<Slic3r::ThickPolyline>;
using Line = Slic3r::Line;
using ThickPolyline = Slic3r::ThickPolyline;

namespace Slic3r {

// part of corecrt_math_defines.h
#define M_PI_8      0.392699081698724154807 // pi/8
#define M_2PI       6.283185307179586476925 // 2*pi
#define M_3PI_4     2.356194490192344928847 // 3*pi/4

// Normalizing the numerical value of the angle from 0 to pi (0...360 degrees)
double constrain2PI(double x);

// Normalizing the numerical value of the angle from -pi/2 to pi/2 (-180...180 degrees)
double constrainPI(double x);

// Convert lines into polyline.
Polyline to_polyline(const Lines& in);

// Convert thick lines into polyline.
Polyline to_polyline(const ThickLines& in);

// Convert extrusion into polyline.
//  close:      forced closing of the line.
Polyline to_polyline(const Arachne::ExtrusionJunctions& in, bool close = false);

// Convert extrusion into polyline with reversing.
//  close:      forced closing of the line.
Polyline to_polyline_reverse(const Arachne::ExtrusionJunctions& in, bool close = false);

// Convert thick lines into thick polyline.
ThickPolyline to_thick_polyline(const ThickLines& in);

// Convert 3D-polyline into thick polyline.
//  z_factor:   the accuracy of calculating the Z axis relative to XY
//  close:      forced closing of the line.
ThickPolyline to_thick_polyline(const Polyline3& in, const double z_factor = 1., bool close = false);

// Convert extrusion line into thick polylines (new quick implementation).
//  close:      forced closing of the line.
ThickPolyline to_thick_polyline(const Arachne::ExtrusionJunctions& in, bool close = false);

// Convert extrusion line into thick polylines (new quick implementation) with reversing.
//  close:      forced closing of the line.
ThickPolyline to_thick_polyline_reverse(const Arachne::ExtrusionJunctions& in, bool close = false);

// Convert thick lines into thick polylines.
//  separate_lines:     split continuous lines into separate.
ThickPolylines to_thick_polylines(const ThickLines& in, bool separate_lines = true);

// Convert thick polyline into thick lines
ThickLines to_thick_lines(ThickPolyline& in);

// Convert polyline into thick lines
// 	width:      width of the line;
//  close:      forced closing of the line.
ThickLines to_thick_lines(const Polyline& in, coordf_t width, bool close = false);

// Convert polylines into thick lines
// 	width:      width of the line;
//  close:      forced closing of the line.
ThickLines to_thick_lines(const Polylines& in, coordf_t width, bool close = false);

// Convert 3D-polyline into thick lines.
//  z_factor:   the accuracy of calculating the Z axis relative to XY;
//  close:      forced closing of the line.
ThickLines to_thick_lines(const Polyline3& in, const double z_factor = 1., bool close = false, bool separate_lines = true);

// Convert extrusion into thicklines.
//  close:      forced closing of the line.
ThickLines to_thick_lines(const Arachne::ExtrusionJunctions& in, bool close = false);

// Convert extrusion into thicklines with reversing.
//  close:      forced closing of the line.
ThickLines to_thick_lines_reverse(const Arachne::ExtrusionJunctions& in, bool close = false);

// Convert thick polyline into thick lines.
// !! Use the thickpolyline.thicklines() function for quick response.
inline ThickLines to_thick_lines(ThickPolyline& in)
    { return in.thicklines(); };

// Convert thick lines into 3D polylines.
//  z_factor:       the accuracy of calculating the Z axis relative to XY;
//  separate_lines: split continuous lines into separate.
Polyline3 to_polyline3(const ThickLines& in, const double z_factor = 1., bool separate_lines = true);

// Convert thick polylines into 3D polylines.
//  z_factor:       the accuracy of calculating the Z axis relative to XY;
Polyline3 to_polyline3(const ThickPolyline& in, const double z_factor = 1.);

// Convert polylines into extrusion.
//  index:      index of the extrusion line;
//  width:      width of the line;
//  reverse_lines:  reverse lines.
Arachne::ExtrusionJunctions to_extrusion(const Polyline& in, size_t width, size_t index, bool reverse_lines = false);

// Convert polylines into extrusion.
//  index:      index of the extrusion line;
//  width:      width of the line;
//  reverse_lines:  reverse lines;
//  separate_lines: split continuous lines into separate.
Arachne::ExtrusionJunctions to_extrusion(const Polylines& in, size_t width, size_t index, bool reverse_lines = false, bool separate_lines = true);

// Convert thick lines into extrusion.
//  index:      index of the extrusion line;
//  width:      width of the line;
//  reverse_lines:  reverse extrusion lines;
//  separate_lines: split continuous lines into separate.
Arachne::ExtrusionJunctions to_extrusion(const ThickLines& in, size_t index, bool reverse_lines = false, bool separate_lines = true);

// Convert thick polyline into extrusion.
//  index:      index of the extrusion line.
Arachne::ExtrusionJunctions to_extrusion(const ThickPolyline& in, size_t index);

// Convert thick polylines into extrusion.
//  index:      index of the extrusion line;
//  separate_lines: split continuous lines into separate.
Arachne::ExtrusionJunctions to_extrusion(const ThickPolylines& in, size_t index, bool separate_lines = true);

// Reverse thick lines.
ThickLines reverse(const ThickLines& in);

// Check polyline in the CW direction.
bool is_clockwise(const Polyline& in);

// Check polyline in the CCW direction.
bool is_counter_clockwise(const Polyline& in);

// Check thick lines in the CW direction.
bool is_clockwise(const ThickLines& in);

// Check thick lines in the CCW direction.
bool is_counter_clockwise(const ThickLines& in);

// Make polyline in the CW direction.
Polyline make_clockwise(const Polyline& in);

// Make polyline in the CCW direction.
Polyline make_counter_clockwise(const Polyline& in);

// Make all polylines in the CW direction.
Polylines make_clockwise(const Polylines& in);

// Make all polylines in the CCW direction.
Polylines make_counter_clockwise(const Polylines& in);

// Get longest polyline among same direction.
//  invert: false is CCW, true is CW.
Polyline get_longest(const Polylines& in, bool invert = false);

// Get longest thick polyline among same direction.
//  invert: false is CCW, true is CW.
ThickPolyline get_longest(const ThickPolylines& in, bool invert = false);

// Get polylines that has CW direction.
Polylines get_clockwise(const Polylines& in);

// Get polylines that has CCW direction.
Polylines get_counter_clockwise(const Polylines& in);

// Simplify thick lines.
//  tolerance:      maximum deviation to the simplified element;
//  width_factor:   the accuracy of width calculating.
ThickLines simplify(const ThickLines& in, double tolerance = SCALED_EPSILON, const double width_factor = 1);

// Simplify thick polyline.
//  tolerance:      maximum deviation to the simplified element;
//  width_factor:   the accuracy of width calculating.
ThickPolyline simplify(const ThickPolyline& in, double tolerance = SCALED_EPSILON, const double width_factor = 1);

// Simplify thick polylines.
//  tolerance:      maximum deviation to the simplified element;
//  width_factor:   the accuracy of width calculating.
ThickPolylines simplify(const ThickPolylines& in, double tolerance = SCALED_EPSILON, const double width_factor = 1);

// Checking for the first intersection a given line of a contour.
// If there are several intersection points, you can search for the first point from the back of the line.
// The result of applying the function will be a bool determination of whether such a crossing occurs.
// The coordinates of the found point are passed through the argument intersection.
//  intersection:   the ptr of the finding point;
//  from_back:      direction of the check.
bool contour_intersection(Line line, Polylines polylines, Point* intersection, bool from_back = false);

// Checking whether a point is contains in a contour.
bool contour_contains(Polylines& polylines, Point& point);

/*
FILTERING:
Short curls are artifacts that remain as a result of applying the polylines blending or polygon union/intersection/clippng procedure.
Since the perimeter length of such curls is usually a multiple of the offset ramge, an effective filtering function can be applied.
The entire polyline is viewed, and if there is an self-intersection at a certain length, then this segment is cut out of the polyline.

Orign polyline:      Negative offsetted line:         Cropped lines:

 ---------------+                       |\ d
                |                       | \
                |        a             b|  \            a               b
                |   ->   ---------------*---' c   ->    ---------------+
                |                       |                              |
                |                       | e                            | e

Let's assume that the results polyline extends from 'a' point.
All short self-intersections are checked for the value of curl_length starting from 'c' point.
If the positive value of 'curl_length' exceeds the total distance of the segments 'cd'+'db',
the extra loop 'cdb' will be rejected. For most cases, this parameter should be 3-4 times the offset length. When the 'curl_length' parameter
is negative, overap range 'cd' will not be account. A distance of 1-2 times of absolute value 'curl_length' will be enough.
But for larger angles, an even greater value will be required, which in turn can affect the quality of the resulting shape.
If several intersections are found at this length, the subsequent line construction will occur from the last point.
*/


// Thick lines filtering procedure for cutting short curls
//  curl_length:
//      if positive, the length of the curl with overlap,
//      if negative, the absolute length of the curl without overlap.
ThickLines filter_curls_reject(ThickLines in, double curl_length = 0);

// Lines filtering procedure for cutting short curls
//  curl_length:
//      if positive, the length of the curl with overlap,
//      if negative, the absolute length of the curl without overlap.
Lines filter_curls_reject(Lines in, double curl_length = 0);

// Polyline filtering procedure for cutting short curls
//  curl_length: 
//      if positive, the length of the curl with overlap,
//      if negative, the absolute length of the curl without overlap.
Polyline filter_curls_reject(const Polyline& in, double curl_length = 0);

// Complex filtering  procedure
// It is possible that this procedure is redundant in its algo and may contain incorrect execution for very complex contours.
// The function accepts only one contour.
// Double offset filtering and trimming of small contours are available.
// The output of the function is a set of contours rotated in the same direction as the input line.
// Contours in the opposite direction show the cut lines, which can be used to calculate the filling of the gaps.
Polylines filter_contour(Polyline in,
                         Polygon crop_contour           = Polygon(),
                         const double filter_offset     = 0,
                         const double filter_perimeter  = 0,
                         const double filter_area       = 0,
                         const double tolerance         = SCALED_EPSILON);
ThickLines filter_by_crop_contour(const ThickLines& in, Polylines contour, double curl_length = 0);

// The procedure for filtering polylines through a shaded polygon.
ThickPolylines filter_by_crop_contour(const ThickPolyline& in, Polylines contour, double curl_length = 0);

// The procedure for filtering polylines through a shaded polygon.
Polyline filter_by_polygon(Polyline in, const double filter_offset);

//  Quick polyline offset with filtering from the resulting curls
Polyline offset_(const Polyline& in, const double delta, double curl_length = 0);

//  Quick polylines offset with filtering from the resulting curls
Polylines offset_(const Polylines& in, const double delta, double curl_length = 0);

// Guaranteed filtering of the contour (closed polyline) through a shaded polygon.
Polyline offset_by_polygon(const Polyline& in, const double delta, ClipperLib::JoinType joinType = jtMiter);

// Guaranteed filtering of the contours (closed polylines) through a shaded polygon.
Polylines offset_by_polygon(const Polylines& in, const double delta, ClipperLib::JoinType joinType = jtMiter);

// Combines two polylines into one
Polyline combine(Polyline polyline1, Polyline polyline2);

// Combines three contours (closed polylines).
// First, the union operation is performed, followed by the intersection operation in the required order.
Polylines combine_3(const Polylines& polylines1, Polylines polylines2 = Polylines(), Polylines polylines3 = Polylines());

// Combines two contours (closed polylines).
// First, the union operation is performed, followed by the intersection operation in the required order.
Polylines combine_2(const Polylines& polylines1, Polylines& polylines2);

} // namespace Slic3r

#endif // slic3r_ClipperTools_hpp_
