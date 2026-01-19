#include "BridgeDetector.hpp"
#include "ClipperUtils.hpp"
#include "Geometry.hpp"
#include <algorithm>

namespace Slic3r {

BridgeDetector::BridgeDetector(
    ExPolygon         _expolygon,
    const ExPolygons &_lower_slices, 
    coord_t           _spacing) :
    // The original infill polygon, not inflated.
    expolygons(expolygons_owned),
    // All surfaces of the object supporting this region.
    lower_slices(_lower_slices),
    spacing(_spacing)
{
    this->expolygons_owned.push_back(std::move(_expolygon));
    initialize();
}

BridgeDetector::BridgeDetector(
    const ExPolygons  &_expolygons,
    const ExPolygons  &_lower_slices,
    coord_t            _spacing) : 
    // The original infill polygon, not inflated.
    expolygons(_expolygons),
    // All surfaces of the object supporting this region.
    lower_slices(_lower_slices),
    spacing(_spacing)
{
    initialize();
}

void BridgeDetector::initialize()
{
    // 5 degrees stepping
    this->resolution = PI/36.0; 
    // output angle not known
    this->angle = -1.;

    // Outset our bridge by an arbitrary amout; we'll use this outer margin for detecting anchors.
    Polygons grown = offset(this->expolygons, float(this->spacing));
    
    // Detect possible anchoring edges of this bridging region.
    // Detect what edges lie on lower slices by turning bridge contour and holes
    // into polylines and then clipping them with each lower slice's contour.
    // Currently _edges are only used to set a candidate direction of the bridge (see bridge_direction_candidates()).
	Polygons contours;
	contours.reserve(this->lower_slices.size());
	for (const ExPolygon &expoly : this->lower_slices)
		contours.push_back(expoly.contour);
    this->_edges = intersection_pl(to_polylines(grown), contours);
    
    #ifdef SLIC3R_DEBUG
    printf("  bridge has %zu support(s)\n", this->_edges.size());
    #endif
    
    // detect anchors as intersection between our bridge expolygon and the lower slices
    // safety offset required to avoid Clipper from detecting empty intersection while Boost actually found some edges
    this->_anchor_regions = intersection_ex(grown, union_safety_offset(this->lower_slices));
    
    /*
    if (0) {
        require "Slic3r/SVG.pm";
        Slic3r::SVG::output("bridge.svg",
            expolygons      => [ $self->expolygon ],
            red_expolygons  => $self->lower_slices,
            polylines       => $self->_edges,
        );
    }
    */
}

bool BridgeDetector::detect_angle(double bridge_direction_override)
{
    if (this->_edges.empty() || this->_anchor_regions.empty()) 
        // The bridging region is completely in the air, there are no anchors available at the layer below.
        return false;

    std::vector<BridgeDirection> candidates;
    if (bridge_direction_override == 0.) {
        std::vector<double> angles = bridge_direction_candidates();
        candidates.reserve(angles.size());
        for (size_t i = 0; i < angles.size(); ++ i)
            candidates.emplace_back(BridgeDirection(angles[i]));
    } else
        candidates.emplace_back(BridgeDirection(bridge_direction_override));
    
    /*  Outset the bridge expolygon by half the amount we used for detecting anchors;
        we'll use this one to clip our test lines and be sure that their endpoints
        are inside the anchors and not on their contours leading to false negatives. */
    Polygons clip_area = offset(this->expolygons, 0.5f * float(this->spacing));
    
    /*  we'll now try several directions using a rudimentary visibility check:
        bridge in several directions and then sum the length of lines having both
        endpoints within anchors */
        
    bool have_coverage = false;
    for (size_t i_angle = 0; i_angle < candidates.size(); ++ i_angle)
    {
        const double angle = candidates[i_angle].angle;

        Lines lines;
        {
            // Get an oriented bounding box around _anchor_regions.
            BoundingBox bbox = get_extents_rotated(this->_anchor_regions, - angle);
            // Cover the region with line segments.
            lines.reserve((bbox.max(1) - bbox.min(1) + this->spacing) / this->spacing);
            double s = sin(angle);
            double c = cos(angle);
            //FIXME Vojtech: The lines shall be spaced half the line width from the edge, but then 
            // some of the test cases fail. Need to adjust the test cases then?
//            for (coord_t y = bbox.min(1) + this->spacing / 2; y <= bbox.max(1); y += this->spacing)
            for (coord_t y = bbox.min(1); y <= bbox.max(1); y += this->spacing)
                lines.push_back(Line(
                    Point((coord_t)round(c * bbox.min(0) - s * y), (coord_t)round(c * y + s * bbox.min(0))),
                    Point((coord_t)round(c * bbox.max(0) - s * y), (coord_t)round(c * y + s * bbox.max(0)))));
        }

        double total_length = 0;
        double max_length = 0;
        {
            Lines clipped_lines = intersection_ln(lines, clip_area);
            size_t archored_line_num = 0;
            for (size_t i = 0; i < clipped_lines.size(); ++i) {
                const Line &line = clipped_lines[i];
                if (expolygons_contain(this->_anchor_regions, line.a) && expolygons_contain(this->_anchor_regions, line.b)) {
                    // This line could be anchored.
                    double len = line.length();
                    total_length += len;
                    max_length = std::max(max_length, len);
                    archored_line_num++;
                }
            }
            if (clipped_lines.size() > 0 && archored_line_num > 0) {
                candidates[i_angle].archored_percent = (double)archored_line_num / (double)clipped_lines.size();
            }
        }
        if (total_length == 0.)
            continue;

        have_coverage = true;
        // Sum length of bridged lines.
        candidates[i_angle].coverage = total_length;
        /*  The following produces more correct results in some cases and more broken in others.
            TODO: investigate, as it looks more reliable than line clipping. */
        // $directions_coverage{$angle} = sum(map $_->area, @{$self->coverage($angle)}) // 0;
        // max length of bridged lines
        candidates[i_angle].max_length = max_length;
    }

    // if no direction produced coverage, then there's no bridge direction
    if (! have_coverage)
        return false;
    
    // sort directions by coverage - most coverage first
    std::sort(candidates.begin(), candidates.end());
    
    // if any other direction is within extrusion width of coverage, prefer it if shorter
    // TODO: There are two options here - within width of the angle with most coverage, or within width of the currently perferred?
    size_t i_best = 0;
//    for (size_t i = 1; i < candidates.size() && abs(candidates[i_best].archored_percent - candidates[i].archored_percent) < EPSILON; ++ i)
    for (size_t i = 1; i < candidates.size() && candidates[i_best].coverage - candidates[i].coverage < this->spacing; ++ i)
        if (candidates[i].max_length < candidates[i_best].max_length)
            i_best = i;

    this->angle = candidates[i_best].angle;
    if (this->angle >= PI)
        this->angle -= PI;
    
    #ifdef SLIC3R_DEBUG
    printf("  Optimal infill angle is %d degrees\n", (int)Slic3r::Geometry::rad2deg(this->angle));
    #endif
    
    return true;
}

std::vector<double> BridgeDetector::bridge_direction_candidates() const
{
    // we test angles according to configured resolution
    std::vector<double> angles;
    for (int i = 0; i <= PI/this->resolution; ++i)
        angles.push_back(i * this->resolution);
    
    // we also test angles of each bridge contour
    {
        Lines lines = to_lines(this->expolygons);
        for (Lines::const_iterator line = lines.begin(); line != lines.end(); ++line)
            angles.push_back(line->direction());
    }
    
    /*  we also test angles of each open supporting edge
        (this finds the optimal angle for C-shaped supports) */
    for (const Polyline &edge : this->_edges)
        if (edge.first_point() != edge.last_point())
            angles.push_back(Line(edge.first_point(), edge.last_point()).direction());
    
    // remove duplicates
    double min_resolution = PI/180.0;  // 1 degree
    std::sort(angles.begin(), angles.end());
    for (size_t i = 1; i < angles.size(); ++i) {
        if (Slic3r::Geometry::directions_parallel(angles[i], angles[i-1], min_resolution)) {
            angles.erase(angles.begin() + i);
            --i;
        }
    }
    /*  compare first value with last one and remove the greatest one (PI) 
        in case they are parallel (PI, 0) */
    if (Slic3r::Geometry::directions_parallel(angles.front(), angles.back(), min_resolution))
        angles.pop_back();

    return angles;
}

/*
static void get_trapezoids(const ExPolygon &expoly, Polygons* polygons) const
{
    ExPolygons expp;
    expp.push_back(expoly);
    boost::polygon::get_trapezoids(*polygons, expp);
}

void ExPolygon::get_trapezoids(ExPolygon clone, Polygons* polygons, double angle) const
{
    clone.rotate(PI/2 - angle, Point(0,0));
    clone.get_trapezoids(polygons);
    for (Polygons::iterator polygon = polygons->begin(); polygon != polygons->end(); ++polygon)
        polygon->rotate(-(PI/2 - angle), Point(0,0));
}
*/

// This algorithm may return more trapezoids than necessary
// (i.e. it may break a single trapezoid in several because
// other parts of the object have x coordinates in the middle)
static void get_trapezoids2(const ExPolygon& expoly, Polygons* polygons)
{
    Polygons     src_polygons = to_polygons(expoly);
    // get all points of this ExPolygon
    const Points pp = to_points(src_polygons);

    // build our bounding box
    BoundingBox bb(pp);

    // get all x coordinates
    std::vector<coord_t> xx;
    xx.reserve(pp.size());
    for (Points::const_iterator p = pp.begin(); p != pp.end(); ++p)
        xx.push_back(p->x());
    std::sort(xx.begin(), xx.end());

    // find trapezoids by looping from first to next-to-last coordinate
    Polygons rectangle;
    rectangle.emplace_back(Polygon());
    for (std::vector<coord_t>::const_iterator x = xx.begin(); x != xx.end()-1; ++x) {
        coord_t next_x = *(x + 1);
        if (*x != next_x) {
            // intersect with rectangle
            // append results to return value
            rectangle.front() = { { *x, bb.min.y() }, { next_x, bb.min.y() }, { next_x, bb.max.y() }, { *x, bb.max.y() } };
            polygons_append(*polygons, intersection(rectangle, src_polygons));
        }
    }
}

static void get_trapezoids2(const ExPolygon &expoly, Polygons* polygons, double angle)
{
    ExPolygon clone = expoly;
    clone.rotate(PI/2 - angle, Point(0,0));
    get_trapezoids2(clone, polygons);
    for (Polygon &polygon : *polygons)
        polygon.rotate(-(PI/2 - angle), Point(0,0));
}



void get_trapezoids3_half(const ExPolygon& expoly, Polygons* polygons, float spacing)
{

    // get all points of this ExPolygon
    Points pp = to_points(expoly);

    if (pp.empty()) return;

    // build our bounding box
    BoundingBox bb(pp);

    // get all x coordinates
    coord_t min_x = pp[0].x(), max_x = pp[0].x();
    std::vector<coord_t> xx;
    for (Points::const_iterator p = pp.begin(); p != pp.end(); ++p) {
        if (min_x > p->x()) min_x = p->x();
        if (max_x < p->x()) max_x = p->x();
    }
    for (coord_t x = min_x; x < max_x - (coord_t)(spacing / 2); x += (coord_t)spacing) {
        xx.push_back(x);
    }
    xx.push_back(max_x);
    //std::sort(xx.begin(), xx.end());

    // find trapezoids by looping from first to next-to-last coordinate
    for (std::vector<coord_t>::const_iterator x = xx.begin(); x != xx.end() - 1; ++x) {
        coord_t next_x = *(x + 1);
        if (*x == next_x) continue;

        // build rectangle
        Polygon poly;
        poly.points.resize(4);
        poly[0].x() = *x + (coord_t)spacing / 4;
        poly[0].y() = bb.min(1);
        poly[1].x() = next_x - (coord_t)spacing / 4;
        poly[1].y() = bb.min(1);
        poly[2].x() = next_x - (coord_t)spacing / 4;
        poly[2].y() = bb.max(1);
        poly[3].x() = *x + (coord_t)spacing / 4;
        poly[3].y() = bb.max(1);

        // intersect with this expolygon
        // append results to return value
        polygons_append(*polygons, intersection(Polygons{ poly }, to_polygons(expoly)));
    }
}

Polygons BridgeDetector::coverage(double angle, bool precise) const
{
    if (angle == -1)
        angle = this->angle;

    Polygons covered;

    if (angle != -1) {
        // Get anchors, convert them to Polygons and rotate them.
        Polygons anchors = to_polygons(this->_anchor_regions);
        polygons_rotate(anchors, PI / 2.0 - angle);
        //same for region which do not need bridging
        //Polygons supported_area = diff(this->lower_slices.expolygons, this->_anchor_regions, true);
        //polygons_rotate(anchors, PI / 2.0 - angle);

        for (ExPolygon expolygon : this->expolygons) {
            // Clone our expolygon and rotate it so that we work with vertical lines.
            expolygon.rotate(PI / 2.0 - angle);
            // Outset the bridge expolygon by half the amount we used for detecting anchors;
            // we'll use this one to generate our trapezoids and be sure that their vertices
            // are inside the anchors and not on their contours leading to false negatives.
            for (ExPolygon &expoly : offset_ex(expolygon, 0.5f * float(this->spacing))) {
                // Compute trapezoids according to a vertical orientation
                Polygons trapezoids;
                if (!precise) get_trapezoids2(expoly, &trapezoids, PI / 2);
                else get_trapezoids3_half(expoly, &trapezoids, float(this->spacing));
                for (Polygon &trapezoid : trapezoids) {
                    size_t n_supported = 0;
                    if (!precise) {
                        // not nice, we need a more robust non-numeric check
                        // imporvment 1: take into account when we go in the supported area.
                        for (const Line &supported_line : intersection_ln(trapezoid.lines(), anchors))
                            if (supported_line.length() >= this->spacing)
                                ++n_supported;
                    } else {
                        Polygons intersects = intersection(Polygons{trapezoid}, anchors);
                        n_supported = intersects.size();

                        if (n_supported >= 2) {
                            // trim it to not allow to go outside of the intersections
                            BoundingBox center_bound = intersects[0].bounding_box();
                            coord_t min_y = center_bound.center()(1), max_y = center_bound.center()(1);
                            for (Polygon &poly_bound : intersects) {
                                center_bound = poly_bound.bounding_box();
                                if (min_y > center_bound.center()(1)) min_y = center_bound.center()(1);
                                if (max_y < center_bound.center()(1)) max_y = center_bound.center()(1);
                            }
                            coord_t min_x = trapezoid[0](0), max_x = trapezoid[0](0);
                            for (Point &p : trapezoid.points) {
                                if (min_x > p(0)) min_x = p(0);
                                if (max_x < p(0)) max_x = p(0);
                            }
                            //add what get_trapezoids3 has removed (+EPSILON)
                            min_x -= (this->spacing / 4 + 1);
                            max_x += (this->spacing / 4 + 1);
                            coord_t mid_x = (min_x + max_x) / 2;
                            for (Point &p : trapezoid.points) {
                                if (p(1) < min_y) p(1) = min_y;
                                if (p(1) > max_y) p(1) = max_y;
                                if (p(0) > min_x && p(0) < mid_x) p(0) = min_x;
                                if (p(0) < max_x && p(0) > mid_x) p(0) = max_x;
                            }
                        }
                    }

                    if (n_supported >= 2) {
                        //add it
                        covered.push_back(std::move(trapezoid));
                    }
                }
            }
        }

        // Unite the trapezoids before rotation, as the rotation creates tiny gaps and intersections between the trapezoids
        // instead of exact overlaps.
        covered = union_(covered);
        // Intersect trapezoids with actual bridge area to remove extra margins and append it to result.
        polygons_rotate(covered, -(PI/2.0 - angle));
        //covered = intersection(this->expolygons, covered);
#if 0
        {
            my @lines = map @{$_->lines}, @$trapezoids;
            $_->rotate(-(PI/2 - $angle), [0,0]) for @lines;
            
            require "Slic3r/SVG.pm";
            Slic3r::SVG::output(
                "coverage_" . rad2deg($angle) . ".svg",
                expolygons          => [$self->expolygon],
                green_expolygons    => $self->_anchor_regions,
                red_expolygons      => $coverage,
                lines               => \@lines,
            );
        }
#endif
    }
    return covered;
}

/*  This method returns the bridge edges (as polylines) that are not supported
    but would allow the entire bridge area to be bridged with detected angle
    if supported too */
void
BridgeDetector::unsupported_edges(double angle, Polylines* unsupported) const
{
    if (angle == -1) angle = this->angle;
    if (angle == -1) return;

    Polygons grown_lower = offset(this->lower_slices, float(this->spacing));

    for (ExPolygons::const_iterator it_expoly = this->expolygons.begin(); it_expoly != this->expolygons.end(); ++ it_expoly) {    
        // get unsupported bridge edges (both contour and holes)
        Lines unsupported_lines = to_lines(diff_pl(to_polylines(*it_expoly), grown_lower));
        /*  Split into individual segments and filter out edges parallel to the bridging angle
            TODO: angle tolerance should probably be based on segment length and flow width,
            so that we build supports whenever there's a chance that at least one or two bridge
            extrusions would be anchored within such length (i.e. a slightly non-parallel bridging
            direction might still benefit from anchors if long enough)
            double angle_tolerance = PI / 180.0 * 5.0; */
        for (const Line &line : unsupported_lines)
            if (! Slic3r::Geometry::directions_parallel(line.direction(), angle)) {
                unsupported->emplace_back(Polyline());
                unsupported->back().points.emplace_back(line.a);
                unsupported->back().points.emplace_back(line.b);
            }
    }
    
    /*
    if (0) {
        require "Slic3r/SVG.pm";
        Slic3r::SVG::output(
            "unsupported_" . rad2deg($angle) . ".svg",
            expolygons          => [$self->expolygon],
            green_expolygons    => $self->_anchor_regions,
            red_expolygons      => union_ex($grown_lower),
            no_arrows           => 1,
            polylines           => \@bridge_edges,
            red_polylines       => $unsupported,
        );
    }
    */
}

Polylines
BridgeDetector::unsupported_edges(double angle) const
{
    Polylines pp;
    this->unsupported_edges(angle, &pp);
    return pp;
}

}
