#include <stdlib.h>
#include <stdint.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>

#include <boost/container/small_vector.hpp>
#include <boost/log/trivial.hpp>
#include <boost/static_assert.hpp>
#include <boost/math/constants/constants.hpp>

#include "../ClipperUtils.hpp"
#include "../ExPolygon.hpp"
#include "../Geometry.hpp"
#include "../Surface.hpp"
#include "../ShortestPath.hpp"
#include "../VariableWidth.hpp"

#include "FillRectilinear.hpp"

// #define SLIC3R_DEBUG
// #define INFILL_DEBUG_OUTPUT

// Make assert active if SLIC3R_DEBUG
#ifdef SLIC3R_DEBUG
    #undef NDEBUG
    #include "SVG.hpp"
#endif

#if defined(SLIC3R_DEBUG) || defined(INFILL_DEBUG_OUTPUT)
    #include "SVG.hpp"
#endif

#include <cassert>

// We want our version of assert.
#include "../libslic3r.h"

namespace Slic3r {

// Having a segment of a closed polygon, calculate its Euclidian length.
// The segment indices seg1 and seg2 signify an end point of an edge in the forward direction of the loop,
// therefore the point p1 lies on poly.points[seg1-1], poly.points[seg1] etc.
static inline coordf_t segment_length(const Polygon &poly, size_t seg1, const Point &p1, size_t seg2, const Point &p2)
{
#ifdef SLIC3R_DEBUG
    // Verify that p1 lies on seg1. This is difficult to verify precisely,
    // but at least verify, that p1 lies in the bounding box of seg1.
    for (size_t i = 0; i < 2; ++ i) {
        size_t seg = (i == 0) ? seg1 : seg2;
        Point  px  = (i == 0) ? p1   : p2;
        Point  pa  = poly.points[((seg == 0) ? poly.points.size() : seg) - 1];
        Point  pb  = poly.points[seg];
        if (pa(0) > pb(0))
            std::swap(pa(0), pb(0));
        if (pa(1) > pb(1))
            std::swap(pa(1), pb(1));
        assert(px(0) >= pa(0) && px(0) <= pb(0));
        assert(px(1) >= pa(1) && px(1) <= pb(1));
    }
#endif /* SLIC3R_DEBUG */
    const Point *pPrev = &p1;
    const Point *pThis = NULL;
    coordf_t len = 0;
    if (seg1 <= seg2) {
        for (size_t i = seg1; i < seg2; ++ i, pPrev = pThis)
           len += (*pPrev - *(pThis = &poly.points[i])).cast<double>().norm();
    } else {
        for (size_t i = seg1; i < poly.points.size(); ++ i, pPrev = pThis)
           len += (*pPrev - *(pThis = &poly.points[i])).cast<double>().norm();
        for (size_t i = 0; i < seg2; ++ i, pPrev = pThis)
           len += (*pPrev - *(pThis = &poly.points[i])).cast<double>().norm();
    }
    len += (*pPrev - p2).cast<double>().norm();
    return len;
}

// Append a segment of a closed polygon to a polyline.
// The segment indices seg1 and seg2 signify an end point of an edge in the forward direction of the loop.
// Only insert intermediate points between seg1 and seg2.
static inline void polygon_segment_append(Points &out, const Polygon &polygon, size_t seg1, size_t seg2)
{
    if (seg1 == seg2) {
        // Nothing to append from this segment.
    } else if (seg1 < seg2) {
        // Do not append a point pointed to by seg2.
        out.insert(out.end(), polygon.points.begin() + seg1, polygon.points.begin() + seg2);
    } else {
        out.reserve(out.size() + seg2 + polygon.points.size() - seg1);
        out.insert(out.end(), polygon.points.begin() + seg1, polygon.points.end());
        // Do not append a point pointed to by seg2.
        out.insert(out.end(), polygon.points.begin(), polygon.points.begin() + seg2);
    }
}

// Append a segment of a closed polygon to a polyline.
// The segment indices seg1 and seg2 signify an end point of an edge in the forward direction of the loop,
// but this time the segment is traversed backward.
// Only insert intermediate points between seg1 and seg2.
static inline void polygon_segment_append_reversed(Points &out, const Polygon &polygon, size_t seg1, size_t seg2)
{
    if (seg1 >= seg2) {
        out.reserve(seg1 - seg2);
        for (size_t i = seg1; i > seg2; -- i)
            out.push_back(polygon.points[i - 1]);
    } else {
        // it could be, that seg1 == seg2. In that case, append the complete loop.
        out.reserve(out.size() + seg2 + polygon.points.size() - seg1);
        for (size_t i = seg1; i > 0; -- i)
            out.push_back(polygon.points[i - 1]);
        for (size_t i = polygon.points.size(); i > seg2; -- i)
            out.push_back(polygon.points[i - 1]);
    }
}

// Intersection point of a vertical line with a polygon segment.
struct SegmentIntersection
{
    // Index of a contour in ExPolygonWithOffset, with which this vertical line intersects.
    size_t      iContour { 0 };
    // Index of a segment in iContour, with which this vertical line intersects.
    size_t      iSegment { 0 };
    // y position of the intersection, rational number.
    int64_t     pos_p { 0 };
    uint32_t    pos_q { 1 };

    coord_t     pos() const {
        // Division rounds both positive and negative down to zero.
        // Add half of q for an arithmetic rounding effect.
        int64_t p = pos_p;
        if (p < 0)
            p -= int64_t(pos_q>>1);
        else
            p += int64_t(pos_q>>1);
        return coord_t(p / int64_t(pos_q)); 
    }

    // Left vertical line / contour intersection point.
    // null if next_on_contour_vertical.
    int32_t	prev_on_contour { 0 };
    // Right vertical line / contour intersection point.
    // If next_on_contour_vertical, then then next_on_contour contains next contour point on the same vertical line.
    int32_t	next_on_contour { 0 };

    // Kind of intersection. With the original contour, or with the inner offestted contour?
    // A vertical segment will be at least intersected by OUTER_LOW, OUTER_HIGH,
    // but it could be intersected with OUTER_LOW, INNER_LOW, INNER_HIGH, OUTER_HIGH,
    // and there may be more than one pair of INNER_LOW, INNER_HIGH between OUTER_LOW, OUTER_HIGH.
    enum SegmentIntersectionType : char {
        UNKNOWN,
        OUTER_LOW,
        OUTER_HIGH,
        INNER_LOW,
        INNER_HIGH,
    };
    SegmentIntersectionType type { UNKNOWN };

    enum class LinkType : uint8_t {
    	// Horizontal link (left or right).
    	Horizontal,
    	// Vertical link, up.
    	Up,
    	// Vertical link, down.
    	Down,
        // Phony intersection point has no link.
        Phony,
    };

    enum class LinkQuality : uint8_t {
    	Invalid,
        Valid,
    	// Valid link, but too long to be followed.
    	TooLong,
    };

    // Kept grouped with other booleans for smaller memory footprint.
    LinkType 		prev_on_contour_type { LinkType::Horizontal };
    LinkType 		next_on_contour_type { LinkType::Horizontal };
    LinkQuality 	prev_on_contour_quality { LinkQuality::Valid };
    LinkQuality 	next_on_contour_quality { LinkQuality::Valid };
    // Was this segment along the y axis consumed?
    // Up means up along the vertical segment.
    bool 	 		consumed_vertical_up { false };
    // Was a segment of the inner perimeter contour consumed?
    // Right means right from the vertical segment.
    bool 	 		consumed_perimeter_right { false };

    // For the INNER_LOW type, this point may be connected to another INNER_LOW point following a perimeter contour.
    // For the INNER_HIGH type, this point may be connected to another INNER_HIGH point following a perimeter contour.
    // If INNER_LOW is connected to INNER_HIGH or vice versa,
    // one has to make sure the vertical infill line does not overlap with the connecting perimeter line.
    bool 	is_inner() const { return type == INNER_LOW  || type == INNER_HIGH; }
    bool 	is_outer() const { return type == OUTER_LOW  || type == OUTER_HIGH; }
    bool 	is_low  () const { return type == INNER_LOW  || type == OUTER_LOW; }
    bool 	is_high () const { return type == INNER_HIGH || type == OUTER_HIGH; }

    enum class Side {
    	Left,
    	Right
    };
    enum class Direction {
    	Up,
    	Down
    };

    bool 	has_left_horizontal()    		 	const { return this->prev_on_contour_type == LinkType::Horizontal; }
    bool 	has_right_horizontal()   		 	const { return this->next_on_contour_type == LinkType::Horizontal; }
    bool 	has_horizontal(Side side)		 	const { return side == Side::Left ? this->has_left_horizontal() : this->has_right_horizontal(); }

    bool 	has_left_vertical_up()   		 	const { return this->prev_on_contour_type == LinkType::Up; }
    bool 	has_left_vertical_down() 		 	const { return this->prev_on_contour_type == LinkType::Down; }
    bool 	has_left_vertical(Direction dir) 	const { return dir == Direction::Up ? this->has_left_vertical_up() : this->has_left_vertical_down(); }
    bool 	has_left_vertical()    	 		 	const { return this->has_left_vertical_up() || this->has_left_vertical_down(); }
    bool 	has_left_vertical_outside()			const { return this->is_low() ? this->has_left_vertical_down() : this->has_left_vertical_up(); }

    bool 	has_right_vertical_up()   			const { return this->next_on_contour_type == LinkType::Up; }
    bool 	has_right_vertical_down() 			const { return this->next_on_contour_type == LinkType::Down; }
    bool 	has_right_vertical(Direction dir) 	const { return dir == Direction::Up ? this->has_right_vertical_up() : this->has_right_vertical_down(); }
    bool 	has_right_vertical()    			const { return this->has_right_vertical_up() || this->has_right_vertical_down(); }
    bool 	has_right_vertical_outside()		const { return this->is_low() ? this->has_right_vertical_down() : this->has_right_vertical_up(); }

    bool 	has_vertical()						const { return this->has_left_vertical() || this->has_right_vertical(); }
    bool 	has_vertical(Side side)				const { return side == Side::Left ? this->has_left_vertical() : this->has_right_vertical(); }
    bool 	has_vertical_up()					const { return this->has_left_vertical_up() || this->has_right_vertical_up(); }
    bool 	has_vertical_down()					const { return this->has_left_vertical_down() || this->has_right_vertical_down(); }
    bool 	has_vertical(Direction dir)			const { return dir == Direction::Up ? this->has_vertical_up() : this->has_vertical_down(); }

    int 	left_horizontal()  					const { return this->has_left_horizontal() 	? this->prev_on_contour : -1; }
    int 	right_horizontal()  				const { return this->has_right_horizontal() ? this->next_on_contour : -1; }
    int 	horizontal(Side side)  				const { return side == Side::Left ? this->left_horizontal() : this->right_horizontal(); }
    LinkQuality horizontal_quality(Side side)	const {
    	assert(this->has_horizontal(side));
    	return side == Side::Left ? this->prev_on_contour_quality : this->next_on_contour_quality;
    }

    int 	left_vertical_up()   		 		const { return this->has_left_vertical_up()    ? this->prev_on_contour : -1; }
    int 	left_vertical_down()   		 		const { return this->has_left_vertical_down()  ? this->prev_on_contour : -1; }
    int 	left_vertical(Direction dir) 		const { return (dir == Direction::Up ? this->has_left_vertical_up() : this->has_left_vertical_down()) ? this->prev_on_contour : -1; }
    int 	left_vertical()   			 		const { return this->has_left_vertical() 	   ? this->prev_on_contour : -1; }
    int 	left_vertical_outside()				const { return this->is_low() ? this->left_vertical_down() : this->left_vertical_up(); }
    int 	right_vertical_up()   		 		const { return this->has_right_vertical_up()   ? this->next_on_contour : -1; }
    int 	right_vertical_down()   	 		const { return this->has_right_vertical_down() ? this->next_on_contour : -1; }
    int 	right_vertical(Direction dir) 		const { return (dir == Direction::Up ? this->has_right_vertical_up() : this->has_right_vertical_down()) ? this->next_on_contour : -1; }
    int 	right_vertical()   			 		const { return this->has_right_vertical() 	   ? this->next_on_contour : -1; }
    int 	right_vertical_outside()			const { return this->is_low() ? this->right_vertical_down() : this->right_vertical_up(); }

    int 	vertical_up(Side side)				const { return side == Side::Left ? this->left_vertical_up() : this->right_vertical_up(); }
    int 	vertical_down(Side side)			const { return side == Side::Left ? this->left_vertical_down() : this->right_vertical_down(); }
    int 	vertical_outside(Side side)			const { return side == Side::Left ? this->left_vertical_outside() : this->right_vertical_outside(); }
    // Returns -1 if there is no link up.
    int 	vertical_up()						const { 
    	return this->has_left_vertical_up() ? this->left_vertical_up() : this->right_vertical_up();
    }
    LinkQuality vertical_up_quality()			const {
    	return this->has_left_vertical_up() ? this->prev_on_contour_quality : this->next_on_contour_quality;
    }
    // Returns -1 if there is no link down.
    int 	vertical_down()						const {
//    	assert(! this->has_left_vertical_down() || ! this->has_right_vertical_down());
    	return this->has_left_vertical_down() ? this->left_vertical_down() : this->right_vertical_down();
    }
    LinkQuality vertical_down_quality()			const {
    	return this->has_left_vertical_down() ? this->prev_on_contour_quality : this->next_on_contour_quality;
    }
    int 	vertical_outside()					const { return this->is_low() ? this->vertical_down() : this->vertical_up(); }
    LinkQuality vertical_outside_quality()		const { return this->is_low() ? this->vertical_down_quality() : this->vertical_up_quality(); }

    // Compare two y intersection points given by rational numbers.
    // Note that the rational number is given as pos_p/pos_q, where pos_p is int64 and pos_q is uint32.
    // This function calculates pos_p * other.pos_q < other.pos_p * pos_q as a 48bit number.
    // We don't use 128bit intrinsic data types as these are usually not supported by 32bit compilers and
    // we don't need the full 128bit precision anyway.
    bool operator<(const SegmentIntersection &other) const
    {
        assert(pos_q > 0);
        assert(other.pos_q > 0);
        if (pos_p == 0 || other.pos_p == 0) {
            // Because the denominators are positive and one of the nominators is zero,
            // following simple statement holds.
            return pos_p < other.pos_p;
        } else {
            // None of the nominators is zero.
            int sign1 = (pos_p > 0) ? 1 : -1;
            int sign2 = (other.pos_p > 0) ? 1 : -1;
            int signs = sign1 * sign2;
            assert(signs == 1 || signs == -1);
            if (signs < 0) {
                // The nominators have different signs.
                return sign1 < 0;
            } else {
                // The nominators have the same sign.
                // Absolute values
                uint64_t p1, p2;
                if (sign1 > 0) {
                    p1 = uint64_t(pos_p);
                    p2 = uint64_t(other.pos_p);
                } else {
                    p1 = uint64_t(- pos_p);
                    p2 = uint64_t(- other.pos_p);
                };
                // Multiply low and high 32bit words of p1 by other_pos.q
                // 32bit x 32bit => 64bit
                // l_hi and l_lo overlap by 32 bits.
                uint64_t l_hi = (p1 >> 32) * uint64_t(other.pos_q);
                uint64_t l_lo = (p1 & 0xffffffffll) * uint64_t(other.pos_q);
                l_hi += (l_lo >> 32);
                uint64_t r_hi = (p2 >> 32) * uint64_t(pos_q);
                uint64_t r_lo = (p2 & 0xffffffffll) * uint64_t(pos_q);
                r_hi += (r_lo >> 32);
                // Compare the high 64 bits.
                if (l_hi == r_hi) {
                    // Compare the low 32 bits.
                    l_lo &= 0xffffffffll;
                    r_lo &= 0xffffffffll;
                    return (sign1 < 0) ? (l_lo > r_lo) : (l_lo < r_lo);
                }
                return (sign1 < 0) ? (l_hi > r_hi) : (l_hi < r_hi);
            }
        }
    }

    bool operator==(const SegmentIntersection &other) const 
    {
        assert(pos_q > 0);
        assert(other.pos_q > 0);
        if (pos_p == 0 || other.pos_p == 0) {
            // Because the denominators are positive and one of the nominators is zero,
            // following simple statement holds.
            return pos_p == other.pos_p;
        }

        // None of the nominators is zero, none of the denominators is zero.
        bool positive = pos_p > 0;
        if (positive != (other.pos_p > 0))
            return false;
        // The nominators have the same sign.
        // Absolute values
        uint64_t p1 = positive ? uint64_t(pos_p) : uint64_t(- pos_p);
        uint64_t p2 = positive ? uint64_t(other.pos_p) : uint64_t(- other.pos_p);
        // Multiply low and high 32bit words of p1 by other_pos.q
        // 32bit x 32bit => 64bit
        // l_hi and l_lo overlap by 32 bits.
        uint64_t l_lo = (p1 & 0xffffffffll) * uint64_t(other.pos_q);
        uint64_t r_lo = (p2 & 0xffffffffll) * uint64_t(pos_q);
        if (l_lo != r_lo)
            return false;
        uint64_t l_hi = (p1 >> 32) * uint64_t(other.pos_q);
        uint64_t r_hi = (p2 >> 32) * uint64_t(pos_q);
        return l_hi + (l_lo >> 32) == r_hi + (r_lo >> 32);
    }
};
static_assert(sizeof(SegmentIntersection::pos_q) == 4, "SegmentIntersection::pos_q has to be 32bit long!");

// A vertical line with intersection points with polygons.
struct SegmentedIntersectionLine
{
    // Index of this vertical intersection line.
    size_t                              idx;
    // x position of this vertical intersection line.
    coord_t                             pos;
    // List of intersection points with polygons, sorted increasingly by the y axis.
    std::vector<SegmentIntersection>    intersections;
};

static SegmentIntersection phony_outer_intersection(SegmentIntersection::SegmentIntersectionType type, coord_t pos)
{
    assert(type == SegmentIntersection::OUTER_LOW || type == SegmentIntersection::OUTER_HIGH);
    SegmentIntersection out;
    // Invalid contour & segment.
    out.iContour                = std::numeric_limits<size_t>::max();
    out.iSegment                = std::numeric_limits<size_t>::max();
    out.pos_p                   = pos;
    out.type                    = type;
    // Invalid prev / next.
    out.prev_on_contour         = -1;
    out.next_on_contour         = -1;
    out.prev_on_contour_type    = SegmentIntersection::LinkType::Phony;
    out.next_on_contour_type    = SegmentIntersection::LinkType::Phony;
    out.prev_on_contour_quality = SegmentIntersection::LinkQuality::Invalid;
    out.next_on_contour_quality = SegmentIntersection::LinkQuality::Invalid;
    return out;
}

// A container maintaining an expolygon with its inner offsetted polygon.
// The purpose of the inner offsetted polygon is to provide segments to connect the infill lines.
struct ExPolygonWithOffset
{
public:
    ExPolygonWithOffset(
        const ExPolygon &expolygon,
        float   angle,
        coord_t aoffset1,
        // If the 2nd offset is zero, then it is ignored and only OUTER_LOW / OUTER_HIGH intersections are
        // populated into vertical intersection lines.
        coord_t aoffset2 = 0)
    {
        // Copy and rotate the source polygons.
        polygons_src = expolygon;
        if (angle != 0.f) {
            polygons_src.contour.rotate(angle);
            for (Polygon &hole : polygons_src.holes)
                hole.rotate(angle);
        }

        double miterLimit = DefaultMiterLimit;
        // for the infill pattern, don't cut the corners.
        // default miterLimt = 3
        //double miterLimit = 10.;
        // FIXME: Resolve properly the cases when it is constructed with aoffset1 = 0 and aoffset2 = 0,
        //        that is used in sample_grid_pattern() for Lightning infill.
        //assert(aoffset1 < 0);
        assert(aoffset2 <= 0);
        // assert(aoffset2 == 0 || aoffset2 < aoffset1);
//        bool sticks_removed = 
        remove_sticks(polygons_src);
//        if (sticks_removed) BOOST_LOG_TRIVIAL(error) << "Sticks removed!";
        polygons_outer = aoffset1 == 0 ? to_polygons(polygons_src) : offset(polygons_src, float(aoffset1), ClipperLib::jtMiter, miterLimit);
        if (aoffset2 < 0)
            polygons_inner = shrink(polygons_outer, float(aoffset1 - aoffset2), ClipperLib::jtMiter, miterLimit);
		// Filter out contours with zero area or small area, contours with 2 points only.
        const double min_area_threshold = 0.01 * aoffset2 * aoffset2;
        remove_small(polygons_outer, min_area_threshold);
        remove_small(polygons_inner, min_area_threshold);
        remove_sticks(polygons_outer);
        remove_sticks(polygons_inner);
		n_contours_outer = polygons_outer.size();
        n_contours_inner = polygons_inner.size();
        n_contours = n_contours_outer + n_contours_inner;
        polygons_ccw.assign(n_contours, false);
        for (size_t i = 0; i < n_contours; ++ i) {
            contour(i).remove_duplicate_points();
            assert(! contour(i).has_duplicate_points());
            polygons_ccw[i] = Slic3r::Geometry::is_ccw(contour(i));
        }
    }

    ExPolygonWithOffset(const ExPolygonWithOffset &rhs, float angle) : ExPolygonWithOffset(rhs) {
        if (angle != 0.f) {
            this->polygons_src.contour.rotate(angle);
            for (Polygon &hole : this->polygons_src.holes)
                hole.rotate(angle);
            for (Polygon &poly : this->polygons_outer)
                poly.rotate(angle);
            for (Polygon &poly : this->polygons_inner)
                poly.rotate(angle);
        }
    }

    // Any contour with offset1
    bool             is_contour_outer(size_t idx) const { return idx < n_contours_outer; }
    // Any contour with offset2
    bool             is_contour_inner(size_t idx) const { return idx >= n_contours_outer; }

    const Polygon&   contour(size_t idx) const 
        { return is_contour_outer(idx) ? polygons_outer[idx] : polygons_inner[idx - n_contours_outer]; }

    Polygon&         contour(size_t idx)
        { return is_contour_outer(idx) ? polygons_outer[idx] : polygons_inner[idx - n_contours_outer]; }

    bool             is_contour_ccw(size_t idx) const { return polygons_ccw[idx]; }

    BoundingBox      bounding_box_src() const 
        { return get_extents(polygons_src); }
    BoundingBox      bounding_box_outer() const 
        { return get_extents(polygons_outer); }
    BoundingBox      bounding_box_inner() const 
        { return get_extents(polygons_inner); }

#ifdef SLIC3R_DEBUG
    void export_to_svg(Slic3r::SVG &svg) {
        svg.draw_outline(polygons_src,   "black");
        svg.draw_outline(polygons_outer, "green");
        svg.draw_outline(polygons_inner, "brown");
    }
#endif /* SLIC3R_DEBUG */

    ExPolygon        polygons_src;
    Polygons         polygons_outer;
    Polygons         polygons_inner;

    size_t           n_contours_outer;
    size_t           n_contours_inner;
    size_t           n_contours;

protected:
    // For each polygon of polygons_inner, remember its orientation.
    std::vector<unsigned char> polygons_ccw;
};

static inline int distance_of_segmens(const Polygon &poly, size_t seg1, size_t seg2, bool forward)
{
    int d = int(seg2) - int(seg1);
    if (! forward)
        d = - d;
    if (d < 0)
        d += int(poly.points.size());
    return d;
}

// Find an intersection on a previous line, but return -1, if the connecting segment of a perimeter was already extruded.
static inline bool intersection_on_prev_next_vertical_line_valid(
    const std::vector<SegmentedIntersectionLine>  &segs,
    size_t                                         iVerticalLine,
    size_t                                         iIntersection,
    SegmentIntersection::Side                      side)
{
    const SegmentedIntersectionLine &vline_this = segs[iVerticalLine];
    const SegmentIntersection       &it_this    = vline_this.intersections[iIntersection];
	if (it_this.has_vertical(side))
	    // Not the first intersection along the contor. This intersection point
	    // has been preceded by an intersection point along the vertical line.
		return false;
    int iIntersectionOther = it_this.horizontal(side);
    if (iIntersectionOther == -1)
        return false;
    assert(side == SegmentIntersection::Side::Right ? (iVerticalLine + 1 < segs.size()) : (iVerticalLine > 0));
    const SegmentedIntersectionLine &vline_other = segs[side == SegmentIntersection::Side::Right ? (iVerticalLine + 1) : (iVerticalLine - 1)];
    const SegmentIntersection       &it_other    = vline_other.intersections[iIntersectionOther];
    assert(it_other.is_inner());
    assert(iIntersectionOther > 0 && size_t(iIntersectionOther + 1) < vline_other.intersections.size());
    // Is iIntersectionOther at the boundary of a vertical segment?
    const SegmentIntersection       &it_other2   = vline_other.intersections[it_other.is_low() ? iIntersectionOther - 1 : iIntersectionOther + 1];
    if (it_other2.is_inner())
        // Cannot follow a perimeter segment into the middle of another vertical segment.
        // Only perimeter segments connecting to the end of a vertical segment are followed.
        return false;
    assert(it_other.is_low() == it_other2.is_low());
    if (it_this.horizontal_quality(side) != SegmentIntersection::LinkQuality::Valid)
    	return false;
    if (side == SegmentIntersection::Side::Right ? it_this.consumed_perimeter_right : it_other.consumed_perimeter_right)
        // This perimeter segment was already consumed.
        return false;
    if (it_other.is_low() ? it_other.consumed_vertical_up : vline_other.intersections[iIntersectionOther - 1].consumed_vertical_up)
        // This vertical segment was already consumed.
        return false;
#if 0
    if (it_other.vertical_outside() != -1 && it_other.vertical_outside_quality() == SegmentIntersection::LinkQuality::Valid)
        // Landed inside a vertical run. Stop here.
        return false;
#endif
    return true;
}

static inline bool intersection_on_prev_vertical_line_valid(
    const std::vector<SegmentedIntersectionLine>  &segs, 
    size_t                                         iVerticalLine, 
    size_t                                         iIntersection)
{
    return intersection_on_prev_next_vertical_line_valid(segs, iVerticalLine, iIntersection, SegmentIntersection::Side::Left);
}

static inline bool intersection_on_next_vertical_line_valid(
    const std::vector<SegmentedIntersectionLine>  &segs, 
    size_t                                         iVerticalLine, 
    size_t                                         iIntersection)
{
    return intersection_on_prev_next_vertical_line_valid(segs, iVerticalLine, iIntersection, SegmentIntersection::Side::Right);
}

// Measure an Euclidian length of a perimeter segment when going from iIntersection to iIntersection2.
static inline coordf_t measure_perimeter_horizontal_segment_length(
    const ExPolygonWithOffset                     &poly_with_offset, 
    const std::vector<SegmentedIntersectionLine>  &segs,
    size_t                                         iVerticalLine,
    size_t                                         iIntersection,
    size_t                                         iIntersection2)
{
    size_t                           iVerticalLineOther = iVerticalLine + 1;
    assert(iVerticalLineOther < segs.size());
    const SegmentedIntersectionLine &vline  = segs[iVerticalLine];
    const SegmentIntersection       &it     = vline.intersections[iIntersection];
    const SegmentedIntersectionLine &vline2 = segs[iVerticalLineOther];
    const SegmentIntersection       &it2    = vline2.intersections[iIntersection2];
    assert(it.iContour == it2.iContour);
    const Polygon                   &poly   = poly_with_offset.contour(it.iContour);
//    const bool                       ccw    = poly_with_offset.is_contour_ccw(vline.iContour);
    assert(it.type == it2.type);
    assert(it.iContour == it2.iContour);

    Point p1(vline.pos,  it.pos());
    Point p2(vline2.pos, it2.pos());
    return it.is_low() ?
        segment_length(poly, it .iSegment, p1, it2.iSegment, p2) :
        segment_length(poly, it2.iSegment, p2, it .iSegment, p1);
}

// Append the points of a perimeter segment when going from iIntersection to iIntersection2.
// The first point (the point of iIntersection) will not be inserted,
// the last point will be inserted.
static inline void emit_perimeter_prev_next_segment(
    const ExPolygonWithOffset                     &poly_with_offset,
    const std::vector<SegmentedIntersectionLine>  &segs,
    size_t                                         iVerticalLine,
    size_t                                         iInnerContour,
    size_t                                         iIntersection,
    size_t                                         iIntersection2,
    Polyline                                      &out,
    bool                                           dir_is_next)
{
    size_t iVerticalLineOther = iVerticalLine;
    if (dir_is_next) {
        ++ iVerticalLineOther;
        assert(iVerticalLineOther < segs.size());
    } else {
        assert(iVerticalLineOther > 0);
        -- iVerticalLineOther;
    }

    const SegmentedIntersectionLine &il     = segs[iVerticalLine];
    const SegmentIntersection       &itsct  = il.intersections[iIntersection];
    const SegmentedIntersectionLine &il2    = segs[iVerticalLineOther];
    const SegmentIntersection       &itsct2 = il2.intersections[iIntersection2];
    const Polygon                   &poly   = poly_with_offset.contour(iInnerContour);
//    const bool                       ccw    = poly_with_offset.is_contour_ccw(iInnerContour);
    assert(itsct.type == itsct2.type);
    assert(itsct.iContour == itsct2.iContour);
    assert(itsct.is_inner());
    const bool                       forward = itsct.is_low() == dir_is_next;
    // Do not append the first point.
    // out.points.push_back(Point(il.pos, itsct.pos));
    if (forward)
        polygon_segment_append(out.points, poly, itsct.iSegment, itsct2.iSegment);
    else
        polygon_segment_append_reversed(out.points, poly, itsct.iSegment, itsct2.iSegment);
    // Append the last point.
    out.points.push_back(Point(il2.pos, itsct2.pos()));
}

static inline coordf_t measure_perimeter_segment_on_vertical_line_length(
    const ExPolygonWithOffset                     &poly_with_offset,
    const std::vector<SegmentedIntersectionLine>  &segs,
    size_t                                         iVerticalLine,
    size_t                                         iIntersection,
    size_t                                         iIntersection2,
    bool                                           forward)
{
    const SegmentedIntersectionLine &il = segs[iVerticalLine];
    const SegmentIntersection       &itsct = il.intersections[iIntersection];
    const SegmentIntersection       &itsct2 = il.intersections[iIntersection2];
    const Polygon                   &poly = poly_with_offset.contour(itsct.iContour);
    assert(itsct.is_inner() == itsct2.is_inner());
    assert(itsct.type != itsct2.type);
    assert(itsct.iContour == itsct2.iContour);
    Point p1(il.pos, itsct.pos());
    Point p2(il.pos, itsct2.pos());
    return forward ?
        segment_length(poly, itsct .iSegment, p1, itsct2.iSegment, p2) :
        segment_length(poly, itsct2.iSegment, p2, itsct .iSegment, p1);
}

// Append the points of a perimeter segment when going from iIntersection to iIntersection2.
// The first point (the point of iIntersection) will not be inserted,
// the last point will be inserted.
static inline void emit_perimeter_segment_on_vertical_line(
	const ExPolygonWithOffset                     &poly_with_offset,
	const std::vector<SegmentedIntersectionLine>  &segs,
	size_t                                         iVerticalLine,
	size_t                                         iInnerContour,
	size_t                                         iIntersection,
	size_t                                         iIntersection2,
	Polyline                                      &out,
	bool                                           forward)
{
	const SegmentedIntersectionLine &il = segs[iVerticalLine];
	const SegmentIntersection       &itsct = il.intersections[iIntersection];
	const SegmentIntersection       &itsct2 = il.intersections[iIntersection2];
	const Polygon                   &poly = poly_with_offset.contour(iInnerContour);
	assert(itsct.is_inner());
	assert(itsct2.is_inner());
	assert(itsct.type != itsct2.type);
    assert(itsct.iContour == iInnerContour);
	assert(itsct.iContour == itsct2.iContour);
	// Do not append the first point.
	// out.points.push_back(Point(il.pos, itsct.pos));
	if (forward)
		polygon_segment_append(out.points, poly, itsct.iSegment, itsct2.iSegment);
	else
		polygon_segment_append_reversed(out.points, poly, itsct.iSegment, itsct2.iSegment);
	// Append the last point.
	out.points.push_back(Point(il.pos, itsct2.pos()));
}

//TBD: For precise infill, measure the area of a slab spanned by an infill line.
/*
static inline float measure_outer_contour_slab(
    const ExPolygonWithOffset                     &poly_with_offset,
    const std::vector<SegmentedIntersectionLine>  &segs,
    size_t                                         i_vline,
    size_t                                         iIntersection)
{
    const SegmentedIntersectionLine &il     = segs[i_vline];
    const SegmentIntersection       &itsct  = il.intersections[i_vline];
    const SegmentIntersection       &itsct2 = il.intersections[iIntersection2];
    const Polygon                   &poly   = poly_with_offset.contour((itsct.iContour);
    assert(itsct.is_outer());
    assert(itsct2.is_outer());
    assert(itsct.type != itsct2.type);
    assert(itsct.iContour == itsct2.iContour);
    if (! itsct.is_outer() || ! itsct2.is_outer() || itsct.type == itsct2.type || itsct.iContour != itsct2.iContour)
        // Error, return zero area.
        return 0.f;

    // Find possible connection points on the previous / next vertical line.
    int iPrev = intersection_on_prev_vertical_line(poly_with_offset, segs, i_vline, itsct.iContour, i_intersection);
    int iNext = intersection_on_next_vertical_line(poly_with_offset, segs, i_vline, itsct.iContour, i_intersection);
    // Find possible connection points on the same vertical line.
    int iAbove = iBelow = -1;
    // Does the perimeter intersect the current vertical line above intrsctn?
    for (size_t i = i_intersection + 1; i + 1 < seg.intersections.size(); ++ i)
        if (seg.intersections[i].iContour == itsct.iContour)
            { iAbove = i; break; }
    // Does the perimeter intersect the current vertical line below intrsctn?
    for (int i = int(i_intersection) - 1; i > 0; -- i)
        if (seg.intersections[i].iContour == itsct.iContour)
            { iBelow = i; break; }

    if (iSegAbove != -1 && seg.intersections[iAbove].type == SegmentIntersection::OUTER_HIGH) {
        // Invalidate iPrev resp. iNext, if the perimeter crosses the current vertical line earlier than iPrev resp. iNext.
        // The perimeter contour orientation.
        const Polygon &poly = poly_with_offset.contour(itsct.iContour);
        {
            int d_horiz = (iPrev  == -1) ? std::numeric_limits<int>::max() :
                distance_of_segmens(poly, segs[i_vline-1].intersections[iPrev].iSegment, itsct.iSegment, true);
            int d_down  = (iBelow == -1) ? std::numeric_limits<int>::max() :
                distance_of_segmens(poly, iSegBelow, itsct.iSegment, true);
            int d_up    = (iAbove == -1) ? std::numeric_limits<int>::max() :
                distance_of_segmens(poly, iSegAbove, itsct.iSegment, true);
            if (intrsection_type_prev == INTERSECTION_TYPE_OTHER_VLINE_OK && d_horiz > std::min(d_down, d_up))
                // The vertical crossing comes eralier than the prev crossing.
                // Disable the perimeter going back.
                intrsection_type_prev = INTERSECTION_TYPE_OTHER_VLINE_NOT_FIRST;
            if (d_up > std::min(d_horiz, d_down))
                // The horizontal crossing comes earlier than the vertical crossing.
                vert_seg_dir_valid_mask &= ~DIR_BACKWARD;
        }
        {
            int d_horiz = (iNext     == -1) ? std::numeric_limits<int>::max() :
                distance_of_segmens(poly, itsct.iSegment, segs[i_vline+1].intersections[iNext].iSegment, true);
            int d_down  = (iSegBelow == -1) ? std::numeric_limits<int>::max() :
                distance_of_segmens(poly, itsct.iSegment, iSegBelow, true);
            int d_up    = (iSegAbove == -1) ? std::numeric_limits<int>::max() :
                distance_of_segmens(poly, itsct.iSegment, iSegAbove, true);
            if (d_up > std::min(d_horiz, d_down))
                // The horizontal crossing comes earlier than the vertical crossing.
                vert_seg_dir_valid_mask &= ~DIR_FORWARD;
        }
    }
}
*/

enum DirectionMask
{
    DIR_FORWARD  = 1,
    DIR_BACKWARD = 2
};

static std::vector<SegmentedIntersectionLine> slice_region_by_vertical_lines(const ExPolygonWithOffset &poly_with_offset, size_t n_vlines, coord_t x0, coord_t line_spacing)
{
    // Allocate storage for the segments.
    std::vector<SegmentedIntersectionLine> segs(n_vlines, SegmentedIntersectionLine());
    for (coord_t i = 0; i < coord_t(n_vlines); ++ i) {
        segs[i].idx = i;
        segs[i].pos = x0 + i * line_spacing;
    }
    // For each contour
    for (size_t iContour = 0; iContour < poly_with_offset.n_contours; ++ iContour) {
        const Points &contour = poly_with_offset.contour(iContour).points;
        if (contour.size() < 2)
            continue;
        // For each segment
        for (size_t iSegment = 0; iSegment < contour.size(); ++ iSegment) {
            size_t iPrev = ((iSegment == 0) ? contour.size() : iSegment) - 1;
            const Point &p1 = contour[iPrev];
            const Point &p2 = contour[iSegment];
            // Which of the equally spaced vertical lines is intersected by this segment?
            coord_t l = p1(0);
            coord_t r = p2(0);
            if (l > r)
                std::swap(l, r);
            // il, ir are the left / right indices of vertical lines intersecting a segment
            int il = (l - x0) / line_spacing;
            while (il * line_spacing + x0 < l)
                ++ il;
            il = std::max(int(0), il);
            int ir = (r - x0 + line_spacing) / line_spacing;
            while (ir * line_spacing + x0 > r)
                -- ir;
            ir = std::min(int(segs.size()) - 1, ir);
            if (il > ir)
                // No vertical line intersects this segment.
                continue;
            assert(il >= 0 && size_t(il) < segs.size());
            assert(ir >= 0 && size_t(ir) < segs.size());
            for (int i = il; i <= ir; ++ i) {
                coord_t this_x = segs[i].pos;
				assert(this_x == i * line_spacing + x0);
                SegmentIntersection is;
                is.iContour = iContour;
                is.iSegment = iSegment;
                assert(l <= this_x);
                assert(r >= this_x);
                // Calculate the intersection position in y axis. x is known.
                if (p1.x() == this_x) {
                    if (p2.x() == this_x) {
                        // Ignore strictly vertical segments.
                        continue;
                    }
                    const Point &p0 = prev_value_modulo(iPrev, contour);
                    if (int64_t(p0.x() - p1.x()) * int64_t(p2.x() - p1.x()) > 0) {
                        // Ignore points of a contour touching the infill line from one side.
                        continue;
                    }
                    is.pos_p = p1.y();
                    is.pos_q = 1;
                } else if (p2.x() == this_x) {
                    const Point &p3 = next_value_modulo(iSegment, contour);
                    if (int64_t(p3.x() - p2.x()) * int64_t(p1.x() - p2.x()) > 0) {
                        // Ignore points of a contour touching the infill line from one side.
                        continue;
                    }
                    is.pos_p = p2.y();
                    is.pos_q = 1;
                } else {
                    // First calculate the intersection parameter 't' as a rational number with non negative denominator.
                    if (p2.x() > p1.x()) {
                        is.pos_p = this_x - p1.x();
                        is.pos_q = p2.x() - p1.x();
                    } else {
                        is.pos_p = p1.x() - this_x;
                        is.pos_q = p1.x() - p2.x();
                    }
                    assert(is.pos_q > 1);
                    assert(is.pos_p > 0 && is.pos_p < is.pos_q);
                    // Make an intersection point from the 't'.
                    is.pos_p *= int64_t(p2.y() - p1.y());
                    is.pos_p += p1.y() * int64_t(is.pos_q);
                }
                // +-1 to take rounding into account.
                assert(is.pos() + 1 >= std::min(p1.y(), p2.y()));
                assert(is.pos() <= std::max(p1.y(), p2.y()) + 1);
                segs[i].intersections.push_back(is);
            }
        }
    }

    // Sort the intersections along their segments, specify the intersection types.
    for (size_t i_seg = 0; i_seg < segs.size(); ++ i_seg) {
        SegmentedIntersectionLine &sil = segs[i_seg];
        // Sort the intersection points using exact rational arithmetic.
        std::sort(sil.intersections.begin(), sil.intersections.end());
        // Assign the intersection types, remove duplicate or overlapping intersection points.
        // When a loop vertex touches a vertical line, intersection point is generated for both segments.
        // If such two segments are oriented equally, then one of them is removed.
        // Otherwise the vertex is tangential to the vertical line and both segments are removed.
        // The same rule applies, if the loop is pinched into a single point and this point touches the vertical line:
        // The loop has a zero vertical size at the vertical line, therefore the intersection point is removed.
        size_t j = 0;
        for (size_t i = 0; i < sil.intersections.size(); ++ i) {
            // What is the orientation of the segment at the intersection point?
            SegmentIntersection       &is       = sil.intersections[i];
            const size_t               iContour = is.iContour;
            const Points              &contour  = poly_with_offset.contour(iContour).points;
            const size_t               iSegment = is.iSegment;
            const size_t               iPrev    = prev_idx_modulo(iSegment, contour);
            const coord_t              dir      = contour[iSegment].x() - contour[iPrev].x();
            const bool                 low      = dir > 0;
            is.type = poly_with_offset.is_contour_outer(iContour) ?
                (low ? SegmentIntersection::OUTER_LOW : SegmentIntersection::OUTER_HIGH) :
                (low ? SegmentIntersection::INNER_LOW : SegmentIntersection::INNER_HIGH);
            bool take_next = true;
            if (j > 0) {
                SegmentIntersection &is2 = sil.intersections[j - 1];
                if (iContour == is2.iContour && is.pos_q == 1 && is2.pos_q == 1) {
                    // Two successive intersection points on a vertical line with the same contour, both points are end points of their respective contour segments.
                    if (is.pos_p == is2.pos_p) {
                        // Two successive segments meet exactly at the vertical line.
                        // Verify that the segments of sil.intersections[i] and sil.intersections[j-1] are adjoint.
                        assert(iSegment == prev_idx_modulo(is2.iSegment, contour) || is2.iSegment == iPrev);
                        assert(is.type == is2.type);
                        // Two successive segments of the same direction (both to the right or both to the left)
                        // meet exactly at the vertical line.
                        // Remove the second intersection point.
                        take_next = false;
                    } else if (is.type == is2.type) {
                        // Two non successive segments of the same direction (both to the right or both to the left)
                        // meet exactly at the vertical line. That means there is a Z shaped path, where the center segment
                        // of the Z shaped path is aligned with this vertical line.
                        // Remove one of the intersection points while maximizing the vertical segment length.
                        if (low) {
                            // Remove the second intersection point, keep the first intersection point.
                        } else {
                            // Remove the first intersection point, keep the second intersection point.
                            sil.intersections[j-1] = sil.intersections[i];
                        }
                        take_next = false;
                    }
                }
            }
            if (take_next) {
                // Vertical line intersects a contour segment at a general position (not at one of its end points).
                if (j < i)
                    sil.intersections[j] = sil.intersections[i];
                ++ j;
            }
        }
        // Shrink the list of intersections, if any of the intersection was removed during the classification.
        if (j < sil.intersections.size())
            sil.intersections.erase(sil.intersections.begin() + j, sil.intersections.end());
    }

    // Verify the segments. If something is wrong, give up.
#ifdef INFILL_DEBUG_OUTPUT
    #define INFILL_DEBUG_ASSERT(CONDITION)
    try {
#else // INFILL_DEBUG_OUTPUT
    #define INFILL_DEBUG_ASSERT(CONDITION) assert(CONDITION)
#endif // INFILL_DEBUG_OUTPUT
#define ASSERT_THROW(CONDITION) do { INFILL_DEBUG_ASSERT(CONDITION); if (! (CONDITION)) throw InfillFailedException(); } while (0)
    for (size_t i_seg = 0; i_seg < segs.size(); ++ i_seg) {
        SegmentedIntersectionLine &sil = segs[i_seg];
        // The intersection points have to be even.
        ASSERT_THROW((sil.intersections.size() & 1) == 0);
        for (size_t i = 0; i < sil.intersections.size();) {
            // An intersection segment crossing the bigger contour may cross the inner offsetted contour even number of times.
            ASSERT_THROW(sil.intersections[i].type == SegmentIntersection::OUTER_LOW);
            size_t j = i + 1;
            ASSERT_THROW(j < sil.intersections.size());
            ASSERT_THROW(sil.intersections[j].type == SegmentIntersection::INNER_LOW || sil.intersections[j].type == SegmentIntersection::OUTER_HIGH);
            for (; j < sil.intersections.size() && sil.intersections[j].is_inner(); ++ j) ;
            ASSERT_THROW(j < sil.intersections.size());
            ASSERT_THROW((j & 1) == 1);
            ASSERT_THROW(sil.intersections[j].type == SegmentIntersection::OUTER_HIGH);
            ASSERT_THROW(i + 1 == j || sil.intersections[j - 1].type == SegmentIntersection::INNER_HIGH);
            i = j + 1;
        }
    }
#undef ASSERT_THROW
#undef INFILL_DEBUG_ASSERT
#ifdef INFILL_DEBUG_OUTPUT
    } catch (const InfillFailedException & /* ex */) {
        // Export the buggy result into an SVG file.
        static int iRun = 0;
        BoundingBox bbox = get_extents(poly_with_offset.polygons_src);
        bbox.offset(scale_(3.));
        ::Slic3r::SVG svg(debug_out_path("slice_region_by_vertical_lines-failed-%d.svg", iRun ++), bbox);
        svg.draw(poly_with_offset.polygons_src);
        svg.draw_outline(poly_with_offset.polygons_src, "green");
        svg.draw_outline(poly_with_offset.polygons_outer, "green");
        svg.draw_outline(poly_with_offset.polygons_inner, "green");
        for (size_t i_seg = 0; i_seg < segs.size(); ++i_seg) {
            SegmentedIntersectionLine &sil = segs[i_seg];
            for (size_t i = 0; i < sil.intersections.size();) {
                // An intersection segment crossing the bigger contour may cross the inner offsetted contour even number of times.
                if (sil.intersections[i].type != SegmentIntersection::OUTER_LOW) {
                    svg.draw(Point(sil.pos, sil.intersections[i].pos()), "red");
                    break;
                }
                size_t j = i + 1;
                if (j == sil.intersections.size()) {
                    svg.draw(Point(sil.pos, sil.intersections[i].pos()), "magenta");
                    break;
                }
                if (! (sil.intersections[j].type == SegmentIntersection::INNER_LOW || sil.intersections[j].type == SegmentIntersection::OUTER_HIGH)) {
                    svg.draw(Point(sil.pos, sil.intersections[j].pos()), "blue");
                    break;
                }
                for (; j < sil.intersections.size() && sil.intersections[j].is_inner(); ++j);
                if (j == sil.intersections.size()) {
                    svg.draw(Point(sil.pos, sil.intersections[j - 1].pos()), "magenta");
                    break;
                }
                if ((j & 1) != 1 || sil.intersections[j].type != SegmentIntersection::OUTER_HIGH) {
                    svg.draw(Point(sil.pos, sil.intersections[j].pos()), "red");
                    break;
                }
                if (! (i + 1 == j || sil.intersections[j - 1].type == SegmentIntersection::INNER_HIGH)) {
                    svg.draw(Point(sil.pos, sil.intersections[j].pos()), "red");
                    break;
                }
                svg.draw(Line(Point(sil.pos, sil.intersections[i].pos()), Point(sil.pos, sil.intersections[j].pos())), "black");
                i = j + 1;
            }
        }
        assert(false);
        throw;
    }
#endif //INFILL_DEBUG_OUTPUT
    return segs;
}

#ifndef NDEBUG
bool validate_segment_intersection_connectivity(const std::vector<SegmentedIntersectionLine> &segs)
{
    // Validate the connectivity.
    for (size_t i_vline = 0; i_vline + 1 < segs.size(); ++ i_vline) {
        const SegmentedIntersectionLine &il_left  = segs[i_vline];
        const SegmentedIntersectionLine &il_right = segs[i_vline + 1];
        for (const SegmentIntersection &it : il_left.intersections) {
            if (it.has_right_horizontal()) {
                const SegmentIntersection &it_right = il_right.intersections[it.right_horizontal()];
                // For a right link there is a symmetric left link.
                assert(it.iContour == it_right.iContour);
                assert(it.type == it_right.type);
                assert(it_right.has_left_horizontal());
                assert(it_right.left_horizontal() == int(&it - il_left.intersections.data()));
            }
        }
        for (const SegmentIntersection &it : il_right.intersections) {
            if (it.has_left_horizontal()) {
                const SegmentIntersection &it_left = il_left.intersections[it.left_horizontal()];
                // For a right link there is a symmetric left link.
                assert(it.iContour == it_left.iContour);
                assert(it.type == it_left.type);
                assert(it_left.has_right_horizontal());
                assert(it_left.right_horizontal() == int(&it - il_right.intersections.data()));
            }
        }
    }
    for (size_t i_vline = 0; i_vline < segs.size(); ++ i_vline) {
        const SegmentedIntersectionLine &il = segs[i_vline];
        for (const SegmentIntersection &it : il.intersections) {
            auto i_it = int(&it - il.intersections.data());
            if (it.has_left_vertical_up()) {
                assert(il.intersections[it.left_vertical_up()].left_vertical_down() == i_it);
                assert(il.intersections[it.left_vertical_up()].prev_on_contour_quality == it.prev_on_contour_quality);
            }
            if (it.has_left_vertical_down()) {
                assert(il.intersections[it.left_vertical_down()].left_vertical_up() == i_it);
                assert(il.intersections[it.left_vertical_down()].prev_on_contour_quality == it.prev_on_contour_quality);
            }
            if (it.has_right_vertical_up()) {
                assert(il.intersections[it.right_vertical_up()].right_vertical_down() == i_it);
                assert(il.intersections[it.right_vertical_up()].next_on_contour_quality == it.next_on_contour_quality);
            }
            if (it.has_right_vertical_down()) {
                assert(il.intersections[it.right_vertical_down()].right_vertical_up() == i_it);
                assert(il.intersections[it.right_vertical_down()].next_on_contour_quality == it.next_on_contour_quality);
            }
        }
    }
    return true;
}
#endif /* NDEBUG */

// Connect each contour / vertical line intersection point with another two contour / vertical line intersection points.
// (fill in SegmentIntersection::{prev_on_contour, prev_on_contour_vertical, next_on_contour, next_on_contour_vertical}.
// These contour points are either on the same vertical line, or on the vertical line left / right to the current one.
static void connect_segment_intersections_by_contours(
	const ExPolygonWithOffset &poly_with_offset, std::vector<SegmentedIntersectionLine> &segs,
	const FillParams &params, const coord_t link_max_length)
{
    for (size_t i_vline = 0; i_vline < segs.size(); ++ i_vline) {
	    SegmentedIntersectionLine       &il      = segs[i_vline];
	    const SegmentedIntersectionLine *il_prev = i_vline > 0 ? &segs[i_vline - 1] : nullptr;
	    const SegmentedIntersectionLine *il_next = i_vline + 1 < segs.size() ? &segs[i_vline + 1] : nullptr;

        for (int i_intersection = 0; i_intersection < int(il.intersections.size()); ++ i_intersection) {
		    SegmentIntersection &itsct   = il.intersections[i_intersection];
	        const Polygon 		&poly    = poly_with_offset.contour(itsct.iContour);
            const bool           forward = itsct.is_low(); // == poly_with_offset.is_contour_ccw(intrsctn->iContour);

	        // 1) Find possible connection points on the previous / next vertical line.
		    // Find an intersection point on il_prev, intersecting i_intersection
		    // at the same orientation as i_intersection, and being closest to i_intersection
		    // in the number of contour segments, when following the direction of the contour.
            //FIXME this has O(n) time complexity. Likely an O(log(n)) scheme is possible.
		    int iprev  = -1;
            int d_prev = std::numeric_limits<int>::max();
		    if (il_prev) {
                for (int i = 0; i < int(il_prev->intersections.size()); ++ i) {
			        const SegmentIntersection &itsct2 = il_prev->intersections[i];
			        if (itsct.iContour == itsct2.iContour && itsct.type == itsct2.type) {
			            // The intersection points lie on the same contour and have the same orientation.
			            // Find the intersection point with a shortest path in the direction of the contour.
			            int d = distance_of_segmens(poly, itsct2.iSegment, itsct.iSegment, forward);
			            if (d < d_prev) {
			                iprev = i;
                            d_prev = d;
			            }
			        }
			    }
			}

            // The same for il_next.
		    int inext  = -1;
            int d_next = std::numeric_limits<int>::max();
            if (il_next) {
                for (int i = 0; i < int(il_next->intersections.size()); ++ i) {
			        const SegmentIntersection &itsct2 = il_next->intersections[i];
			        if (itsct.iContour == itsct2.iContour && itsct.type == itsct2.type) {
			            // The intersection points lie on the same contour and have the same orientation.
			            // Find the intersection point with a shortest path in the direction of the contour.
			            int d = distance_of_segmens(poly, itsct.iSegment, itsct2.iSegment, forward);
			            if (d < d_next) {
			                inext = i;
                            d_next = d;
			            }
			        }
			    }
			}

	        // 2) Find possible connection points on the same vertical line.
            bool same_prev = false;
            bool same_next = false;
            // Does the perimeter intersect the current vertical line above intrsctn?
            for (int i = 0; i < int(il.intersections.size()); ++ i)
                if (const SegmentIntersection &it2 = il.intersections[i];
                    i != i_intersection && it2.iContour == itsct.iContour && it2.type != itsct.type) {
                    int d = distance_of_segmens(poly, it2.iSegment, itsct.iSegment, forward);
                    if (d < d_prev) {
                        iprev     = i;
                        d_prev    = d;
                        same_prev = true;
                    }
                    d = distance_of_segmens(poly, itsct.iSegment, it2.iSegment, forward);
                    if (d < d_next) {
                        inext     = i;
                        d_next    = d;
                        same_next = true;
                    }
                }
            assert(iprev >= 0);
            assert(inext >= 0);

            itsct.prev_on_contour 	    = iprev;
            itsct.prev_on_contour_type  = same_prev ? 
                (iprev < i_intersection ? SegmentIntersection::LinkType::Down : SegmentIntersection::LinkType::Up) :
                SegmentIntersection::LinkType::Horizontal;
            itsct.next_on_contour 	    = inext;
            itsct.next_on_contour_type  = same_next ?
                (inext < i_intersection ? SegmentIntersection::LinkType::Down : SegmentIntersection::LinkType::Up) :
                SegmentIntersection::LinkType::Horizontal;

        	if (same_prev) {
        		// Only follow a vertical perimeter segment if it skips just the outer intersections.
        		SegmentIntersection *it  = &itsct;
        		SegmentIntersection *end = il.intersections.data() + iprev;
        		assert(it != end);
        		if (it > end)
        			std::swap(it, end);
                for (++ it; it != end; ++ it)
                    if (it->is_inner()) {
        				itsct.prev_on_contour_quality = SegmentIntersection::LinkQuality::Invalid;
                        break;
                    }
            }

        	if (same_next) {
        		// Only follow a vertical perimeter segment if it skips just the outer intersections.
        		SegmentIntersection *it  = &itsct;
        		SegmentIntersection *end = il.intersections.data() + inext;
        		assert(it != end);
        		if (it > end)
        			std::swap(it, end);
                for (++ it; it != end; ++ it)
                    if (it->is_inner()) {
        				itsct.next_on_contour_quality = SegmentIntersection::LinkQuality::Invalid;
                        break;
                    }
            }

            // If both iprev and inext are on this vline, then there must not be any intersection with the previous or next contour and we will
            // not trace this contour when generating infill.
            if (same_prev && same_next) {
            	assert(iprev != i_intersection);
            	assert(inext != i_intersection);
            	if ((iprev > i_intersection) == (inext > i_intersection)) {
            		// Both closest intersections of this contour are on the same vertical line and at the same side of this point.
            		// Ignore them when tracing the infill.
	                itsct.prev_on_contour_quality = SegmentIntersection::LinkQuality::Invalid;
	                itsct.next_on_contour_quality = SegmentIntersection::LinkQuality::Invalid;
	            }
            }

			if (params.dont_connect()) {
				if (itsct.prev_on_contour_quality == SegmentIntersection::LinkQuality::Valid)
					itsct.prev_on_contour_quality = SegmentIntersection::LinkQuality::TooLong;
				if (itsct.next_on_contour_quality == SegmentIntersection::LinkQuality::Valid)
					itsct.next_on_contour_quality = SegmentIntersection::LinkQuality::TooLong;
			} else if (link_max_length > 0) {
            	// Measure length of the links.
				if (itsct.prev_on_contour_quality == SegmentIntersection::LinkQuality::Valid &&
            	    (same_prev ? 
            		 	measure_perimeter_segment_on_vertical_line_length(poly_with_offset, segs, i_vline, iprev, i_intersection, forward) :
            			measure_perimeter_horizontal_segment_length(poly_with_offset, segs, i_vline - 1, iprev, i_intersection)) > link_max_length)
	    			itsct.prev_on_contour_quality = SegmentIntersection::LinkQuality::TooLong;
				if (itsct.next_on_contour_quality == SegmentIntersection::LinkQuality::Valid &&
            		(same_next ?
            			measure_perimeter_segment_on_vertical_line_length(poly_with_offset, segs, i_vline, i_intersection, inext, forward) :
            			measure_perimeter_horizontal_segment_length(poly_with_offset, segs, i_vline, i_intersection, inext)) > link_max_length)
	    			itsct.next_on_contour_quality = SegmentIntersection::LinkQuality::TooLong;
            }
	    }

	    // Make the LinkQuality::Invalid symmetric on vertical connections.
        for (int i_intersection = 0; i_intersection < int(il.intersections.size()); ++ i_intersection) {
		    SegmentIntersection &it = il.intersections[i_intersection];
            if (it.has_left_vertical() && it.prev_on_contour_quality == SegmentIntersection::LinkQuality::Invalid) {
			    SegmentIntersection &it2 = il.intersections[it.left_vertical()];
			    assert(it2.left_vertical() == i_intersection);
			    it2.prev_on_contour_quality = SegmentIntersection::LinkQuality::Invalid;
            }
            if (it.has_right_vertical() && it.next_on_contour_quality == SegmentIntersection::LinkQuality::Invalid) {
			    SegmentIntersection &it2 = il.intersections[it.right_vertical()];
			    assert(it2.right_vertical() == i_intersection);
			    it2.next_on_contour_quality = SegmentIntersection::LinkQuality::Invalid;
            }
		}
    }

    assert(validate_segment_intersection_connectivity(segs));
}

static void pinch_contours_insert_phony_outer_intersections(std::vector<SegmentedIntersectionLine> &segs)
{
    // Keep the vector outside the loops, so they will not be reallocated.
    // Where to insert new outer points.
    std::vector<size_t>                 insert_after;
    // Mapping of indices of current intersection line after inserting new outer points.
    std::vector<int32_t>                map;
    std::vector<SegmentIntersection>    temp_intersections;

    for (size_t i_vline = 1; i_vline < segs.size(); ++ i_vline) {
        SegmentedIntersectionLine &il = segs[i_vline];
        assert(il.intersections.empty() || il.intersections.size() >= 2);
        if (! il.intersections.empty()) {
            assert(il.intersections.front().type == SegmentIntersection::OUTER_LOW);
            assert(il.intersections.back().type == SegmentIntersection::OUTER_HIGH);
            auto end = il.intersections.end() - 1;
            insert_after.clear();
            for (auto it = il.intersections.begin() + 1; it != end;) {
                if (it->type == SegmentIntersection::OUTER_HIGH) {
                    ++ it;
                    assert(it->type == SegmentIntersection::OUTER_LOW);
                    ++ it;
                } else {
                    assert(it->type == SegmentIntersection::INNER_LOW);
                    auto hi  = ++ it;
                    assert(hi->type == SegmentIntersection::INNER_HIGH);
                    auto lo2 = ++ it;
                    if (lo2->type == SegmentIntersection::INNER_LOW) {
                        // INNER_HIGH followed by INNER_LOW. The outer contour may have squeezed the inner contour into two separate loops.
                        // In that case one shall insert a phony OUTER_HIGH / OUTER_LOW pair.
                        int up = hi->vertical_up();
                        int dn = lo2->vertical_down();

                        assert(up == -1 || up > 0);
                        assert(dn == -1 || dn >= 0);
                        assert((up == -1 && dn == -1) || (dn + 1 == up));

                        bool pinched = dn + 1 != up;
                        if (pinched) {
                            // hi is not connected with its inner contour to lo2.
                            // Insert a phony OUTER_HIGH / OUTER_LOW pair.
#if 0
                            static int pinch_idx = 0;
                            printf("Pinched %d\n", pinch_idx++);
#endif
                            insert_after.emplace_back(hi - il.intersections.begin());
                        }
                    }
                }
            }

            if (! insert_after.empty()) {
                // Insert phony OUTER_HIGH / OUTER_LOW pairs, adjust indices pointing to intersection points on this contour.
                map.clear();
                {
                    size_t i = 0;
                    temp_intersections.clear();
                    for (size_t idx_inset_after : insert_after) {
                        for (; i <= idx_inset_after; ++ i) {
                            map.emplace_back(temp_intersections.size());
                            temp_intersections.emplace_back(il.intersections[i]);
                        }
                        coord_t pos = (temp_intersections.back().pos() + il.intersections[i].pos()) / 2;
                        temp_intersections.emplace_back(phony_outer_intersection(SegmentIntersection::OUTER_HIGH, pos));
                        temp_intersections.emplace_back(phony_outer_intersection(SegmentIntersection::OUTER_LOW, pos));
                    }
                    for (; i < il.intersections.size(); ++ i) {
                        map.emplace_back(temp_intersections.size());
                        temp_intersections.emplace_back(il.intersections[i]);
                    }
                    temp_intersections.swap(il.intersections);
                }
                // Reindex references on current intersection line.
                for (SegmentIntersection &ip : il.intersections) {
                    if (ip.has_left_vertical())
                        ip.prev_on_contour = map[ip.prev_on_contour];
                    if (ip.has_right_vertical())
                        ip.next_on_contour = map[ip.next_on_contour];
                }
                // Reindex references on previous intersection line.
                for (SegmentIntersection &ip : segs[i_vline - 1].intersections)
                    if (ip.has_right_horizontal())
                        ip.next_on_contour = map[ip.next_on_contour];
                if (i_vline + 1 < segs.size()) {
                    // Reindex references on next intersection line.
                    for (SegmentIntersection &ip : segs[i_vline + 1].intersections)
                        if (ip.has_left_horizontal())
                            ip.prev_on_contour = map[ip.prev_on_contour];
                }
            }
        }
    }

    assert(validate_segment_intersection_connectivity(segs));
}

// Find the last INNER_HIGH intersection starting with INNER_LOW, that is followed by OUTER_HIGH intersection.
// Such intersection shall always exist.
static const SegmentIntersection& end_of_vertical_run_raw(const SegmentIntersection &start)
{
	assert(start.type == SegmentIntersection::INNER_LOW);
    // Step back to the beginning of the vertical segment to mark it as consumed.
    auto *it = &start;
    do {
        ++ it;
    } while (it->type != SegmentIntersection::OUTER_HIGH);
    if ((it - 1)->is_inner()) {
        // Step back.
        -- it;
        assert(it->type == SegmentIntersection::INNER_HIGH);
    }
    return *it;
}

// Find the last INNER_HIGH intersection starting with INNER_LOW, that is followed by OUTER_HIGH intersection, traversing vertical up contours if enabled.
// Such intersection shall always exist.
static const SegmentIntersection& end_of_vertical_run(const SegmentedIntersectionLine &il, const SegmentIntersection &start)
{
	assert(start.type == SegmentIntersection::INNER_LOW);
	const SegmentIntersection *end = &end_of_vertical_run_raw(start);
	assert(end->type == SegmentIntersection::INNER_HIGH);
	for (;;) {
		int up = end->vertical_up();
		if (up == -1 || (end->has_left_vertical_up() ? end->prev_on_contour_quality : end->next_on_contour_quality) != SegmentIntersection::LinkQuality::Valid)
			break;
		const SegmentIntersection &new_start = il.intersections[up];
		assert(end->iContour == new_start.iContour);
		assert(new_start.type == SegmentIntersection::INNER_LOW);
		end = &end_of_vertical_run_raw(new_start);
	}
	assert(end->type == SegmentIntersection::INNER_HIGH);
	return *end;
}
static SegmentIntersection& end_of_vertical_run(SegmentedIntersectionLine &il, SegmentIntersection &start)
{
	return const_cast<SegmentIntersection&>(end_of_vertical_run(std::as_const(il), std::as_const(start)));
}

static void traverse_graph_generate_polylines(const ExPolygonWithOffset              &poly_with_offset,
                                              const FillParams                       &params,
                                              std::vector<SegmentedIntersectionLine> &segs,
                                              const bool                              consistent_pattern,
                                              Polylines                              &polylines_out)
{
    // For each outer only chords, measure their maximum distance to the bow of the outer contour.
    // Mark an outer only chord as consumed, if the distance is low.
    for (int i_vline = 0; i_vline < int(segs.size()); ++ i_vline) {
        SegmentedIntersectionLine &vline = segs[i_vline];
        for (int i_intersection = 0; i_intersection + 1 < int(vline.intersections.size()); ++ i_intersection) {
            if (vline.intersections[i_intersection].type == SegmentIntersection::OUTER_LOW &&
                vline.intersections[i_intersection + 1].type == SegmentIntersection::OUTER_HIGH) {
                bool consumed = false;
                //                if (params.full_infill()) {
                //                        measure_outer_contour_slab(poly_with_offset, segs, i_vline, i_ntersection);
                //                } else
                consumed = true;
                vline.intersections[i_intersection].consumed_vertical_up = consumed;
            }
        }
    }

    // Now construct a graph.
    // Find the first point.
    // Naively one would expect to achieve best results by chaining the paths by the shortest distance,
    // but that procedure does not create the longest continuous paths.
    // A simple "sweep left to right" procedure achieves better results.
    int    	  i_vline = 0;
    int    	  i_intersection = -1;
    // Follow the line, connect the lines into a graph.
    // Until no new line could be added to the output path:
    Point     pointLast;
    Polyline* polyline_current = nullptr;
    if (! polylines_out.empty())
        pointLast = polylines_out.back().points.back();
    for (;;) {
        if (i_intersection == -1) {
            // The path has been interrupted. Find a next starting point.
            for (int i_vline2 = 0; i_vline2 < int(segs.size()); ++i_vline2) {
                const SegmentedIntersectionLine &vline = segs[i_vline2];
                if (!vline.intersections.empty()) {
                    assert(vline.intersections.size() > 1);
                    // Even number of intersections with the loops.
                    assert((vline.intersections.size() & 1) == 0);
                    assert(vline.intersections.front().type == SegmentIntersection::OUTER_LOW);

                    // For infill that needs to be consistent between layers (like Zig Zag),
                    // we are switching between forward and backward passes based on the line index.
                    const bool forward_pass = !consistent_pattern || (i_vline2 % 2 == 0);
                    for (int i = 0; i < int(vline.intersections.size()); ++i) {
                        const int                  intrsctn_idx = forward_pass ? i : int(vline.intersections.size()) - i - 1;
                        const SegmentIntersection &intrsctn     = vline.intersections[intrsctn_idx];
                        if (intrsctn.is_outer()) {
                            assert(intrsctn.is_low() || intrsctn_idx > 0);
                            const bool consumed = intrsctn.is_low() ? intrsctn.consumed_vertical_up : vline.intersections[intrsctn_idx - 1].consumed_vertical_up;
                            if (!consumed) {
                                i_vline        = i_vline2;
                                i_intersection = intrsctn_idx;
                                goto found;
                            }
                        }
                    }
                }
            }
            if (i_intersection == -1)
                // We are finished.
                break;
        found:
            // Start a new path.
            polylines_out.push_back(Polyline());
            polyline_current = &polylines_out.back();
            // Emit the first point of a path.
            pointLast = Point(segs[i_vline].pos, segs[i_vline].intersections[i_intersection].pos());
            polyline_current->points.push_back(pointLast);
        }

        // From the initial point (i_vline, i_intersection), follow a path.
        SegmentedIntersectionLine &vline 		= segs[i_vline];
        SegmentIntersection 	  *it 			= &vline.intersections[i_intersection];
        bool 					   going_up 	= it->is_low();
        bool 					   try_connect 	= false;
        if (going_up) {
            assert(! it->consumed_vertical_up);
            assert(size_t(i_intersection + 1) < vline.intersections.size());
            // Step back to the beginning of the vertical segment to mark it as consumed.
            if (it->is_inner()) {
                assert(i_intersection > 0);
                -- it;
                -- i_intersection;
            }
            // Consume the complete vertical segment up to the outer contour.
            do {
                it->consumed_vertical_up = true;
                ++ it;
                ++ i_intersection;
                assert(size_t(i_intersection) < vline.intersections.size());
            } while (it->type != SegmentIntersection::OUTER_HIGH);
            if ((it - 1)->is_inner()) {
                // Step back.
                -- it;
                -- i_intersection;
                assert(it->type == SegmentIntersection::INNER_HIGH);
                try_connect = true;
            }
        } else {
            // Going down.
            assert(it->is_high());
            assert(i_intersection > 0);
            assert(!(it - 1)->consumed_vertical_up);
            // Consume the complete vertical segment up to the outer contour.
            if (it->is_inner())
                it->consumed_vertical_up = true;
            do {
                assert(i_intersection > 0);
                -- it;
                -- i_intersection;
                it->consumed_vertical_up = true;
            } while (it->type != SegmentIntersection::OUTER_LOW);
            if ((it + 1)->is_inner()) {
                // Step back.
                ++ it;
                ++ i_intersection;
                assert(it->type == SegmentIntersection::INNER_LOW);
                try_connect = true;
            }
        }
        if (try_connect) {
            // Decide, whether to finish the segment, or whether to follow the perimeter.
            // 1) Find possible connection points on the previous / next vertical line.
        	int  i_prev = it->left_horizontal();
        	int  i_next = it->right_horizontal();

            // To ensure pattern consistency between layers for Zig Zag infill, we always
            // try to connect to the next vertical line and never to the previous vertical line.
            bool intersection_prev_valid = intersection_on_prev_vertical_line_valid(segs, i_vline, i_intersection) && !consistent_pattern;
            bool intersection_next_valid = intersection_on_next_vertical_line_valid(segs, i_vline, i_intersection);
            bool intersection_horizontal_valid = intersection_prev_valid || intersection_next_valid;

            // Mark both the left and right connecting segment as consumed, because one cannot go to this intersection point as it has been consumed.
            if (i_prev != -1)
                segs[i_vline - 1].intersections[i_prev].consumed_perimeter_right = true;
            if (i_next != -1)
                it->consumed_perimeter_right = true;

            // Try to connect to a previous or next vertical line, making a zig-zag pattern.
            if (intersection_horizontal_valid) {
            	// A horizontal connection along the perimeter line exists.
	            assert(it->is_inner());
            	bool take_next = intersection_next_valid;
            	if (intersection_prev_valid && intersection_next_valid) {
            		// Take the shorter segment. This greedy heuristics may not be the best.
            		coordf_t dist_prev = measure_perimeter_horizontal_segment_length(poly_with_offset, segs, i_vline - 1, i_prev, i_intersection);
	                coordf_t dist_next = measure_perimeter_horizontal_segment_length(poly_with_offset, segs, i_vline, i_intersection, i_next);
	                take_next = dist_next < dist_prev;
	            }
                polyline_current->points.emplace_back(vline.pos, it->pos());
                emit_perimeter_prev_next_segment(poly_with_offset, segs, i_vline, it->iContour, i_intersection, take_next ? i_next : i_prev, *polyline_current, take_next);
                //FIXME consume the left / right connecting segments at the other end of this line? Currently it is not critical because a perimeter segment is not followed if the vertical segment at the other side has already been consumed.
                // Advance to the neighbor line.
                if (take_next) {
                    ++ i_vline;
                    i_intersection = i_next;
                }
                else {
                    -- i_vline;
                    i_intersection = i_prev;
                }
                continue;
            }

            // Try to connect to a previous or next point on the same vertical line.
            int i_vertical = it->vertical_outside();
            auto vertical_link_quality = (i_vertical == -1 || vline.intersections[i_vertical + (going_up ? 0 : -1)].consumed_vertical_up) ? 
            	SegmentIntersection::LinkQuality::Invalid : it->vertical_outside_quality();
#if 0            	
            if (vertical_link_quality == SegmentIntersection::LinkQuality::Valid ||
            	// Follow the link if there is no horizontal link available.
            	(! intersection_horizontal_valid && vertical_link_quality != SegmentIntersection::LinkQuality::Invalid)) {
#else
           	if (vertical_link_quality != SegmentIntersection::LinkQuality::Invalid) {
#endif
                assert(it->iContour == vline.intersections[i_vertical].iContour);
                polyline_current->points.emplace_back(vline.pos, it->pos());
                if (vertical_link_quality == SegmentIntersection::LinkQuality::Valid)
                    // Consume the connecting contour and the next segment.
                    emit_perimeter_segment_on_vertical_line(poly_with_offset, segs, i_vline, it->iContour, i_intersection, i_vertical,
                        *polyline_current, going_up ? it->has_left_vertical_up() : it->has_right_vertical_down());
                else {
                    // Just skip the connecting contour and start a new path.
                    polylines_out.emplace_back();
                    polyline_current = &polylines_out.back();
                    polyline_current->points.emplace_back(vline.pos, vline.intersections[i_vertical].pos());
                }
                // Mark both the left and right connecting segment as consumed, because one cannot go to this intersection point as it has been consumed.
                // If there are any outer intersection points skipped (bypassed) by the contour,
                // mark them as processed.
                if (going_up)
                    for (int i = i_intersection; i < i_vertical; ++i)
                        vline.intersections[i].consumed_vertical_up = true;
                else
                    for (int i = i_vertical; i < i_intersection; ++i)
                        vline.intersections[i].consumed_vertical_up = true;
                // seg.intersections[going_up ? i_intersection : i_intersection - 1].consumed_vertical_up = true;
                it->consumed_perimeter_right = true;
                (going_up ? ++it : --it)->consumed_perimeter_right = true;
                i_intersection = i_vertical;
                continue;
            }

            // No way to continue the current polyline. Take the rest of the line up to the outer contour.
            // This will finish the polyline, starting another polyline at a new point.
            going_up ? ++ it : -- it;
        }

        // Finish the current vertical line,
        // reset the current vertical line to pick a new starting point in the next round.
        assert(it->is_outer());
        assert(it->is_high() == going_up);
        pointLast = Point(vline.pos, it->pos());
        polyline_current->points.emplace_back(pointLast);
        // Handle duplicate points and zero length segments.
        polyline_current->remove_duplicate_points();
        assert(! polyline_current->has_duplicate_points());
        // Handle nearly zero length edges.
        if (polyline_current->points.size() <= 1 ||
            (polyline_current->points.size() == 2 &&
                std::abs(polyline_current->points.front()(0) - polyline_current->points.back()(0)) < SCALED_EPSILON &&
                std::abs(polyline_current->points.front()(1) - polyline_current->points.back()(1)) < SCALED_EPSILON))
            polylines_out.pop_back();
        it 				 = nullptr;
        i_intersection   = -1;
        polyline_current = nullptr;
    }
}

struct MonotonicRegion
{
    struct Boundary {
        int vline;
        int low;
        int high;
    };

    Boundary 	left;
    Boundary 	right;

    // Length when starting at left.low
    float 		len1 { 0.f };
    // Length when starting at left.high
    float 		len2 { 0.f };
    // If true, then when starting at left.low, then ending at right.high and vice versa.
    // If false, then ending at the same side as starting.
    bool 		flips { false };

    float       length(bool region_flipped) const { return region_flipped ? len2 : len1; }
    int 		left_intersection_point(bool region_flipped) const { return region_flipped ? left.high : left.low; }
    int 		right_intersection_point(bool region_flipped) const { return (region_flipped == flips) ? right.low : right.high; }

#if NDEBUG
    // Left regions are used to track whether all regions left to this one have already been printed.
    boost::container::small_vector<MonotonicRegion*, 4>	left_neighbors;
    // Right regions are held to pick a next region to be extruded using the "Ant colony" heuristics.
    boost::container::small_vector<MonotonicRegion*, 4>	right_neighbors;
#else
    // For debugging, use the normal vector as it is better supported by debug visualizers.
    std::vector<MonotonicRegion*> left_neighbors;
    std::vector<MonotonicRegion*> right_neighbors;
#endif
};

struct AntPath
{
	float length 	 { -1. }; 		// Length of the link to the next region.
	float visibility { -1. }; 		// 1 / length. Which length, just to the next region, or including the path accross the region?
	float pheromone  { 0 }; 		// <0, 1>
};

struct MonotonicRegionLink
{
    MonotonicRegion     *region;
    bool 				 flipped;
    // Distance of right side of this region to left side of the next region, if the "flipped" flag of this region and the next region 
    // is applied as defined.
    AntPath 			*next;
    // Distance of right side of this region to left side of the next region, if the "flipped" flag of this region and the next region
    // is applied in reverse order as if the zig-zags were flipped.
    AntPath 			*next_flipped;
};

// Matrix of paths (AntPath) connecting ends of MontonousRegions.
// AntPath lengths and their derived visibilities refer to the length of the perimeter line if such perimeter segment exists.
class AntPathMatrix
{
public:
	AntPathMatrix(
		const std::vector<MonotonicRegion> 			    &regions, 
		const ExPolygonWithOffset 						&poly_with_offset, 
		const std::vector<SegmentedIntersectionLine> 	&segs,
		const float 									 initial_pheromone) : 
		m_regions(regions),
		m_poly_with_offset(poly_with_offset),
		m_segs(segs),
		// From end of one region to the start of another region, both flipped or not flipped.
		m_matrix(regions.size() * regions.size() * 4, AntPath{ -1., -1., initial_pheromone}) {}

	void update_inital_pheromone(float initial_pheromone)
	{
		for (AntPath &ap : m_matrix)
			ap.pheromone = initial_pheromone;
	}

	AntPath& operator()(const MonotonicRegion &region_from, bool flipped_from, const MonotonicRegion &region_to, bool flipped_to)
	{
		int row = 2 * int(&region_from - m_regions.data()) + flipped_from;
		int col = 2 * int(&region_to   - m_regions.data()) + flipped_to;
		AntPath &path = m_matrix[row * m_regions.size() * 2 + col];
		if (path.length == -1.) {
			// This path is accessed for the first time. Update the length and cost.
			int i_from = region_from.right_intersection_point(flipped_from);
			int i_to   = region_to.left_intersection_point(flipped_to);
			const SegmentedIntersectionLine &vline_from = m_segs[region_from.right.vline];
			const SegmentedIntersectionLine &vline_to   = m_segs[region_to.left.vline];
			if (region_from.right.vline + 1 == region_from.left.vline) {
				int i_right = vline_from.intersections[i_from].right_horizontal();
				if (i_right == i_to && vline_from.intersections[i_from].next_on_contour_quality == SegmentIntersection::LinkQuality::Valid) {
					// Measure length along the contour.
	                path.length = unscale<float>(measure_perimeter_horizontal_segment_length(m_poly_with_offset, m_segs, region_from.right.vline, i_from, i_to));
				}
			}
			if (path.length == -1.) {
				// Just apply the Eucledian distance of the end points.
			    path.length = unscale<float>(Vec2f(vline_to.pos - vline_from.pos, vline_to.intersections[i_to].pos() - vline_from.intersections[i_from].pos()).norm());
			}
			path.visibility = 1.f / (path.length + float(EPSILON));
		}
		return path;
	}

	AntPath& operator()(const MonotonicRegionLink &region_from, const MonotonicRegion &region_to, bool flipped_to)
		{ return (*this)(*region_from.region, region_from.flipped, region_to, flipped_to); }
	AntPath& operator()(const MonotonicRegion &region_from, bool flipped_from, const MonotonicRegionLink &region_to)
		{ return (*this)(region_from, flipped_from, *region_to.region, region_to.flipped); }
    AntPath& operator()(const MonotonicRegionLink &region_from, const MonotonicRegionLink &region_to)
        { return (*this)(*region_from.region, region_from.flipped, *region_to.region, region_to.flipped); }

private:
	// Source regions, used for addressing and updating m_matrix.
	const std::vector<MonotonicRegion>    			&m_regions;
	// To calculate the intersection points and contour lengths.
	const ExPolygonWithOffset 						&m_poly_with_offset;
	const std::vector<SegmentedIntersectionLine> 	&m_segs;
	// From end of one region to the start of another region, both flipped or not flipped.
	//FIXME one may possibly use sparse representation of the matrix, likely using hashing.
	std::vector<AntPath>					         m_matrix;
};

static const SegmentIntersection& vertical_run_bottom(const SegmentedIntersectionLine &vline, const SegmentIntersection &start)
{
	assert(start.is_inner());
	const SegmentIntersection *it = &start;
	// Find the lowest SegmentIntersection::INNER_LOW starting with right.
	for (;;) {
		while (it->type != SegmentIntersection::INNER_LOW)
			-- it;
        if ((it - 1)->type == SegmentIntersection::INNER_HIGH)
            -- it;
        else {
            int down = it->vertical_down();
            if (down == -1 || it->vertical_down_quality() != SegmentIntersection::LinkQuality::Valid)
                break;
            it = &vline.intersections[down];
            assert(it->type == SegmentIntersection::INNER_HIGH);
        }
	}
	return *it;
}
static SegmentIntersection& vertical_run_bottom(SegmentedIntersectionLine& vline, SegmentIntersection& start)
{
    return const_cast<SegmentIntersection&>(vertical_run_bottom(std::as_const(vline), std::as_const(start)));
}

static const SegmentIntersection& vertical_run_top(const SegmentedIntersectionLine &vline, const SegmentIntersection &start)
{
	assert(start.is_inner());
	const SegmentIntersection *it = &start;
	// Find the lowest SegmentIntersection::INNER_LOW starting with right.
	for (;;) {
		while (it->type != SegmentIntersection::INNER_HIGH)
			++ it;
        if ((it + 1)->type == SegmentIntersection::INNER_LOW)
            ++ it;
        else {
            int up = it->vertical_up();
            if (up == -1 || it->vertical_up_quality() != SegmentIntersection::LinkQuality::Valid)
                break;
            it = &vline.intersections[up];
            assert(it->type == SegmentIntersection::INNER_LOW);
        }
	}
	return *it;
}
static SegmentIntersection& vertical_run_top(SegmentedIntersectionLine& vline, SegmentIntersection& start)
{
    return const_cast<SegmentIntersection&>(vertical_run_top(std::as_const(vline), std::as_const(start)));
}

static SegmentIntersection* overlap_bottom(SegmentIntersection &start, SegmentIntersection &end, SegmentedIntersectionLine &vline_this, SegmentedIntersectionLine &vline_other, SegmentIntersection::Side side)
{
	SegmentIntersection *other = nullptr;
    assert(start.is_inner());
    assert(end.is_inner());
    const SegmentIntersection *it = &start;
    for (;;) {
        if (it->is_inner()) {
            int i = it->horizontal(side);
            if (i != -1) {
                other = &vline_other.intersections[i];
                break;
            }
            if (it == &end)
                break;
        }
        if (it->type != SegmentIntersection::INNER_HIGH)
            ++ it;
        else if ((it + 1)->type == SegmentIntersection::INNER_LOW)
            ++ it;
        else {
            int up = it->vertical_up();
            if (up == -1 || it->vertical_up_quality() != SegmentIntersection::LinkQuality::Valid)
                break;
            it = &vline_this.intersections[up];
            assert(it->type == SegmentIntersection::INNER_LOW);
        }
    }
	return other == nullptr ? nullptr : &vertical_run_bottom(vline_other, *other);
}

static SegmentIntersection* overlap_top(SegmentIntersection &start, SegmentIntersection &end, SegmentedIntersectionLine &vline_this, SegmentedIntersectionLine &vline_other, SegmentIntersection::Side side)
{
    SegmentIntersection *other = nullptr;
    assert(start.is_inner());
    assert(end.is_inner());
    const SegmentIntersection *it = &end;
    for (;;) {
        if (it->is_inner()) {
            int i = it->horizontal(side);
            if (i != -1) {
                other = &vline_other.intersections[i];
                break;
            }
            if (it == &start)
                break;
        }
        if (it->type != SegmentIntersection::INNER_LOW)
            -- it;
        else if ((it - 1)->type == SegmentIntersection::INNER_HIGH)
            -- it;
        else {
            int down = it->vertical_down();
            if (down == -1 || it->vertical_down_quality() != SegmentIntersection::LinkQuality::Valid)
                break;
            it = &vline_this.intersections[down];
            assert(it->type == SegmentIntersection::INNER_HIGH);
        }
    }
	return other == nullptr ? nullptr : &vertical_run_top(vline_other, *other);
}

static std::pair<SegmentIntersection*, SegmentIntersection*> left_overlap(SegmentIntersection &start, SegmentIntersection &end, SegmentedIntersectionLine &vline_this, SegmentedIntersectionLine &vline_left)
{
	std::pair<SegmentIntersection*, SegmentIntersection*> out(nullptr, nullptr);
	out.first = overlap_bottom(start, end, vline_this, vline_left, SegmentIntersection::Side::Left);
	if (out.first != nullptr)
		out.second = overlap_top(start, end, vline_this, vline_left, SegmentIntersection::Side::Left);
    assert((out.first == nullptr && out.second == nullptr) || out.first < out.second);
	return out;
}

static std::pair<SegmentIntersection*, SegmentIntersection*> left_overlap(std::pair<SegmentIntersection*, SegmentIntersection*> &start_end, SegmentedIntersectionLine &vline_this, SegmentedIntersectionLine &vline_left)
{
	assert((start_end.first == nullptr) == (start_end.second == nullptr));
	return start_end.first == nullptr ? start_end : left_overlap(*start_end.first, *start_end.second, vline_this, vline_left);
}

static std::pair<SegmentIntersection*, SegmentIntersection*> right_overlap(SegmentIntersection &start, SegmentIntersection &end, SegmentedIntersectionLine &vline_this, SegmentedIntersectionLine &vline_right)
{
	std::pair<SegmentIntersection*, SegmentIntersection*> out(nullptr, nullptr);
	out.first = overlap_bottom(start, end, vline_this, vline_right, SegmentIntersection::Side::Right);
	if (out.first != nullptr)
		out.second = overlap_top(start, end, vline_this, vline_right, SegmentIntersection::Side::Right);
    assert((out.first == nullptr && out.second == nullptr) || out.first < out.second);
    return out;
}

static std::pair<SegmentIntersection*, SegmentIntersection*> right_overlap(std::pair<SegmentIntersection*, SegmentIntersection*> &start_end, SegmentedIntersectionLine &vline_this, SegmentedIntersectionLine &vline_right)
{
	assert((start_end.first == nullptr) == (start_end.second == nullptr));
	return start_end.first == nullptr ? start_end : right_overlap(*start_end.first, *start_end.second, vline_this, vline_right);
}

static std::vector<MonotonicRegion> generate_montonous_regions(std::vector<SegmentedIntersectionLine> &segs)
{
	std::vector<MonotonicRegion> monotonic_regions;

#ifndef NDEBUG
	#define SLIC3R_DEBUG_MONOTONOUS_REGIONS
#endif

#ifdef SLIC3R_DEBUG_MONOTONOUS_REGIONS
    std::vector<std::vector<std::pair<int, int>>> consumed(segs.size());
    auto test_overlap = [&consumed](int segment, int low, int high) {
        for (const std::pair<int, int>& interval : consumed[segment])
            if ((low >= interval.first && low <= interval.second) ||
                (interval.first >= low && interval.first <= high))
                return true;
        consumed[segment].emplace_back(low, high);
        return false;
    };
#else
    [[maybe_unused]] auto test_overlap = [](int, int, int) { return false; };
#endif

    for (int i_vline_seed = 0; i_vline_seed < int(segs.size()); ++ i_vline_seed) {
        SegmentedIntersectionLine  &vline_seed = segs[i_vline_seed];
        for (int i_intersection_seed = 1; i_intersection_seed + 1 < int(vline_seed.intersections.size()); ) {
            while (i_intersection_seed < int(vline_seed.intersections.size()) &&
	        	   vline_seed.intersections[i_intersection_seed].type != SegmentIntersection::INNER_LOW)
	        	++ i_intersection_seed;
            if (i_intersection_seed == int(vline_seed.intersections.size()))
                break;
			SegmentIntersection *start = &vline_seed.intersections[i_intersection_seed];
            SegmentIntersection *end   = &end_of_vertical_run(vline_seed, *start);
			if (! start->consumed_vertical_up) {
				// Draw a new monotonic region starting with this segment.
				// while there is only a single right neighbor
		        int i_vline = i_vline_seed;
                std::pair<SegmentIntersection*, SegmentIntersection*> left(start, end);
				MonotonicRegion region;
				region.left.vline = i_vline;
				region.left.low   = int(left.first  - vline_seed.intersections.data());
				region.left.high  = int(left.second - vline_seed.intersections.data());
				region.right      = region.left;
                assert(! test_overlap(region.left.vline, region.left.low, region.left.high));
				start->consumed_vertical_up = true;
				int num_lines = 1;
                while (++ i_vline < int(segs.size())) {
			        SegmentedIntersectionLine  &vline_left	= segs[i_vline - 1];
			        SegmentedIntersectionLine  &vline_right = segs[i_vline];
					std::pair<SegmentIntersection*, SegmentIntersection*> right 	      = right_overlap(left, vline_left, vline_right);
                    if (right.first == nullptr)
                        // No neighbor at the right side of the current segment.
                        break;
                    SegmentIntersection*                                  right_top_first = &vertical_run_top(vline_right, *right.first);
                    if (right_top_first != right.second)
                        // This segment overlaps with multiple segments at its right side.
                        break;
                    std::pair<SegmentIntersection*, SegmentIntersection*> right_left      = left_overlap(right, vline_right, vline_left);
					if (left != right_left)
						// Left & right draws don't overlap exclusively, right neighbor segment overlaps with multiple segments at its left.
						break;
					region.right.vline = i_vline;
					region.right.low   = int(right.first  - vline_right.intersections.data());
					region.right.high  = int(right.second - vline_right.intersections.data());
					right.first->consumed_vertical_up = true;
                    assert(! test_overlap(region.right.vline, region.right.low, region.right.high));
                    ++ num_lines;
					left = right;
				}
				// Even number of lines makes the infill zig-zag to exit on the other side of the region than where it starts.
				region.flips = (num_lines & 1) != 0;
                monotonic_regions.emplace_back(region);
			}
			i_intersection_seed = int(end - vline_seed.intersections.data()) + 1;
		}
    }

    return monotonic_regions;
}

#ifdef INFILL_DEBUG_OUTPUT
static void export_monotonous_regions_to_svg(
    const ExPolygonWithOffset                       &poly_with_offset,
    const std::vector<SegmentedIntersectionLine>    &segs,
    const std::vector<MonotonicRegion>              &monotonic_regions,
    const std::string                               &path)
{
    BoundingBox bbox = get_extents(poly_with_offset.polygons_src);
    bbox.offset(scale_(3.));

    ::Slic3r::SVG svg(path, bbox);
    svg.draw(poly_with_offset.polygons_src);
    svg.draw_outline(poly_with_offset.polygons_src, "green");
    svg.draw_outline(poly_with_offset.polygons_outer, "green");
    svg.draw_outline(poly_with_offset.polygons_inner, "green");

    // Draw the infill line candidates in red.
    for (const SegmentedIntersectionLine &sil : segs) {
        for (size_t i = 0; i + 1 < sil.intersections.size(); ++ i)
            if (sil.intersections[i].type == SegmentIntersection::INNER_LOW && sil.intersections[i + 1].type == SegmentIntersection::INNER_HIGH) {
                Line l(Point(sil.pos, sil.intersections[i].pos()), Point(sil.pos, sil.intersections[i + 1].pos()));
                svg.draw(l, "blue");
            } else if (sil.intersections[i].type == SegmentIntersection::INNER_HIGH && sil.intersections[i].has_vertical_up()) {
                std::string color;
                const SegmentIntersection *it = &sil.intersections[i];
                switch (it->vertical_up_quality()) {
                case SegmentIntersection::LinkQuality::Invalid:     color = "red"; break;
                case SegmentIntersection::LinkQuality::Valid:       color = "blue"; break;
                case SegmentIntersection::LinkQuality::TooLong:
                default:                                            color = "yellow"; break;
                }
                Polyline polyline;
                polyline.points.push_back({ sil.pos, it->pos() });
                emit_perimeter_segment_on_vertical_line(poly_with_offset, segs, &sil - segs.data() , it->iContour, it - sil.intersections.data(), it->vertical_up(), polyline, it->has_left_vertical_up());
                svg.draw(polyline, color, scale_(0.05));
            }
    }

    // Draw the monotonic regions.
    for (const MonotonicRegion &region : monotonic_regions) {
        auto draw_boundary_line = [&poly_with_offset, &segs, &svg](const MonotonicRegion::Boundary &boundary) {
            const SegmentedIntersectionLine &sil = segs[boundary.vline];
            for (size_t i = boundary.low; i < boundary.high; ++ i)
                if (sil.intersections[i].type == SegmentIntersection::INNER_LOW && sil.intersections[i + 1].type == SegmentIntersection::INNER_HIGH) {
                    Line l(Point(sil.pos, sil.intersections[i].pos()), Point(sil.pos, sil.intersections[i + 1].pos()));
                    svg.draw(l, "red", scale_(0.05));
                }
        };
        draw_boundary_line(region.left);
        draw_boundary_line(region.right);
    }
}
#endif // INFILL_DEBUG_OUTPUT

// Traverse path, calculate length of the draw for the purpose of optimization.
// This function is very similar to polylines_from_paths() in the way how it traverses the path, but
// polylines_from_paths() emits a path, while this function just calculates the path length.
static float montonous_region_path_length(const MonotonicRegion &region, bool dir, const ExPolygonWithOffset &poly_with_offset, const std::vector<SegmentedIntersectionLine> &segs)
{
    // From the initial point (i_vline, i_intersection), follow a path.
	int   i_intersection = region.left_intersection_point(dir);
	int   i_vline 		 = region.left.vline;
	float total_length   = 0.;
	bool  no_perimeter   = false;
	Vec2f last_point;

	for (;;) {
        const SegmentedIntersectionLine &vline 		= segs[i_vline];
        const SegmentIntersection       *it    		= &vline.intersections[i_intersection];
        const bool                       going_up   = it->is_low();

        if (no_perimeter)
        	total_length += (last_point - Vec2f(vline.pos, (it + (going_up ? - 1 : 1))->pos())).norm();

        int iright = it->right_horizontal();
        if (going_up) {
            // Traverse the complete vertical segment up to the inner contour.
            for (;;) {
	            do {
	                ++ it;
					iright = std::max(iright, it->right_horizontal());
                    assert(it->is_inner());
                } while (it->type != SegmentIntersection::INNER_HIGH || (it + 1)->type != SegmentIntersection::OUTER_HIGH);
	            int inext = it->vertical_up();
                if (inext == -1 || it->vertical_up_quality() != SegmentIntersection::LinkQuality::Valid)
	            	break;
                assert(it->iContour == vline.intersections[inext].iContour);
                it = vline.intersections.data() + inext;
            } 
        } else {
            // Going down.
            assert(it->is_high());
            assert(i_intersection > 0);
            for (;;) {
	            do {
	                -- it;
	                if (int iright_new = it->right_horizontal(); iright_new != -1)
	                	iright = iright_new;
                    assert(it->is_inner());
	            } while (it->type != SegmentIntersection::INNER_LOW || (it - 1)->type != SegmentIntersection::OUTER_LOW);
	            int inext = it->vertical_down();
	            if (inext == -1 || it->vertical_down_quality() != SegmentIntersection::LinkQuality::Valid)
	            	break;
                assert(it->iContour == vline.intersections[inext].iContour);
                it = vline.intersections.data() + inext;
            } 
        }

        if (i_vline == region.right.vline)
        	break;

        int inext = it->right_horizontal();
        assert(iright != -1);
        assert(inext == -1 || inext == iright);

        // Find the end of the next overlapping vertical segment.
        const SegmentedIntersectionLine &vline_right = segs[i_vline + 1];
        const SegmentIntersection       *right       = going_up ? 
            &vertical_run_top(vline_right, vline_right.intersections[iright]) : &vertical_run_bottom(vline_right, vline_right.intersections[iright]);
        i_intersection = int(right - vline_right.intersections.data());

        if (inext == i_intersection && it->next_on_contour_quality == SegmentIntersection::LinkQuality::Valid) {
        	// Summarize length of the connection line along the perimeter.
        	//FIXME should it be weighted with a lower weight than non-extruding connection line? What weight?
        	// Taking half of the length.
    		total_length += 0.5f * float(measure_perimeter_horizontal_segment_length(poly_with_offset, segs, i_vline, it - vline.intersections.data(), inext));
			// Don't add distance to the next vertical line start to the total length.
			no_perimeter = false;
        } else {
	        // Finish the current vertical line,
        	going_up ? ++ it : -- it;
	        assert(it->is_outer());
	        assert(it->is_high() == going_up);
	        // Mark the end of this vertical line.
			last_point = Vec2f(vline.pos, it->pos());
			// Remember to add distance to the last point.
			no_perimeter = true;
        }

        ++ i_vline;
    }

    return unscale<float>(total_length);
}

static void connect_monotonic_regions(std::vector<MonotonicRegion> &regions, const ExPolygonWithOffset &poly_with_offset, std::vector<SegmentedIntersectionLine> &segs)
{
	// Map from low intersection to left / right side of a monotonic region.
	using MapType = std::pair<SegmentIntersection*, MonotonicRegion*>;
	std::vector<MapType> map_intersection_to_region_start;
	std::vector<MapType> map_intersection_to_region_end;
	map_intersection_to_region_start.reserve(regions.size());
	map_intersection_to_region_end.reserve(regions.size());
	for (MonotonicRegion &region : regions) {
		map_intersection_to_region_start.emplace_back(&segs[region.left.vline].intersections[region.left.low], &region);
		map_intersection_to_region_end.emplace_back(&segs[region.right.vline].intersections[region.right.low], &region);
	}
	auto intersections_lower = [](const MapType &l, const MapType &r){ return l.first < r.first ; };
    std::sort(map_intersection_to_region_start.begin(), map_intersection_to_region_start.end(), intersections_lower);
	std::sort(map_intersection_to_region_end.begin(), map_intersection_to_region_end.end(), intersections_lower);

	// Scatter links to neighboring regions.
	for (MonotonicRegion &region : regions) {
		if (region.left.vline > 0) {
			auto &vline = segs[region.left.vline];
            auto &vline_left = segs[region.left.vline - 1];
            auto[lbegin, lend] = left_overlap(vline.intersections[region.left.low], vline.intersections[region.left.high], vline, vline_left);
            if (lbegin != nullptr) {
                for (;;) {
                    MapType key(lbegin, nullptr);
                    auto it = std::lower_bound(map_intersection_to_region_end.begin(), map_intersection_to_region_end.end(), key);
                    assert(it != map_intersection_to_region_end.end() && it->first == key.first);
				    it->second->right_neighbors.emplace_back(&region);
				    SegmentIntersection *lnext = &vertical_run_top(vline_left, *lbegin);
				    if (lnext == lend)
					    break;
				    while (lnext->type != SegmentIntersection::INNER_LOW)
					    ++ lnext;
				    lbegin = lnext;
			    }
            }
		}
        if (region.right.vline + 1 < int(segs.size())) {
			auto &vline = segs[region.right.vline];
            auto &vline_right = segs[region.right.vline + 1];
            auto [rbegin, rend] = right_overlap(vline.intersections[region.right.low], vline.intersections[region.right.high], vline, vline_right);
            if (rbegin != nullptr) {
			    for (;;) {
				    MapType key(rbegin, nullptr);
				    auto it = std::lower_bound(map_intersection_to_region_start.begin(), map_intersection_to_region_start.end(), key);
				    assert(it != map_intersection_to_region_start.end() && it->first == key.first);
				    it->second->left_neighbors.emplace_back(&region);
				    SegmentIntersection *rnext = &vertical_run_top(vline_right, *rbegin);
				    if (rnext == rend)
					    break;
				    while (rnext->type != SegmentIntersection::INNER_LOW)
					    ++ rnext;
				    rbegin = rnext;
			    }
            }
		}
	}

	// Sometimes a segment may indicate that it connects to a segment on the other side while the other does not.
    // This may be a valid case if one side contains runs of OUTER_LOW, INNER_LOW, {INNER_HIGH, INNER_LOW}*, INNER_HIGH, OUTER_HIGH,
    // where the part in the middle does not connect to the other side, but it will be extruded through.
    for (MonotonicRegion &region : regions) {
        std::sort(region.left_neighbors.begin(),  region.left_neighbors.end());
        std::sort(region.right_neighbors.begin(), region.right_neighbors.end());
    }
    for (MonotonicRegion &region : regions) {
        for (MonotonicRegion *neighbor : region.left_neighbors) {
            auto it = std::lower_bound(neighbor->right_neighbors.begin(), neighbor->right_neighbors.end(), &region);
            if (it == neighbor->right_neighbors.end() || *it != &region)
                neighbor->right_neighbors.insert(it, &region);
        }
        for (MonotonicRegion *neighbor : region.right_neighbors) {
            auto it = std::lower_bound(neighbor->left_neighbors.begin(), neighbor->left_neighbors.end(), &region);
            if (it == neighbor->left_neighbors.end() || *it != &region)
                neighbor->left_neighbors.insert(it, &region);
        }
    }

#ifndef NDEBUG
    // Verify symmetry of the left_neighbors / right_neighbors.
    for (MonotonicRegion &region : regions) {
        for (MonotonicRegion *neighbor : region.left_neighbors) {
            assert(std::count(region.left_neighbors.begin(), region.left_neighbors.end(), neighbor) == 1);
            assert(std::find(neighbor->right_neighbors.begin(), neighbor->right_neighbors.end(), &region) != neighbor->right_neighbors.end());
        }
        for (MonotonicRegion *neighbor : region.right_neighbors) {
            assert(std::count(region.right_neighbors.begin(), region.right_neighbors.end(), neighbor) == 1);
            assert(std::find(neighbor->left_neighbors.begin(), neighbor->left_neighbors.end(), &region) != neighbor->left_neighbors.end());
        }
    }
#endif /* NDEBUG */

    // Fill in sum length of connecting lines of a region. This length is used for optimizing the infill path for minimum length.
    for (MonotonicRegion &region : regions) {
    	region.len1 = montonous_region_path_length(region, false, poly_with_offset, segs);
    	region.len2 = montonous_region_path_length(region, true,  poly_with_offset, segs);
    	// Subtract the smaller length from the longer one, so we will optimize just with the positive difference of the two.
    	if (region.len1 > region.len2) {
    		region.len1 -= region.len2;
    		region.len2 = 0;
    	} else {
    		region.len2 -= region.len1;
    		region.len1 = 0;
    	}
    }
}

// Raad Salman: Algorithms for the Precedence Constrained Generalized Travelling Salesperson Problem
// https://www.chalmers.se/en/departments/math/research/research-groups/optimization/OptimizationMasterTheses/MScThesis-RaadSalman-final.pdf
// Algorithm 6.1 Lexicographic Path Preserving 3-opt
// Optimize path while maintaining the ordering constraints.
void monotonic_3_opt(std::vector<MonotonicRegionLink> &path, const std::vector<SegmentedIntersectionLine> &segs)
{
	// When doing the 3-opt path preserving flips, one has to fulfill two constraints:
	//
	// 1) The new path should be shorter than the old path.
	// 2) The precedence constraints shall be satisified on the new path.
	//
	// Branch & bound with KD-tree may be used with the shorter path constraint, but the precedence constraint will have to be recalculated for each
	// shorter path candidate found, which has a quadratic cost for a dense precedence graph. For a sparse precedence graph the precedence
	// constraint verification will be cheaper.
	//
	// On the other side, if the full search space is traversed as in the diploma thesis by Raad Salman (page 24, Algorithm 6.1 Lexicographic Path Preserving 3-opt),
	// then the precedence constraint verification is amortized inside the O(n^3) loop. Now which is better for our task?
	//
	// It is beneficial to also try flipping of the infill zig-zags, for which a prefix sum of both flipped and non-flipped paths over
	// MonotonicRegionLinks may be utilized, however updating the prefix sum has a linear complexity, the same complexity as doing the 3-opt
	// exchange by copying the pieces.
}

// #define SLIC3R_DEBUG_ANTS

template<typename... TArgs>
inline void print_ant(const std::string& fmt, TArgs&&... args) {
#ifdef SLIC3R_DEBUG_ANTS
    std::cout << Slic3r::format(fmt, std::forward<TArgs>(args)...) << std::endl;
#endif
}

// Find a run through monotonic infill blocks using an 'Ant colony" optimization method.
// http://www.scholarpedia.org/article/Ant_colony_optimization
static std::vector<MonotonicRegionLink> chain_monotonic_regions(
	std::vector<MonotonicRegion> &regions, const ExPolygonWithOffset &poly_with_offset, const std::vector<SegmentedIntersectionLine> &segs, std::mt19937_64 &rng)
{
	// Number of left neighbors (regions that this region depends on, this region cannot be printed before the regions left of it are printed) + self.
	std::vector<int32_t>			left_neighbors_unprocessed(regions.size(), 1);
	// Queue of regions, which have their left neighbors already printed.
	std::vector<MonotonicRegion*> 	queue;
	queue.reserve(regions.size());
	for (MonotonicRegion &region : regions)
		if (region.left_neighbors.empty())
			queue.emplace_back(&region);
		else
			left_neighbors_unprocessed[&region - regions.data()] += int(region.left_neighbors.size());
	// Make copy of structures that need to be initialized at each ant iteration.
	auto left_neighbors_unprocessed_initial = left_neighbors_unprocessed;
	auto queue_initial 						= queue;

	std::vector<MonotonicRegionLink> path, best_path;
	path.reserve(regions.size());
	best_path.reserve(regions.size());
	float best_path_length = std::numeric_limits<float>::max();

	struct NextCandidate {
        MonotonicRegion    *region = nullptr;
        AntPath  	        *link;
        AntPath  	        *link_flipped;
        float                probability;
        bool 		         dir = false;
	};
	std::vector<NextCandidate> next_candidates;

    [[maybe_unused]]auto validate_unprocessed =
#ifdef NDEBUG
        []() { return true; };
#else
        [&regions, &left_neighbors_unprocessed, &path, &queue]() {
            std::vector<unsigned char> regions_processed(regions.size(), false);
            std::vector<unsigned char> regions_in_queue(regions.size(), false);
            for (const MonotonicRegion *region : queue) {
            	// This region is not processed yet, his predecessors are processed.
                assert(left_neighbors_unprocessed[region - regions.data()] == 1);
                regions_in_queue[region - regions.data()] = true;
            }
            for (const MonotonicRegionLink &link : path) {
                assert(left_neighbors_unprocessed[link.region - regions.data()] == 0);
                regions_processed[link.region - regions.data()] = true;
            }
            for (size_t i = 0; i < regions_processed.size(); ++ i) {
                assert(! regions_processed[i] || ! regions_in_queue[i]);
                const MonotonicRegion &region = regions[i];
                if (regions_processed[i] || regions_in_queue[i]) {
                    assert(left_neighbors_unprocessed[i] == (regions_in_queue[i] ? 1 : 0));
                    // All left neighbors should be processed already.
                    for (const MonotonicRegion *left : region.left_neighbors) {
                        assert(regions_processed[left - regions.data()]);
                        assert(left_neighbors_unprocessed[left - regions.data()] == 0);
                    }
                } else {
                    // Some left neihgbor should not be processed yet.
                    assert(left_neighbors_unprocessed[i] > 1);
                    int32_t num_predecessors_unprocessed = 0;
                    bool   has_left_last_on_path       = false;
                    for (const MonotonicRegion* left : region.left_neighbors) {
                        size_t iprev = left - regions.data();
                        if (regions_processed[iprev]) {
                        	assert(left_neighbors_unprocessed[iprev] == 0);
                            if (left == path.back().region) {
                                // This region should actually be on queue, but to optimize the queue management
                                // this item will be processed in the next round by traversing path.back().region->right_neighbors before processing the queue.
                                assert(! has_left_last_on_path);
                                has_left_last_on_path = true;
                                ++ num_predecessors_unprocessed;
                            }
                        } else {
                        	if (regions_in_queue[iprev])
	                    		assert(left_neighbors_unprocessed[iprev] == 1);
	                    	else 
	                    		assert(left_neighbors_unprocessed[iprev] > 1);
	                    	++ num_predecessors_unprocessed;
                        }
                    }
                    assert(num_predecessors_unprocessed > 0);
                    assert(left_neighbors_unprocessed[i] == num_predecessors_unprocessed + 1);
                }
            }
            return true;
        };
#endif /* NDEBUG */

	// How many times to repeat the ant simulation (number of ant generations).
	constexpr int const   num_rounds = 25;
	// After how many rounds without an improvement to exit?
	constexpr int const   num_rounds_no_change_exit = 8;
	// With how many ants each of the run will be performed?
	const int             num_ants = std::min(int(regions.size()), 10);
	// Base (initial) pheromone level. This value will be adjusted based on the length of the first greedy path found.
	float                 pheromone_initial_deposit = 0.5f;
	// Evaporation rate of pheromones.
	constexpr float const pheromone_evaporation = 0.1f;
    // Evaporation rate to diversify paths taken by individual ants.
    constexpr float const pheromone_diversification = 0.1f;
	// Probability at which to take the next best path. Otherwise take the the path based on the cost distribution.
	constexpr float const probability_take_best = 0.9f;
	// Exponents of the cost function.
	constexpr float const pheromone_alpha = 1.f; // pheromone exponent
	constexpr float const pheromone_beta  = 2.f; // attractiveness weighted towards edge length

    AntPathMatrix path_matrix(regions, poly_with_offset, segs, pheromone_initial_deposit);

    // Find an initial path in a greedy way, set the initial pheromone value to 10% of the cost of the greedy path.
    {
    	// Construct the first path in a greedy way to calculate an initial value of the pheromone value.
		queue = queue_initial;
		left_neighbors_unprocessed = left_neighbors_unprocessed_initial;
        assert(validate_unprocessed());
        // Pick the last of the queue.
        MonotonicRegionLink path_end { queue.back(), false };
        queue.pop_back();
        -- left_neighbors_unprocessed[path_end.region - regions.data()];

        float total_length = path_end.region->length(false);
		while (! queue.empty() || ! path_end.region->right_neighbors.empty()) {
            // Chain.
			MonotonicRegion 		    &region = *path_end.region;
			bool 			  			 dir    = path_end.flipped;
			NextCandidate 				 next_candidate;
			next_candidate.probability = 0;
			for (MonotonicRegion *next : region.right_neighbors) {
                assert(left_neighbors_unprocessed[next - regions.data()] > 1);
				if (left_neighbors_unprocessed[next - regions.data()] == 2) {
					// Dependencies of the successive blocks are satisfied.
                    AntPath &path1 = path_matrix(region, dir, *next, false);
                    AntPath &path2 = path_matrix(region, dir, *next, true);
                    if (path1.visibility > next_candidate.probability)
                        next_candidate = { next, &path1, &path1, path1.visibility, false };
                    if (path2.visibility > next_candidate.probability)
                        next_candidate = { next, &path2, &path2, path2.visibility, true  };
				}
			}
			bool from_queue = next_candidate.probability == 0;
            if (from_queue) {
                for (MonotonicRegion *next : queue) {
                    AntPath &path1 = path_matrix(region, dir, *next, false);
                    AntPath &path2 = path_matrix(region, dir, *next, true);
                    if (path1.visibility > next_candidate.probability)
                        next_candidate = { next, &path1, &path1, path1.visibility, false };
                    if (path2.visibility > next_candidate.probability)
                        next_candidate = { next, &path2, &path2, path2.visibility, true  };
                }
            }
            // Move the other right neighbors with satisified constraints to the queue.
			for (MonotonicRegion *next : region.right_neighbors)
				if (-- left_neighbors_unprocessed[next - regions.data()] == 1 && next_candidate.region != next)
	                queue.emplace_back(next);
            if (from_queue) {
                // Remove the selected path from the queue.
                auto it = std::find(queue.begin(), queue.end(), next_candidate.region);
                assert(it != queue.end());
                *it = queue.back();
                queue.pop_back();
            }
			// Extend the path.
            assert(next_candidate.region);
			MonotonicRegion *next_region = next_candidate.region;
			bool              next_dir    = next_candidate.dir;
            total_length += next_region->length(next_dir) + path_matrix(*path_end.region, path_end.flipped, *next_region, next_dir).length;
            path_end = { next_region, next_dir };
            assert(left_neighbors_unprocessed[next_region - regions.data()] == 1);
            left_neighbors_unprocessed[next_region - regions.data()] = 0;          
        }

        // Set an initial pheromone value to 10% of the greedy path's value.
        pheromone_initial_deposit = 0.1f / total_length;
        path_matrix.update_inital_pheromone(pheromone_initial_deposit);
    }

    // Probability (unnormalized) of traversing a link between two monotonic regions.
	auto path_probability = [
#ifndef __APPLE__
        // clang complains when capturing constexpr constants.
        pheromone_alpha, pheromone_beta
#endif // __APPLE__
        ](AntPath &path) {
		return pow(path.pheromone, pheromone_alpha) * pow(path.visibility, pheromone_beta);
	};

#ifdef SLIC3R_DEBUG_ANTS
    static int irun = 0;
    ++ irun;
#endif /* SLIC3R_DEBUG_ANTS */

    int num_rounds_no_change = 0;
    for (int round = 0; round < num_rounds && num_rounds_no_change < num_rounds_no_change_exit; ++ round)
	{
		bool improved = false;
		for (int ant = 0; ant < num_ants; ++ ant) 
		{
			// Find a new path following the pheromones deposited by the previous ants.
			print_ant("Round %1% ant %2%", round, ant);
			path.clear();
			queue = queue_initial;
			left_neighbors_unprocessed = left_neighbors_unprocessed_initial;
            assert(validate_unprocessed());
            // Pick randomly the first from the queue at random orientation.
            //FIXME picking the 1st monotonic region should likely be done based on accumulated pheromone level as well,
            // but the inefficiency caused by the random pick of the 1st monotonic region is likely insignificant.
            int first_idx = std::uniform_int_distribution<>(0, int(queue.size()) - 1)(rng);
            path.emplace_back(MonotonicRegionLink{ queue[first_idx], rng() > rng.max() / 2 });
            *(queue.begin() + first_idx) = std::move(queue.back());
            queue.pop_back();
            -- left_neighbors_unprocessed[path.back().region - regions.data()];
            assert(left_neighbors_unprocessed[path.back().region - regions.data()] == 0);
            assert(validate_unprocessed());
            print_ant("\tRegion (%1%:%2%,%3%) (%4%:%5%,%6%)",
				path.back().region->left.vline, 
                path.back().flipped ? path.back().region->left.high : path.back().region->left.low,
                path.back().flipped ? path.back().region->left.low  : path.back().region->left.high,
                path.back().region->right.vline, 
                path.back().flipped == path.back().region->flips ? path.back().region->right.high : path.back().region->right.low,
                path.back().flipped == path.back().region->flips ? path.back().region->right.low : path.back().region->right.high);

			while (! queue.empty() || ! path.back().region->right_neighbors.empty()) {
                // Chain.
				MonotonicRegion 		    &region = *path.back().region;
				bool 			  			 dir    = path.back().flipped;
				// Sort by distance to pt.
                next_candidates.clear();
				next_candidates.reserve(region.right_neighbors.size() * 2);
				for (MonotonicRegion *next : region.right_neighbors) {
					int &unprocessed = left_neighbors_unprocessed[next - regions.data()];
					assert(unprocessed > 1);
					if (-- unprocessed == 1) {
						// Dependencies of the successive blocks are satisfied.
                        AntPath &path1  	   = path_matrix(region,   dir, *next, false);
                        AntPath &path1_flipped = path_matrix(region, ! dir, *next, true);
                        AntPath &path2 	       = path_matrix(region,   dir, *next, true);
                        AntPath &path2_flipped = path_matrix(region, ! dir, *next, false);
                        next_candidates.emplace_back(NextCandidate{ next, &path1, &path1_flipped, path_probability(path1), false });
                        next_candidates.emplace_back(NextCandidate{ next, &path2, &path2_flipped, path_probability(path2), true  });
					}
				}
                size_t num_direct_neighbors = next_candidates.size();
                //FIXME add the queue items to the candidates? These are valid moves as well.
                if (num_direct_neighbors == 0) {
                    // Add the queue candidates.
                    for (MonotonicRegion *next : queue) {
                    	assert(left_neighbors_unprocessed[next - regions.data()] == 1);
                        AntPath &path1  	   = path_matrix(region,   dir, *next, false);
                        AntPath &path1_flipped = path_matrix(region, ! dir, *next, true);
                        AntPath &path2 	       = path_matrix(region,   dir, *next, true);
                        AntPath &path2_flipped = path_matrix(region, ! dir, *next, false);
                        next_candidates.emplace_back(NextCandidate{ next, &path1, &path1_flipped, path_probability(path1), false });
                        next_candidates.emplace_back(NextCandidate{ next, &path2, &path2_flipped, path_probability(path2), true  });
                    }
                }
				float dice = float(rng()) / float(rng.max());
                std::vector<NextCandidate>::iterator take_path;
				if (dice < probability_take_best) {
					// Take the highest probability path.
					take_path = std::max_element(next_candidates.begin(), next_candidates.end(), [](auto &l, auto &r){ return l.probability < r.probability; });
					print_ant("\tTaking best path at probability %1% below %2%", dice,  probability_take_best);
				} else {
					// Take the path based on the probability.
                    // Calculate the total probability.
                    float total_probability = std::accumulate(next_candidates.begin(), next_candidates.end(), 0.f, [](const float l, const NextCandidate& r) { return l + r.probability; });
					// Take a random path based on the probability.
                    float probability_threshold = float(rng()) * total_probability / float(rng.max());
                    take_path = next_candidates.end();
                    -- take_path;
                    for (auto it = next_candidates.begin(); it < next_candidates.end(); ++ it)
                        if ((probability_threshold -= it->probability) <= 0.) {
                            take_path = it;
                            break;
                        }
					print_ant("\tTaking path at probability threshold %1% of %2%", probability_threshold, total_probability);
				}
                // Move the other right neighbors with satisified constraints to the queue.
                for (std::vector<NextCandidate>::iterator it_next_candidate = next_candidates.begin(); it_next_candidate != next_candidates.begin() + num_direct_neighbors; ++ it_next_candidate)
                    if ((queue.empty() || it_next_candidate->region != queue.back()) && it_next_candidate->region != take_path->region)
                        queue.emplace_back(it_next_candidate->region);
                if (size_t(take_path - next_candidates.begin()) >= num_direct_neighbors) {
                    // Remove the selected path from the queue.
                    auto it = std::find(queue.begin(), queue.end(), take_path->region);
                    assert(it != queue.end());
                    *it = queue.back();
                    queue.pop_back();
                }
				// Extend the path.
				MonotonicRegion  *next_region = take_path->region;
				bool              next_dir    = take_path->dir;
                path.back().next         = take_path->link;
                path.back().next_flipped = take_path->link_flipped;
                path.emplace_back(MonotonicRegionLink{ next_region, next_dir });
                assert(left_neighbors_unprocessed[next_region - regions.data()] == 1);
                left_neighbors_unprocessed[next_region - regions.data()] = 0;
				print_ant("\tRegion (%1%:%2%,%3%) (%4%:%5%,%6%) length to prev %7%", 
                    next_region->left.vline, 
                    next_dir ? next_region->left.high : next_region->left.low,
                    next_dir ? next_region->left.low  : next_region->left.high,
					next_region->right.vline, 
                    next_dir == next_region->flips ? next_region->right.high : next_region->right.low,
                    next_dir == next_region->flips ? next_region->right.low  : next_region->right.high,
					take_path->link->length);

                print_ant("\tRegion (%1%:%2%,%3%) (%4%:%5%,%6%)",
                    path.back().region->left.vline,
                    path.back().flipped ? path.back().region->left.high : path.back().region->left.low,
                    path.back().flipped ? path.back().region->left.low : path.back().region->left.high,
                    path.back().region->right.vline,
                    path.back().flipped == path.back().region->flips ? path.back().region->right.high : path.back().region->right.low,
                    path.back().flipped == path.back().region->flips ? path.back().region->right.low : path.back().region->right.high);

				// Update pheromones along this link, see Ant Colony System (ACS) update rule.
				// http://www.scholarpedia.org/article/Ant_colony_optimization
				// The goal here is to lower the pheromone trace for paths taken to diversify the next path picked in the same batch of ants.
				take_path->link->pheromone = (1.f - pheromone_diversification) * take_path->link->pheromone + pheromone_diversification * pheromone_initial_deposit;
                assert(validate_unprocessed());
            }

			// Perform 3-opt local optimization of the path.
			monotonic_3_opt(path, segs);

			// Measure path length.
            assert(! path.empty());
            float path_length = std::accumulate(path.begin(), path.end() - 1,
                path.back().region->length(path.back().flipped),
                [&path_matrix](const float l, const MonotonicRegionLink &r) { 
                    const MonotonicRegionLink &next = *(&r + 1);
                    return l + r.region->length(r.flipped) + path_matrix(*r.region, r.flipped, *next.region, next.flipped).length;
                });
			// Save the shortest path.
			print_ant("\tThis length: %1%, shortest length: %2%", path_length, best_path_length);
			if (path_length < best_path_length) {
				best_path_length = path_length;
				std::swap(best_path, path);
#if 0 // #if ! defined(SLIC3R_DEBUG_ANTS) && ! defined(ndebug)
				if (round == 0 && ant == 0)
					std::cout << std::endl;
			    std::cout << Slic3r::format("round %1% ant %2% path length %3%", round, ant, path_length) << std::endl;
#endif
                if (path_length == 0)
                    // Perfect path found.
                    goto end;
                improved = true;
			}
		}

		// Reinforce the path pheromones with the best path.
        float total_cost = best_path_length + float(EPSILON);
        for (size_t i = 0; i + 1 < path.size(); ++ i) {
            MonotonicRegionLink &link = path[i];
            link.next->pheromone = (1.f - pheromone_evaporation) * link.next->pheromone + pheromone_evaporation / total_cost;
        }

        if (improved)
            num_rounds_no_change = 0;
        else
			++ num_rounds_no_change;
	}

end:
    return best_path;
}

// Traverse path, produce polylines.
static void polylines_from_paths(const std::vector<MonotonicRegionLink> &path, const ExPolygonWithOffset &poly_with_offset, const std::vector<SegmentedIntersectionLine> &segs, Polylines &polylines_out)
{
	Polyline *polyline = nullptr;
	auto finish_polyline = [&polyline, &polylines_out]() {
        polyline->remove_duplicate_points();
        // Handle duplicate points and zero length segments.
        assert(!polyline->has_duplicate_points());
        // Handle nearly zero length edges.
        if (polyline->points.size() <= 1 ||
            (polyline->points.size() == 2 &&
                std::abs(polyline->points.front().x() - polyline->points.back().x()) < SCALED_EPSILON &&
                std::abs(polyline->points.front().y() - polyline->points.back().y()) < SCALED_EPSILON))
            polylines_out.pop_back();
        else if (polylines_out.size() >= 2) {
            assert(polyline->points.size() >= 2);
            // Merge the two last polylines. An extrusion may have been split by an introduction of phony outer points on intersection lines
            // to cope with pinching of inner offset contours.
            Polyline &pl_prev = polylines_out[polylines_out.size() - 2];
            if (std::abs(polyline->points.front().x() - pl_prev.points.back().x()) < SCALED_EPSILON &&
                std::abs(polyline->points.front().y() - pl_prev.points.back().y()) < SCALED_EPSILON) {
                pl_prev.points.back() = (pl_prev.points.back() + polyline->points.front()) / 2;
                pl_prev.points.insert(pl_prev.points.end(), polyline->points.begin() + 1, polyline->points.end());
                polylines_out.pop_back();
            }
        }
    	polyline = nullptr;
    };

	for (const MonotonicRegionLink &path_segment : path) {
		MonotonicRegion &region = *path_segment.region;
		bool 		     dir    = path_segment.flipped;

        // From the initial point (i_vline, i_intersection), follow a path.
		int  i_intersection = region.left_intersection_point(dir);
		int  i_vline 		= region.left.vline;

        if (polyline != nullptr && &path_segment != path.data()) {
        	// Connect previous path segment with the new one.
        	const MonotonicRegionLink 	      &path_segment_prev  = *(&path_segment - 1);
			const MonotonicRegion 		      &region_prev		  = *path_segment_prev.region;
			bool 			  			       dir_prev 		  = path_segment_prev.flipped;
			int                                i_vline_prev       = region_prev.right.vline;
			const SegmentedIntersectionLine   &vline_prev         = segs[i_vline_prev];
			int 		       				   i_intersection_prev = region_prev.right_intersection_point(dir_prev);
			const SegmentIntersection         *ip_prev 			  = &vline_prev.intersections[i_intersection_prev];
			bool 						       extended           = false;
			if (i_vline_prev + 1 == i_vline) {
				if (ip_prev->right_horizontal() == i_intersection && ip_prev->next_on_contour_quality == SegmentIntersection::LinkQuality::Valid) {
		        	// Emit a horizontal connection contour.
		            emit_perimeter_prev_next_segment(poly_with_offset, segs, i_vline_prev, ip_prev->iContour, i_intersection_prev, i_intersection, *polyline, true);
		            extended = true;
				}
	        }
	        if (! extended) {
		        // Finish the current vertical line,
                assert(ip_prev->is_inner());
                ip_prev->is_low() ? -- ip_prev : ++ ip_prev;
		        assert(ip_prev->is_outer());
	        	polyline->points.back() = Point(vline_prev.pos, ip_prev->pos());
				finish_polyline();
			}
        }

		for (;;) {
	        const SegmentedIntersectionLine &vline = segs[i_vline];
            const SegmentIntersection       *it    = &vline.intersections[i_intersection];
            const bool                       going_up = it->is_low();
            if (polyline == nullptr) {
				polylines_out.emplace_back();
	            polyline = &polylines_out.back();
	            // Extend the infill line up to the outer contour.
	        	polyline->points.emplace_back(vline.pos, (it + (going_up ? - 1 : 1))->pos());
			} else
				polyline->points.emplace_back(vline.pos, it->pos());

			int iright = it->right_horizontal();
	        if (going_up) {
	            // Consume the complete vertical segment up to the inner contour.
	            for (;;) {
		            do {
		                ++ it;
						iright = std::max(iright, it->right_horizontal());
                        assert(it->is_inner());
                    } while (it->type != SegmentIntersection::INNER_HIGH || (it + 1)->type != SegmentIntersection::OUTER_HIGH);
	                polyline->points.emplace_back(vline.pos, it->pos());
		            int inext = it->vertical_up();
                    if (inext == -1 || it->vertical_up_quality() != SegmentIntersection::LinkQuality::Valid)
		            	break;
	                assert(it->iContour == vline.intersections[inext].iContour);
	                emit_perimeter_segment_on_vertical_line(poly_with_offset, segs, i_vline, it->iContour, it - vline.intersections.data(), inext, *polyline, it->has_left_vertical_up());
	                it = vline.intersections.data() + inext;
	            } 
	        } else {
	            // Going down.
                assert(it->is_high());
                assert(i_intersection > 0);
	            for (;;) {
		            do {
		                -- it;
		                if (int iright_new = it->right_horizontal(); iright_new != -1)
		                	iright = iright_new;
                        assert(it->is_inner());
		            } while (it->type != SegmentIntersection::INNER_LOW || (it - 1)->type != SegmentIntersection::OUTER_LOW);
	                polyline->points.emplace_back(vline.pos, it->pos());
		            int inext = it->vertical_down();
		            if (inext == -1 || it->vertical_down_quality() != SegmentIntersection::LinkQuality::Valid)
		            	break;
	                assert(it->iContour == vline.intersections[inext].iContour);
	                emit_perimeter_segment_on_vertical_line(poly_with_offset, segs, i_vline, it->iContour, it - vline.intersections.data(), inext, *polyline, it->has_right_vertical_down());
	                it = vline.intersections.data() + inext;
	            } 
	        }

	        if (i_vline == region.right.vline)
	        	break;

            int inext = it->right_horizontal();
            assert(iright != -1);
            assert(inext == -1 || inext == iright);

            // Find the end of the next overlapping vertical segment.
            const SegmentedIntersectionLine &vline_right = segs[i_vline + 1];
            const SegmentIntersection       *right       = going_up ? 
                &vertical_run_top(vline_right, vline_right.intersections[iright]) : &vertical_run_bottom(vline_right, vline_right.intersections[iright]);
            i_intersection = int(right - vline_right.intersections.data());

	        if (inext == i_intersection && it->next_on_contour_quality == SegmentIntersection::LinkQuality::Valid) {
	        	// Emit a horizontal connection contour.
	            emit_perimeter_prev_next_segment(poly_with_offset, segs, i_vline, it->iContour, it - vline.intersections.data(), inext, *polyline, true);
	        } else {
		        // Finish the current vertical line,
	        	going_up ? ++ it : -- it;
		        assert(it->is_outer());
		        assert(it->is_high() == going_up);
	        	polyline->points.back() = Point(vline.pos, it->pos());
				finish_polyline();
	        }

	        ++ i_vline;
	    }
    }

    if (polyline != nullptr) {
        // Finish the current vertical line,
        const MonotonicRegion            &region = *path.back().region;
        const SegmentedIntersectionLine  &vline  = segs[region.right.vline];
        const SegmentIntersection        *ip     = &vline.intersections[region.right_intersection_point(path.back().flipped)];
        assert(ip->is_inner());
        ip->is_low() ? -- ip : ++ ip;
        assert(ip->is_outer());
        polyline->points.back() = Point(vline.pos, ip->pos());
        finish_polyline();
    }
}

// The extended bounding box of the whole object that covers any rotation of every layer.
BoundingBox FillRectilinear::extended_object_bounding_box() const {
    BoundingBox out = this->bounding_box;
    out.merge(Point(out.min.y(), out.min.x()));
    out.merge(Point(out.max.y(), out.max.x()));

    // The bounding box is scaled by sqrt(2.) to ensure that the bounding box
    // covers any possible rotations.
    return out.scaled(sqrt(2.));
}

bool FillRectilinear::fill_surface_by_lines(const Surface *surface, const FillParams &params, float angleBase, float pattern_shift, Polylines &polylines_out)
{
    // At the end, only the new polylines will be rotated back.
    size_t n_polylines_out_initial = polylines_out.size();

    // Shrink the input polygon a bit first to not push the infill lines out of the perimeters.
//    const float INFILL_OVERLAP_OVER_SPACING = 0.3f;
    const float INFILL_OVERLAP_OVER_SPACING = 0.45f;
    assert(INFILL_OVERLAP_OVER_SPACING > 0 && INFILL_OVERLAP_OVER_SPACING < 0.5f);

    // Rotate polygons so that we can work with vertical lines here
    std::pair<float, Point> rotate_vector = this->_infill_direction(surface);
    if (params.locked_zag)
        rotate_vector.first += float(M_PI/2.);
    rotate_vector.first += angleBase;

    assert(params.density > 0.0001f && params.density <= 1.f);
    coord_t line_spacing = coord_t(scale_(this->spacing) / params.density);

    // On the polygons of poly_with_offset, the infill lines will be connected.
    ExPolygonWithOffset poly_with_offset(
        surface->expolygon, 
        - rotate_vector.first, 
        float(scale_(this->overlap - (0.5 - INFILL_OVERLAP_OVER_SPACING) * this->spacing)),
        float(scale_(this->overlap - 0.5f * this->spacing)));
    if (poly_with_offset.n_contours_inner == 0) {
        // Not a single infill line fits.
        //FIXME maybe one shall trigger the gap fill here?
        return true;
    }

    // For infill that needs to be consistent between layers (like Zig Zag),
    // we use bounding box of whole object to match vertical lines between layers.
    BoundingBox bounding_box_src = poly_with_offset.bounding_box_src();
    BoundingBox bounding_box     = this->has_consistent_pattern() ? this->extended_object_bounding_box() : bounding_box_src;

    // define flow spacing according to requested density
    if (params.full_infill() && !params.dont_adjust) {
        line_spacing = this->_adjust_solid_spacing(bounding_box_src.size().x(), line_spacing);
        this->spacing = unscale<double>(line_spacing);
    } else {
        // extend bounding box so that our pattern will be aligned with other layers
        // Transform the reference point to the rotated coordinate system.
        Point refpt = rotate_vector.second.rotated(- rotate_vector.first);
        // align_to_grid will not work correctly with positive pattern_shift.
        coord_t pattern_shift_scaled = coord_t(scale_(pattern_shift)) % line_spacing;
        refpt.x() -= (pattern_shift_scaled >= 0) ? pattern_shift_scaled : (line_spacing + pattern_shift_scaled);
        bounding_box.merge(align_to_grid(
            bounding_box.min, 
            Point(line_spacing, line_spacing), 
            refpt));
    }

    // Intersect a set of euqally spaced vertical lines wiht expolygon.
    // n_vlines = ceil(bbox_width / line_spacing)
    size_t  n_vlines = (bounding_box.max(0) - bounding_box.min(0) + line_spacing - 1) / line_spacing;
	coord_t x0 = bounding_box.min(0);
	if (params.full_infill())
		x0 += (line_spacing + coord_t(SCALED_EPSILON)) / 2;

    int gap_line = params.horiz_move / line_spacing;
    if (gap_line % 2 == 0) {
        x0 += params.horiz_move - gap_line * line_spacing;
    } else {
        x0 += params.horiz_move - (gap_line - 1) * line_spacing;
        n_vlines += 1;
    }
#ifdef SLIC3R_DEBUG
    static int iRun = 0;
    BoundingBox bbox_svg = poly_with_offset.bounding_box_outer();
    ::Slic3r::SVG svg(debug_out_path("FillRectilinear-%d.svg", iRun), bbox_svg); // , scale_(1.));
    poly_with_offset.export_to_svg(svg);
    {
        ::Slic3r::SVG svg(debug_out_path("FillRectilinear-initial-%d.svg", iRun), bbox_svg); // , scale_(1.));
        poly_with_offset.export_to_svg(svg);
    }
    iRun ++;
#endif /* SLIC3R_DEBUG */
    std::vector<SegmentedIntersectionLine> segs = slice_region_by_vertical_lines(poly_with_offset, n_vlines, x0, line_spacing);
    // Connect by horizontal / vertical links, classify the links based on link_max_length as too long.
	connect_segment_intersections_by_contours(poly_with_offset, segs, params, link_max_length);

#ifdef SLIC3R_DEBUG
    // Paint the segments and finalize the SVG file.
    for (size_t i_seg = 0; i_seg < segs.size(); ++ i_seg) {
        SegmentedIntersectionLine &sil = segs[i_seg];
        for (size_t i = 0; i < sil.intersections.size();) {
            size_t j = i + 1;
            for (; j < sil.intersections.size() && sil.intersections[j].is_inner(); ++ j) ;
            if (i + 1 == j) {
                svg.draw(Line(Point(sil.pos, sil.intersections[i].pos()), Point(sil.pos, sil.intersections[j].pos())), "blue");
            } else {
                svg.draw(Line(Point(sil.pos, sil.intersections[i].pos()), Point(sil.pos, sil.intersections[i+1].pos())), "green");
                svg.draw(Line(Point(sil.pos, sil.intersections[i+1].pos()), Point(sil.pos, sil.intersections[j-1].pos())), (j - i + 1 > 4) ? "yellow" : "magenta");
                svg.draw(Line(Point(sil.pos, sil.intersections[j-1].pos()), Point(sil.pos, sil.intersections[j].pos())), "green");
            }
            i = j + 1;
        }
    }
    svg.Close();
#endif /* SLIC3R_DEBUG */

    //FIXME this is a hack to get the monotonic infill rolling. We likely want a smarter switch, likely based on user decison.
    bool monotonic_infill = params.monotonic; // || params.density > 0.99;
    if (monotonic_infill) {
        // Sometimes the outer contour pinches the inner contour from both sides along a single vertical line.
        // This situation is not handled correctly by generate_montonous_regions().
        // Insert phony OUTER_HIGH / OUTER_LOW pairs at the position where the contour is pinched.
        pinch_contours_insert_phony_outer_intersections(segs);
		std::vector<MonotonicRegion> regions = generate_montonous_regions(segs);
#ifdef INFILL_DEBUG_OUTPUT
        {
            static int iRun;
            export_monotonous_regions_to_svg(poly_with_offset, segs, regions, debug_out_path("%s-%03d.svg", "MontonousRegions-initial", iRun ++));
        }
#endif // INFILL_DEBUG_OUTPUT
		connect_monotonic_regions(regions, poly_with_offset, segs);
        if (! regions.empty()) {
		    std::mt19937_64 rng;
		    std::vector<MonotonicRegionLink> path = chain_monotonic_regions(regions, poly_with_offset, segs, rng);
		    polylines_from_paths(path, poly_with_offset, segs, polylines_out);
        }
	} else {
		traverse_graph_generate_polylines(poly_with_offset, params, segs, this->has_consistent_pattern(), polylines_out);
    }

#ifdef SLIC3R_DEBUG
    {
        {
            ::Slic3r::SVG svg(debug_out_path("FillRectilinear-final-%03d.svg", iRun), bbox_svg); // , scale_(1.));
            poly_with_offset.export_to_svg(svg);
            for (size_t i = n_polylines_out_initial; i < polylines_out.size(); ++ i)
                svg.draw(polylines_out[i].lines(), "black");
        }
        // Paint a picture per polyline. This makes it easier to discover the order of the polylines and their overlap.
        for (size_t i_polyline = n_polylines_out_initial; i_polyline < polylines_out.size(); ++ i_polyline) {
            ::Slic3r::SVG svg(debug_out_path("FillRectilinear-final-%03d-%03d.svg", iRun, i_polyline), bbox_svg); // , scale_(1.));
            svg.draw(polylines_out[i_polyline].lines(), "black");
        }
    }
#endif /* SLIC3R_DEBUG */

    // paths must be rotated back
    for (Polylines::iterator it = polylines_out.begin() + n_polylines_out_initial; it != polylines_out.end(); ++ it) {
        // No need to translate, the absolute position is irrelevant.
        // it->translate(- rotate_vector.second(0), - rotate_vector.second(1));
        assert(! it->has_duplicate_points());
        it->rotate(rotate_vector.first);
        //FIXME rather simplify the paths to avoid very short edges?
        //assert(! it->has_duplicate_points());
        it->remove_duplicate_points();

         //get origin direction infill
        if (params.symmetric_infill_y_axis) {
            it->symmetric_y(params.symmetric_y_axis);
        }
    }

#ifdef SLIC3R_DEBUG
    // Verify, that there are no duplicate points in the sequence.
    for (Polyline &polyline : polylines_out)
        assert(! polyline.has_duplicate_points());
#endif /* SLIC3R_DEBUG */



    return true;
}

void make_fill_lines(const ExPolygonWithOffset &poly_with_offset, Point refpt, double angle, coord_t x_margin, coord_t line_spacing, coord_t pattern_shift, Polylines &fill_lines)
{
    BoundingBox bounding_box = poly_with_offset.bounding_box_src();
    // Don't produce infill lines, which fully overlap with the infill perimeter.
    coord_t     x_min = bounding_box.min.x() + x_margin;
    coord_t     x_max = bounding_box.max.x() - x_margin;
    // extend bounding box so that our pattern will be aligned with other layers
    // align_to_grid will not work correctly with positive pattern_shift.
    coord_t pattern_shift_scaled = pattern_shift % line_spacing;
    refpt.x() -= (pattern_shift_scaled >= 0) ? pattern_shift_scaled : (line_spacing + pattern_shift_scaled);
    bounding_box.merge(Slic3r::align_to_grid(bounding_box.min, Point(line_spacing, line_spacing), refpt));

    // Intersect a set of euqally spaced vertical lines wiht expolygon.
    // n_vlines = ceil(bbox_width / line_spacing)
    const size_t n_vlines = (bounding_box.max.x() - bounding_box.min.x() + line_spacing - 1) / line_spacing;
    const double cos_a    = cos(angle);
    const double sin_a    = sin(angle);
    for (const SegmentedIntersectionLine &vline : slice_region_by_vertical_lines(poly_with_offset, n_vlines, bounding_box.min.x(), line_spacing))
        if (vline.pos >= x_min) {
            if (vline.pos > x_max)
                break;
            for (auto it = vline.intersections.begin(); it != vline.intersections.end();) {
                auto it_low  = it ++;
                assert(it_low->type == SegmentIntersection::OUTER_LOW);
                if (it_low->type != SegmentIntersection::OUTER_LOW)
                    continue;
                auto it_high = it;
                assert(it_high->type == SegmentIntersection::OUTER_HIGH);
                if (it_high->type == SegmentIntersection::OUTER_HIGH) {
                    if (angle == 0.)
                        fill_lines.emplace_back(Point(vline.pos, it_low->pos()), Point(vline.pos, it_high->pos()));
                    else
                        fill_lines.emplace_back(Point(vline.pos, it_low->pos()).rotated(cos_a, sin_a), Point(vline.pos, it_high->pos()).rotated(cos_a, sin_a));
                    ++ it;
                }
            }
        }
}

     // Remove lines that are too close to each other.
static inline void remove_overlapped(Polylines& polylines, coord_t line_width){
    const coord_t tolerance = coord_t(0.75 * line_width);
    Polylines cleaned;
    cleaned.reserve(polylines.size());

    auto midpoint = [](const Polyline& line) -> Point {
        const Point& p1 = line.first_point();
        const Point& p2 = line.last_point();
        return Point((p1.x() + p2.x()) / 2, (p1.y() + p2.y()) / 2);
    };

    for (const Polyline& line : polylines) {
        Point mp1 = midpoint(line);
        bool overlapped = false;

        for (const Polyline& existing : cleaned) {
            Point mp2 = midpoint(existing);

            // Early skip: if they're far apart on one axis, skip
            if (std::abs(mp1.y() - mp2.y()) > tolerance &&
                std::abs(mp1.x() - mp2.x()) > tolerance)
                continue;

            if (mp1.distance_to(mp2) < tolerance) {
                overlapped = true;
                break;
            }
        }

        if (!overlapped)
            cleaned.push_back(line);
    }

    polylines = std::move(cleaned);
}

bool FillRectilinear::fill_surface_by_multilines(const Surface *surface, FillParams params, const std::initializer_list<SweepParams> &sweep_params, Polylines &polylines_out)
{
    assert(sweep_params.size() >= 1);
    assert(!params.full_infill());
    params.density /= double(sweep_params.size());
    assert(params.density > 0.0001f && params.density <= 1.f);

    ExPolygonWithOffset poly_with_offset_base(surface->expolygon, 0, float(scale_(this->overlap + 0.5 * params.multiline * this->spacing)));//increase offset to crop infill lines when using multiline infill
    if (poly_with_offset_base.n_contours == 0)
        // Not a single infill line fits.
        return true;

    Polylines fill_lines;
    coord_t line_width   = coord_t(scale_(this->spacing));
    coord_t line_spacing = coord_t(scale_(this->spacing) * params.multiline / params.density);
    std::pair<float, Point> rotate_vector = this->_infill_direction(surface);
    for (const SweepParams &sweep : sweep_params) {
        // Rotate polygons so that we can work with vertical lines here
        float angle = rotate_vector.first + sweep.angle_base;

        make_fill_lines(ExPolygonWithOffset(poly_with_offset_base, -angle), rotate_vector.second.rotated(-angle), angle,
                        line_width + coord_t(SCALED_EPSILON), line_spacing, coord_t(scale_(sweep.pattern_shift)), fill_lines);
    }
        
    // Apply multiline offset if needed
    multiline_fill(fill_lines, params, spacing);
 
    // Contract surface polygon by half line width to avoid excesive overlap with perimeter
    ExPolygons contracted = offset_ex(surface->expolygon, -float(scale_(0.5 * this->spacing)));
    
    // if contraction results in empty polygon, use original surface
    const ExPolygon &intersection_surface = contracted.empty() ? surface->expolygon : contracted.front();

    // Intersect polylines with perimeter
    fill_lines = intersection_pl(std::move(fill_lines), intersection_surface);

    if ((params.pattern == ipLateralLattice || params.pattern == ipLateralHoneycomb ) && params.multiline >1 )
    remove_overlapped(fill_lines, line_width);

    if (!fill_lines.empty()) {
        if (params.dont_connect()) { 
            if (fill_lines.size() > 1)
                fill_lines = chain_polylines(std::move(fill_lines));
            append(polylines_out, std::move(fill_lines));
        } else
             connect_infill(std::move(fill_lines), intersection_surface, polylines_out, this->spacing, params);
    }

    return true;
}

bool FillRectilinear::fill_surface_trapezoidal(
    const Surface*                            surface,
    FillParams                                params,
    const std::initializer_list<SweepParams>& sweep_params,
    Polylines&                                polylines_out,
    int                                       Pattern_type) // 0=grid, 1=triangular
{
    assert(params.multiline > 1);

    Polylines polylines;

    // Common parameters
    const coord_t d1 = coord_t(scale_(this->spacing)) * params.multiline; // Infill total wall thickness

    // Pattern-specific parameters
    coord_t                  period;
    double                   base_angle;
    std::pair<double, Point> rotate_vector = this->_infill_direction(surface);

    if (Pattern_type == 0) {
        // Grid pattern parameters
        period     = coord_t((2.0 * d1 / params.density) * std::sqrt(2.0));
        base_angle = rotate_vector.first + M_PI_4; // 45
    } else {
        // Triangular pattern parameters
        period     = coord_t(( 2.0 * d1 / params.density) * std::sqrt(3.0));
        base_angle = rotate_vector.first + M_PI_2; //90
    }

    // Obtain the expolygon and rotate to align with pattern base angle
    ExPolygon expolygon = surface->expolygon;
    if (std::abs(base_angle) >= EPSILON) {
        expolygon.rotate(-base_angle, rotate_vector.second);
    }

    // Use extended object bounding box for consistent pattern across layers
    BoundingBox bb = this->extended_object_bounding_box();

    switch (Pattern_type) {
    case 0: // Grid / Trapezoidal
    {
        // Generate a non-crossing trapezoidal pattern to avoid overextrusion at intersections when `multiline > 1`.
        //      P1--P2
        //     /      \
        //  P0/        \P3__P4
        //
        // P1x-P2x=P3x-P4x=d1
        // P0y-P1y=P2y-P3y=d2
        
        const coord_t d2 = coord_t(0.5 * period - d1);

        //  Align bounding box to the grid
        bb.merge(align_to_grid(bb.min, Point(period, period)));
        const coord_t xmin = bb.min.x();
        const coord_t xmax = bb.max.x();
        const coord_t ymin = bb.min.y();
        const coord_t ymax = bb.max.y();

        // Create the two base row patterns once
        Polyline base_row_normal;
        base_row_normal.points.reserve(((xmax - xmin) / period + 1) * 5); // 5 points per trapezoid
        Polyline base_row_flipped;
        base_row_flipped.points.reserve(((xmax - xmin) / period + 1) * 5); // 5 points per trapezoid

        // Build complete rows from xmin to xmax
        for (coord_t x = xmin; x < xmax; x += period) {
            // Normal row
            base_row_normal.points.emplace_back(Point(x, d1 / 2));                    // P0
            base_row_normal.points.emplace_back(Point(x + d1, d1 / 2));               // P1
            base_row_normal.points.emplace_back(Point(x + d1 + d2, d1 / 2 + d2));     // P2
            base_row_normal.points.emplace_back(Point(x + 2 * d1 + d2, d1 / 2 + d2)); // P3
            base_row_normal.points.emplace_back(Point(x + 2 * d1 + 2 * d2, d1 / 2));  // P4
        }

        // Flipped row (mirrored vertically)
        base_row_flipped.points = base_row_normal.points;
        for (auto& p : base_row_flipped.points) {
            p.y() = period / 2 - p.y();
        }
        
        // Pre-allocate polylines
        const size_t estimated_rows = ((ymax - ymin) / (period / 2) + 1);
        polylines.reserve(estimated_rows);

        bool flip_vertical = false;

        // Now just copy and translate vertically
        for (coord_t y = ymin; y < ymax; y += period / 2) {
            Polyline pl_row = flip_vertical ? base_row_flipped : base_row_normal;

            // Translate all points vertically
            for (Point& p : pl_row.points) {
                p.y() += y;
            }

            polylines.emplace_back(std::move(pl_row));
            flip_vertical = !flip_vertical;
        }

        // transpose points for odd layers
        if (layer_id % 2 == 1) {
            for (Polyline& pl : polylines) {
                for (Point& p : pl.points) {
                    std::swap(p.x(), p.y());
                    p.x() += d1 / 2;
                    p.y() -= d1 / 2;
                }
            }
        }
        break;
    }

    case 1: // Triangular
    {
        // Generate a non-crossing trapezoidal pattern with a base line below.
        //      P1-P2
        //     /     \
        //  P0/       \P3_P4
        //  ----------------
        // P1x-P2x=P3x-P4x=d2
        // P0y-P1y=P2y-P3y=h-2d1
        //
        
        // Triangular pattern density adjustment:
        const coord_t d2_tri = coord_t(2.0 / std::sqrt(3.0) * d1);
        const coord_t h      = coord_t(0.5 * std::sqrt(3.0) * period); // height of triangle

        //  Align bounding box to the grid
        bb.merge(align_to_grid(bb.center(), Point(period,h)));
        const int    layer_mod = layer_id % 3;
        const double angle     = layer_mod * 2.0 * M_PI / 3.0;

        const Point rotation_center = bb.center();
        const coord_t half_w = bb.size().x() / 2;
        const coord_t half_h = bb.size().y() / 2;
        
        // Compute how many full periods fit in each direction
        const coord_t num_periods_x = coord_t(std::ceil(half_w / double(period)));
        coord_t num_periods_y =coord_t(std::ceil(half_h / double(h)));        
        // Ensure an even number of rows so the pattern stays centered
        if ((num_periods_y % 2) != 0)
            ++num_periods_y;
        
        // Compute aligned limits (symmetric around the origin)
        const coord_t x_min_aligned = -num_periods_x * period;
        const coord_t x_max_aligned =  num_periods_x * period;   
        const coord_t y_min_aligned = -num_periods_y * h;
        const coord_t y_max_aligned =  num_periods_y * h;

        // Pre-allocate estimated number of polylines
        const size_t estimated_rows = (y_max_aligned - y_min_aligned) / h + 2;
        const size_t estimated_polylines = (estimated_rows + 1) * 2; // base line + trapezoid line per row
        polylines.reserve(estimated_polylines);

        // Create the two base row templates once
        Polyline base_line_template;
        base_line_template.points.reserve(2); // 2 points for base line
        Polyline trapezoid_row_normal;
        trapezoid_row_normal.points.reserve(((x_max_aligned - x_min_aligned) / period + 1) * 5); // 5 points per trapezoid
        Polyline trapezoid_row_shifted;
        trapezoid_row_shifted.points.reserve(((x_max_aligned - x_min_aligned) / period + 1) * 5); // 5 points per trapezoid
        // Build base line template (from x_min_aligned to x_max_aligned)
        base_line_template.points.emplace_back(Point(x_min_aligned, 0));
        base_line_template.points.emplace_back(Point(x_max_aligned, 0));

        // Build complete trapezoid rows once
        // Normal row (no shift)
        for (coord_t x = x_min_aligned; x < x_max_aligned; x += period) {
            trapezoid_row_normal.points.emplace_back(Point(x + d2_tri / 2, d1));                  // P0
            trapezoid_row_normal.points.emplace_back(Point(x + period / 2 - d2_tri / 2, h - d1)); // P1
            trapezoid_row_normal.points.emplace_back(Point(x + period / 2 + d2_tri / 2, h - d1)); // P2
            trapezoid_row_normal.points.emplace_back(Point(x + period - d2_tri / 2, d1));         // P3
            trapezoid_row_normal.points.emplace_back(Point(x + period, d1));                      // P4
        }

        // Shifted row (mirrored vertically)
        trapezoid_row_shifted.points = trapezoid_row_normal.points;
        for (auto& p : trapezoid_row_shifted.points)
            p.y() = h - p.y();

        bool shift_row = false;

        // Generate pattern by copying and translating templates vertically
        for (coord_t y = y_min_aligned; y < y_max_aligned; y += h) {
            // Base line - copy and translate
            Polyline base_line = base_line_template;
            for (Point& p : base_line.points) {
                p.y() += y;
            }
            polylines.emplace_back(std::move(base_line));

            // Trapezoid line - copy and translate the appropriate template
            Polyline trapezoid_line = shift_row ? trapezoid_row_shifted : trapezoid_row_normal;
            for (Point& p : trapezoid_line.points) {
                p.y() += y;
            }

            if (!trapezoid_line.points.empty()) {
                polylines.emplace_back(std::move(trapezoid_line));
            }

            shift_row = !shift_row;
        }

        //  Rotate around origin (0,0)
        if (layer_mod)
            for (auto& pl : polylines)
                pl.rotate(angle, Point(0,0));

        break;
    }

    default:
        // Handle unknown pattern type
        break;
    }

    // Apply multiline fill
    multiline_fill(polylines, params, spacing);

    // Contract surface polygon by half line width to avoid excesive overlap with perimeter
    ExPolygons contracted = offset_ex(expolygon, -float(scale_(0.5 * this->spacing)));

    // if contraction results in empty polygon, use original surface
    const ExPolygon &intersection_surface = contracted.empty() ? expolygon : contracted.front();

    // Intersect polylines with offset expolygon
    polylines = intersection_pl(std::move(polylines), intersection_surface);

    // Remove very short segments that may cause connection issues
    const double minlength = scale_(0.8 * this->spacing);
    if (minlength > 0 && !polylines.empty()) {
        polylines.erase(std::remove_if(polylines.begin(), polylines.end(),
                                       [minlength](const Polyline& pl) { return pl.length() < minlength; }),
                        polylines.end());
    }

    // Connect infill lines using offset expolygon
    int infill_start_idx = polylines_out.size();
    if (!polylines.empty()) {
        Slic3r::Fill::chain_or_connect_infill(std::move(polylines), intersection_surface, polylines_out, this->spacing, params);

        // Rotate back the infill lines to original orientation
        if (std::abs(base_angle) >= EPSILON) {
            for (auto it = polylines_out.begin() + infill_start_idx; it != polylines_out.end(); ++it) {
                it->rotate(base_angle, rotate_vector.second);
            }
        }
    }

    return true;
}

Polylines FillRectilinear::fill_surface(const Surface *surface, const FillParams &params)
{
    Polylines polylines_out;
    // Orca Todo: fow now don't use fill_surface_by_multilines for zipzag infill
    if (params.full_infill() || params.multiline == 1 || params.pattern == ipCrossZag || params.pattern == ipZigZag || params.pattern == ipLockedZag) {
        if (!fill_surface_by_lines(surface, params, 0.f, 0.f, polylines_out))
            BOOST_LOG_TRIVIAL(error) << "FillRectilinear::fill_surface() fill_surface_by_lines() failed to fill a region.";
    } else {
        if (!fill_surface_by_multilines(surface, params, {{0.f, 0.f}}, polylines_out))
            BOOST_LOG_TRIVIAL(error) << "FillRectilinear::fill_surface() fill_surface_by_multilines() failed to fill a region.";
    }
    return polylines_out;
}

Polylines FillMonotonic::fill_surface(const Surface *surface, const FillParams &params)
{
    FillParams params2 = params;
    params2.monotonic = true;
    Polylines polylines_out;
    if (! fill_surface_by_lines(surface, params2, 0.f, 0.f, polylines_out))
        BOOST_LOG_TRIVIAL(error) << "FillMonotonous::fill_surface() failed to fill a region.";
    return polylines_out;
}

Polylines FillMonotonicLine::fill_surface(const Surface* surface, const FillParams& params)
{
    FillParams params2 = params;
    params2.monotonic = true;
    params2.anchor_length_max = 0.0f;
    Polylines polylines_out;
    if (! fill_surface_by_lines(surface, params2, 0.f, 0.f, polylines_out))
        BOOST_LOG_TRIVIAL(error) << "Failed to fill a region by MonotonicLine pattern";
    return polylines_out;
}

Polylines FillGrid::fill_surface(const Surface *surface, const FillParams &params)
{
    Polylines polylines_out;

    if (params.multiline > 1) {
        // Experimental trapezoidal grid
        if (!this->fill_surface_trapezoidal(
                 surface, params,
                 { { 0.f, 0.f }, { float(M_PI / 2.), 0.f } },
                polylines_out,0))
            BOOST_LOG_TRIVIAL(error) << "FillGrid::fill_surface_trapezoidal() failed.";

    } else {
        if (!this->fill_surface_by_multilines(
                surface, params,
                { { 0.f, 0.f }, { float(M_PI / 2.), 0.f } },
                polylines_out))
            BOOST_LOG_TRIVIAL(error) << "FillGrid::fill_surface() failed to fill a region.";
    

       if (this->layer_id % 2 == 1)
           for (int i = 0; i < polylines_out.size(); i++)
               std::reverse(polylines_out[i].begin(), polylines_out[i].end());
    }
    return polylines_out;
}

Polylines FillLateralLattice::fill_surface(const Surface *surface, const FillParams &params)
{
    Polylines polylines_out;
    coordf_t dx1 = tan(Geometry::deg2rad(params.lateral_lattice_angle_1)) * z;
    coordf_t dx2 = tan(Geometry::deg2rad(params.lateral_lattice_angle_2)) * z;
    if (! this->fill_surface_by_multilines(
            surface, params,
            { { float(M_PI / 2.), float(dx1) }, { float(M_PI / 2.), float(dx2) } },
            polylines_out))
        BOOST_LOG_TRIVIAL(error) << "FillLateralLattice::fill_surface() failed to fill a region.";

    if (this->layer_id % 2 == 1)
        for (int i = 0; i < polylines_out.size(); i++)
            std::reverse(polylines_out[i].begin(), polylines_out[i].end());
    return polylines_out;
}

Polylines FillTriangles::fill_surface(const Surface *surface, const FillParams &params){
    Polylines polylines_out;
        if (params.multiline > 1) {
        // Experimental trapezoidal grid
        if (!this->fill_surface_trapezoidal(
                 surface, params,
                 { { 0.f, 0.f }, { float(M_PI / 2.), 0.f } },
                polylines_out,1))
            BOOST_LOG_TRIVIAL(error) << "FillGrid::fill_surface_trapezoidal() failed.";

    } else {
    if (! this->fill_surface_by_multilines(
            surface, params,
            { { 0.f, 0.f }, { float(M_PI / 3.), 0.f }, { float(2. * M_PI / 3.), 0. } },
            polylines_out))
        BOOST_LOG_TRIVIAL(error) << "FillTriangles::fill_surface() failed to fill a region.";
    }
    return polylines_out;
    
}

Polylines FillStars::fill_surface(const Surface *surface, const FillParams &params)
{
    Polylines polylines_out;
    if (! this->fill_surface_by_multilines(
            surface, params,
            { { 0.f, 0.f }, { float(M_PI / 3.), 0.f }, { float(2. * M_PI / 3.), float((3./2.) * this->spacing * params.multiline / params.density) } },
            polylines_out))
        BOOST_LOG_TRIVIAL(error) << "FillStars::fill_surface() failed to fill a region.";
    return polylines_out;
}

Polylines FillCubic::fill_surface(const Surface *surface, const FillParams &params)
{
    Polylines polylines_out;
    coordf_t dx = sqrt(0.5) * z;
    if (! this->fill_surface_by_multilines(
            surface, params, 
            { { 0.f, float(dx) }, { float(M_PI / 3.), - float(dx) }, { float(M_PI * 2. / 3.), float(dx) } },
            polylines_out))
        BOOST_LOG_TRIVIAL(error) << "FillCubic::fill_surface() failed to fill a region.";
    return polylines_out; 
}

Polylines FillQuarterCubic::fill_surface(const Surface* surface, const FillParams& params)
{
    using namespace boost::math::float_constants;
    Polylines polylines_out;

    coord_t line_width = coord_t(scale_(this->spacing)) * params.multiline;
    coord_t period = coord_t(scale_(this->spacing) *params.multiline / params.density) * 4;

    // First half tetrahedral fill
    double  pattern_z_shift = 0.0;
    coord_t shift = coord_t(one_div_root_two * (scale_(z) + pattern_z_shift * period * 2)) % period;
    shift = std::min(shift, period - shift); // symmetry due to the fact that we are applying the shift in both directions
    shift = std::min(shift, period / 2 - line_width / 2); // don't put lines too close to each other
    shift = std::max(shift, line_width / 2);              // don't put lines too close to each other
    float dx1 = unscale_(shift);

    // Second half tetrahedral fill
    pattern_z_shift = 0.5;
    shift = coord_t(one_div_root_two * (scale_(z) + pattern_z_shift * period * 2)) % period;
    shift = std::min(shift, period - shift); // symmetry due to the fact that we are applying the shift in both directions
    shift = std::min(shift, period / 2 - line_width / 2); // don't put lines too close to each other
    shift = std::max(shift, line_width / 2);              // don't put lines too close to each other
    float dx2 = unscale_(shift);
    if (!this->fill_surface_by_multilines(
            surface, params, 
            {{0.f, dx1}, {0.f, -dx1}, {float(M_PI / 2.), dx2}, {float(M_PI / 2.), -dx2}},
            polylines_out))
        BOOST_LOG_TRIVIAL(error) << "FillQuarterCubic::fill_surface() failed to fill a region.";

    return polylines_out;
}

Polylines FillLateralHoneycomb::fill_surface(const Surface *surface, const FillParams &params)
{
    // the lateral honeycomb is generated based on a base pattern of an inverted Y with its junction at height zero
    //     |
    //     |
    // 0 --+--
    //    / \
    // why inverted?
    // it makes determining some of the properties easier
    // and the two angled legs provide additional horizontal stiffness
    // the additional horizontal stiffness is not required close to the bed (unless you don't have any kind of bottom or flange)

    using namespace boost::math::float_constants;

    // lets begin calculating some base properties of the honeycomb pattern
    const float half_horizontal_period = .5f * (1*(2/3.f) + 2*(1/3.f)) * float(spacing) * params.multiline / params.density;
    const float vertical_period = 3 * half_horizontal_period / tanf(degree * float(params.infill_overhang_angle));

    // we want to align the base pattern with its knot on height 0
    // therefore the double line part is 1/3 below and the single line is 2/3 above 0
    const float vertical_thirds_float = 3 * float(z) / vertical_period;
    const int vertical_thirds_int = vertical_thirds_float; // converstion to int does implicit floor wich is desired here
    const bool single_line = (vertical_thirds_int + 1) % 3;

    // the base pattern needs to be horizontally shifted by half every odd pattern layer
    const bool odd_layer = ((vertical_thirds_int + 1) / 3) % 2;
    const float horizontal_offset = odd_layer ? half_horizontal_period : 0;

    Polylines polylines_out;

    if (single_line)
    {
        FillParams multiline_params = params;
        multiline_params.density *= 1 / (1*(2/3.) + 2*(1/3.));

        if (!fill_surface_by_multilines(
                surface, multiline_params,
                { { half_pi, horizontal_offset } },
                polylines_out))
            BOOST_LOG_TRIVIAL(error) << "FillLateralHoneycomb::fill_surface() failed to fill a region.";
    } else {
        FillParams multiline_params = params;
        multiline_params.density *= 2 / (1*(2/3.) + 2*(1/3.));

        const float horizontal_position = (1 - (vertical_thirds_float - vertical_thirds_int)) * half_horizontal_period;

        if (!fill_surface_by_multilines(
                surface, multiline_params,
                { { half_pi, -horizontal_position + horizontal_offset }, { half_pi, horizontal_position + horizontal_offset } },
                polylines_out))
            BOOST_LOG_TRIVIAL(error) << "FillLateralHoneycomb::fill_surface() failed to fill a region.";
    }

    if (this->layer_id % 2 == 1)
        for (int i = 0; i < polylines_out.size(); i++)
            std::reverse(polylines_out[i].begin(), polylines_out[i].end());

    return polylines_out;
}

Polylines FillSupportBase::fill_surface(const Surface *surface, const FillParams &params)
{
    assert(! params.full_infill());

    Polylines polylines_out;
    std::pair<float, Point> rotate_vector = this->_infill_direction(surface);
    ExPolygonWithOffset poly_with_offset(surface->expolygon, - rotate_vector.first, float(scale_(this->overlap - 0.5 * this->spacing)));
    if (poly_with_offset.n_contours > 0) {
        Polylines fill_lines;
        coord_t line_spacing = coord_t(scale_(this->spacing) / params.density);
        // Create infill lines, keep them vertical.
        make_fill_lines(poly_with_offset, rotate_vector.second.rotated(- rotate_vector.first), 0, 0, line_spacing, 0, fill_lines);
        // Both the poly_with_offset and polylines_out are rotated, so the infill lines are strictly vertical.
        connect_base_support(std::move(fill_lines), poly_with_offset.polygons_outer, poly_with_offset.bounding_box_outer(), polylines_out, this->spacing, params);
        // Rotate back by rotate_vector.first
        const double cos_a = cos(rotate_vector.first);
        const double sin_a = sin(rotate_vector.first);
        for (Polyline &pl : polylines_out)
            for (Point &pt : pl.points)
                pt.rotate(cos_a, sin_a);
    }
    return polylines_out;
}

// Lightning infill assumes that the distance between any two sampled points is always
// at least equal to the value of spacing. To meet this assumption, we need to use
// BoundingBox for whole layers instead of bounding box just around processing ExPolygon.
// Using just BoundingBox around processing ExPolygon could produce two points closer
// than spacing (in cases where two ExPolygon are closer than spacing).
Points sample_grid_pattern(const ExPolygon& expolygon, coord_t spacing, const BoundingBox& global_bounding_box)
{
    ExPolygonWithOffset poly_with_offset(expolygon, 0, 0, 0);
    std::vector<SegmentedIntersectionLine> segs = slice_region_by_vertical_lines(
        poly_with_offset, 
        (global_bounding_box.max.x() - global_bounding_box.min.x() + spacing - 1) / spacing,
        global_bounding_box.min.x(),
        spacing);

    Points out;
    for (const SegmentedIntersectionLine &sil : segs) {
        for (size_t i = 0; i < sil.intersections.size(); i += 2) {
            coord_t a = sil.intersections[i].pos();
            coord_t b = sil.intersections[i + 1].pos();
            for (coord_t y = a - (a % spacing) - spacing; y < b; y += spacing)
                if (y > a)
                    out.emplace_back(sil.pos, y);
        }
    }
    return out;
}

Points sample_grid_pattern(const ExPolygons& expolygons, coord_t spacing, const BoundingBox& global_bounding_box)
{
    Points out;
    for (const ExPolygon& expoly : expolygons)
        append(out, sample_grid_pattern(expoly, spacing, global_bounding_box));
    return out;
}

Points sample_grid_pattern(const Polygons& polygons, coord_t spacing, const BoundingBox& global_bounding_box)
{
    return sample_grid_pattern(union_ex(polygons), spacing, global_bounding_box);
}

// Orca: Introduced FillMonotonicLines from Prusa slicer, inhereting from FillRectilinear
// This replaces the FillMonotonicLineWGapFill from BBS
Polylines FillMonotonicLines::fill_surface(const Surface *surface, const FillParams &params)
{
    FillParams params2 = params;
    params2.monotonic = true;
    params2.anchor_length_max = 0.0f;
    Polylines polylines_out;
    if (! fill_surface_by_lines(surface, params2, 0.f, 0.f, polylines_out))
        BOOST_LOG_TRIVIAL(error) << "FillMonotonicLines::fill_surface() failed to fill a region.";
    return polylines_out;
}
    
// Orca: Replaced with FillMonotonicLines from Prusa slicer. Moved gap fill algorithm to
// FillBase to perform gap fill for all fill types.
/*void FillMonotonicLineWGapFill::fill_surface_extrusion(const Surface* surface, const FillParams& params, ExtrusionEntitiesPtr& out)
{
    ExtrusionEntityCollection *coll_nosort = new ExtrusionEntityCollection();
    coll_nosort->no_sort = this->no_sort();

    Polylines polylines_rectilinear;
    Surface rectilinear_surface{ *surface };
    FillParams params2 = params;
    params2.monotonic = true;
    params2.anchor_length_max = 0.0f;
    //BBS: always don't adjust the spacing of top surface infill
    params2.dont_adjust = true;

    //BBS: always use no overlap expolygons to avoid overflow in top surface
    //for (const ExPolygon &rectilinear_area : this->no_overlap_expolygons) {
    //    rectilinear_surface.expolygon = rectilinear_area;
    //    fill_surface_by_lines(&rectilinear_surface, params2, polylines_rectilinear);
    //}
    
    // Orca: The above causes pockmarks in top layer surfaces with a properly calibrated printer with PA and EM tuned.
    // Revert implementation to the prusa slicer approach that respects the infill/wall overlap setting
    // while retaining the gap fill logic below. The user can adjust the overlap calue to reduce overflow if needed.
    fill_surface_by_lines(surface, params2, polylines_rectilinear);
    ExPolygons unextruded_areas;
    Flow new_flow = params.flow;
    if (!polylines_rectilinear.empty()) {
        // calculate actual flow from spacing (which might have been adjusted by the infill
        // pattern generator)
        double flow_mm3_per_mm = params.flow.mm3_per_mm();
        double flow_width = params.flow.width();
        if (params.using_internal_flow) {
            // if we used the internal flow we're not doing a solid infill
            // so we can safely ignore the slight variation that might have
            // been applied to f->spacing
        }
        else {
            new_flow = params.flow.with_spacing(this->spacing);
            flow_mm3_per_mm = new_flow.mm3_per_mm();
            flow_width = new_flow.width();
        }

        extrusion_entities_append_paths_with_wipe(
                coll_nosort->entities, std::move(polylines_rectilinear),
                params.extrusion_role,
                flow_mm3_per_mm, float(flow_width), params.flow.height());
        unextruded_areas = diff_ex(this->no_overlap_expolygons, union_ex(coll_nosort->polygons_covered_by_spacing(10)));
    }
    else
        unextruded_areas = this->no_overlap_expolygons;

    //gapfill
    ExPolygons gapfill_areas = union_ex(unextruded_areas);
    if (!this->no_overlap_expolygons.empty())
            gapfill_areas = intersection_ex(gapfill_areas, this->no_overlap_expolygons);
    if (gapfill_areas.size() > 0 && params.density >= 1) {
        double min = 0.2 * new_flow.scaled_spacing() * (1 - INSET_OVERLAP_TOLERANCE);
        double max = 2. * new_flow.scaled_spacing();
        ExPolygons gaps_ex = diff_ex(
            opening_ex(gapfill_areas, float(min / 2.)),
            offset2_ex(gapfill_areas, -float(max / 2.), float(max / 2. + ClipperSafetyOffset)));
        //BBS: sort the gap_ex to avoid mess travel
        Points ordering_points;
        ordering_points.reserve(gaps_ex.size());
        ExPolygons gaps_ex_sorted;
        gaps_ex_sorted.reserve(gaps_ex.size());
        for (const ExPolygon &ex : gaps_ex)
            ordering_points.push_back(ex.contour.first_point());
        std::vector<Points::size_type> order = chain_points(ordering_points);
        for (size_t i : order)
            gaps_ex_sorted.emplace_back(std::move(gaps_ex[i]));

        ThickPolylines polylines;
        for (ExPolygon& ex : gaps_ex_sorted) {
            //BBS: Use DP simplify to avoid duplicated points and accelerate medial-axis calculation as well.
            ex.douglas_peucker(SCALED_RESOLUTION * 0.1);
            ex.medial_axis(min, max, &polylines);
        }

        if (!polylines.empty() && !is_bridge(params.extrusion_role)) {
            polylines.erase(std::remove_if(polylines.begin(), polylines.end(),
                [&](const ThickPolyline& p) {
                    return p.length() < scale_(params.config->filter_out_gap_fill.value);
                }), polylines.end());

            ExtrusionEntityCollection gap_fill;
            variable_width(polylines, erGapFill, params.flow, gap_fill.entities);
            coll_nosort->append(std::move(gap_fill.entities));

        }
    }

    if (!coll_nosort->empty()) {
        out.push_back(coll_nosort);
    } else {
        delete coll_nosort;
    }
}

void FillMonotonicLineWGapFill::fill_surface_by_lines(const Surface* surface, const FillParams& params, Polylines& polylines_out)
{
    // At the end, only the new polylines will be rotated back.
    size_t n_polylines_out_initial = polylines_out.size();

    // Shrink the input polygon a bit first to not push the infill lines out of the perimeters.
//    const float INFILL_OVERLAP_OVER_SPACING = 0.3f;
    const float INFILL_OVERLAP_OVER_SPACING = 0.45f;
    assert(INFILL_OVERLAP_OVER_SPACING > 0 && INFILL_OVERLAP_OVER_SPACING < 0.5f);

    // Rotate polygons so that we can work with vertical lines here
    std::pair<float, Point> rotate_vector = this->_infill_direction(surface);

    assert(params.full_infill());
    coord_t line_spacing = params.flow.scaled_spacing();

    // On the polygons of poly_with_offset, the infill lines will be connected.
    ExPolygonWithOffset poly_with_offset(
        surface->expolygon,
        - rotate_vector.first, 
        float(scale_(0 - (0.5 - INFILL_OVERLAP_OVER_SPACING) * params.flow.spacing())),
        float(scale_(0 - 0.5f * params.flow.spacing())));
    if (poly_with_offset.n_contours_inner == 0) {
        // Not a single infill line fits.
        //FIXME maybe one shall trigger the gap fill here?
        return;
    }

    BoundingBox bounding_box = poly_with_offset.bounding_box_src();

    // define flow spacing according to requested density
    if (params.full_infill() && !params.dont_adjust) {
        line_spacing = this->_adjust_solid_spacing(bounding_box.size()(0), line_spacing);
        this->spacing = unscale<double>(line_spacing);
    } else {
        // extend bounding box so that our pattern will be aligned with other layers
        // Transform the reference point to the rotated coordinate system.
        Point refpt = rotate_vector.second.rotated(-rotate_vector.first);
        bounding_box.merge(align_to_grid(
            bounding_box.min,
            Point(line_spacing, line_spacing),
            refpt));
    }

    // Intersect a set of euqally spaced vertical lines wiht expolygon.
    size_t  n_vlines = (bounding_box.max(0) - bounding_box.min(0) + line_spacing - 1) / line_spacing;
	coord_t x0 = bounding_box.min(0);
	if (params.full_infill())
		x0 += (line_spacing + coord_t(SCALED_EPSILON)) / 2;

    std::vector<SegmentedIntersectionLine> segs = slice_region_by_vertical_lines(poly_with_offset, n_vlines, x0, line_spacing);
    // Connect by horizontal / vertical links, classify the links based on link_max_length as too long.
	connect_segment_intersections_by_contours(poly_with_offset, segs, params, link_max_length);

    // Sometimes the outer contour pinches the inner contour from both sides along a single vertical line.
    // This situation is not handled correctly by generate_montonous_regions().
    // Insert phony OUTER_HIGH / OUTER_LOW pairs at the position where the contour is pinched.
    pinch_contours_insert_phony_outer_intersections(segs);
	std::vector<MonotonicRegion> regions = generate_montonous_regions(segs);

	connect_monotonic_regions(regions, poly_with_offset, segs);
    if (! regions.empty()) {
		std::mt19937_64 rng;
		std::vector<MonotonicRegionLink> path = chain_monotonic_regions(regions, poly_with_offset, segs, rng);
		polylines_from_paths(path, poly_with_offset, segs, polylines_out);
    }

    // paths must be rotated back
    for (Polylines::iterator it = polylines_out.begin() + n_polylines_out_initial; it != polylines_out.end(); ++ it) {
        // No need to translate, the absolute position is irrelevant.
        // it->translate(- rotate_vector.second(0), - rotate_vector.second(1));
        assert(! it->has_duplicate_points());
        it->rotate(rotate_vector.first);
        //FIXME rather simplify the paths to avoid very short edges?
        //assert(! it->has_duplicate_points());
        it->remove_duplicate_points();
    }
}*/

void FillLockedZag::fill_surface_locked_zag (const Surface *                          surface,
                                             const FillParams &                       params,
                                             std::vector<std::pair<Polylines, Flow>> &multi_width_polyline)
{
    // merge different part exps
    // diff skin flow
    Polylines skin_lines;
    Polylines skeloton_lines;
    double    offset_threshold  = params.skin_infill_depth;
    double    overlap_threshold = params.infill_lock_depth;
    Surface   cross_surface     = *surface;
    Surface   zig_surface       = *surface;
    // inner exps
    // inner union exps
    ExPolygons zig_expas   = offset_ex({surface->expolygon}, -offset_threshold);
    ExPolygons cross_expas = diff_ex(surface->expolygon, zig_expas);

    bool       zig_get    = false;
    FillParams zig_params = params;
    zig_params.horiz_move = 0;
    // generate skeleton for diff density
    auto generate_for_different_flow = [&multi_width_polyline](const std::map<Flow, ExPolygons> &flow_params, const Polylines &polylines) {
        auto it = flow_params.begin();
        while (it != flow_params.end()) {
            ExPolygons region_exp = union_safety_offset_ex(it->second);

            Polylines polys = intersection_pl(polylines, region_exp);
            multi_width_polyline.emplace_back(polys, it->first);
            it++;
        }
    };

    auto it = this->lock_param.skeleton_density_params.begin();
    while (it != this->lock_param.skeleton_density_params.end()) {
        ExPolygons region_exp = union_safety_offset_ex(it->second);
        ExPolygons exps       = intersection_ex(region_exp, zig_expas);
        zig_params.density    = it->first;
        exps                  = intersection_ex(offset_ex(exps, overlap_threshold), surface->expolygon);
        for (ExPolygon &exp : exps) {
            zig_surface.expolygon = exp;

            Polylines zig_polylines_out = this->fill_surface(&zig_surface, zig_params);
            skeloton_lines.insert(skeloton_lines.end(), zig_polylines_out.begin(), zig_polylines_out.end());
        }
        it++;
    }

    // set skeleton flow
    generate_for_different_flow(this->lock_param.skeleton_flow_params, skeloton_lines);

    // skin exps
    bool       cross_get      = false;
    FillParams cross_params   = params;
    cross_params.locked_zag = false;
    auto skin_density         = this->lock_param.skin_density_params.begin();
    while (skin_density != this->lock_param.skin_density_params.end()) {
        ExPolygons region_exp = union_safety_offset_ex(skin_density->second);
        ExPolygons exps       = intersection_ex(region_exp, cross_expas);
        cross_params.density  = skin_density->first;
        for (ExPolygon &exp : exps) {
            cross_surface.expolygon       = exp;
            Polylines cross_polylines_out = this->fill_surface(&cross_surface, cross_params);
            skin_lines.insert(skin_lines.end(), cross_polylines_out.begin(), cross_polylines_out.end());
        }
        skin_density++;
    }

    generate_for_different_flow(this->lock_param.skin_flow_params, skin_lines);
}

void FillLockedZag::fill_surface_extrusion(const Surface *surface, const FillParams &params, ExtrusionEntitiesPtr &out)
{
    Polylines polylines;
    ThickPolylines thick_polylines;
    std::vector<std::pair<Polylines, Flow>> multi_width_polyline;
    try {
        this->fill_surface_locked_zag(surface, params, multi_width_polyline);
    }
    catch (InfillFailedException&) {}

    if (!thick_polylines.empty() || !multi_width_polyline.empty()) {
        // Save into layer.
        ExtrusionEntityCollection* eec = nullptr;
        out.push_back(eec = new ExtrusionEntityCollection());
        // Only concentric fills are not sorted.
        eec->no_sort = this->no_sort();
        size_t idx   = eec->entities.size();
        {
            for (std::pair<Polylines, Flow> &poly_with_flow: multi_width_polyline) {
                // calculate actual flow from spacing (which might have been adjusted by the infill
                // pattern generator)
                double flow_mm3_per_mm = poly_with_flow.second.mm3_per_mm();
                double flow_width      = poly_with_flow.second.width();
                if (params.using_internal_flow) {
                    // if we used the internal flow we're not doing a solid infill
                    // so we can safely ignore the slight variation that might have
                    // been applied to f->spacing
                } else {
                    Flow new_flow   = poly_with_flow.second.with_spacing(this->spacing);
                    flow_mm3_per_mm = new_flow.mm3_per_mm();
                    flow_width      = new_flow.width();
                }
            extrusion_entities_append_paths(
                eec->entities, std::move(poly_with_flow.first),
                params.extrusion_role,
                flow_mm3_per_mm, float(flow_width), poly_with_flow.second.height());
            }
        }
        if (!params.can_reverse) {
            for (size_t i = idx; i < eec->entities.size(); i++)
                eec->entities[i]->set_reverse();
        }
    }
}

} // namespace Slic3r
