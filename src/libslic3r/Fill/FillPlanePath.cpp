#include "../ClipperUtils.hpp"
#include "../ShortestPath.hpp"
#include "../Surface.hpp"

#include "FillPlanePath.hpp"

namespace Slic3r {

class InfillPolylineClipper : public InfillPolylineOutput {
public:
    InfillPolylineClipper(const BoundingBox bbox, const double scale_out) : InfillPolylineOutput(scale_out), m_bbox(bbox) {}

    void            add_point(const Vec2d &pt);
    Points&&        result() { return std::move(m_out); }
    bool            clips() const override { return true; }

private:
    enum class Side {
        Left   = 1,
        Right  = 2,
        Top    = 4,
        Bottom = 8
    };

    int sides(const Point &p) const {
        return int(p.x() < m_bbox.min.x()) * int(Side::Left) +
               int(p.x() > m_bbox.max.x()) * int(Side::Right) +
               int(p.y() < m_bbox.min.y()) * int(Side::Bottom) +
               int(p.y() > m_bbox.max.y()) * int(Side::Top);
    };

    // Bounding box to clip the polyline with.
    BoundingBox m_bbox;

    // Classification of the two last points processed.
    int         m_sides_prev;
    int         m_sides_this;
};

void InfillPolylineClipper::add_point(const Vec2d &fpt)
{
    const Point pt{ this->scaled(fpt) };

    if (m_out.size() < 2) {
        // Collect the two first points and their status.
        (m_out.empty() ? m_sides_prev : m_sides_this) = sides(pt);
        m_out.emplace_back(pt);
    } else {
        // Classify the last inserted point, possibly remove it.
        int sides_next = sides(pt);
        if (// This point is inside. Take it.
            m_sides_this == 0 ||
            // Either this point is outside and previous or next is inside, or
            // the edge possibly cuts corner of the bounding box.
            (m_sides_prev & m_sides_this & sides_next) == 0) {
            // Keep the last point.
            m_sides_prev = m_sides_this;
        } else {
            // All the three points (this, prev, next) are outside at the same side.
            // Ignore the last point.
            m_out.pop_back();
        }
        // And save the current point.
        m_out.emplace_back(pt);
        m_sides_this = sides_next;
    }
}

void FillPlanePath::_fill_surface_single(
    const FillParams                &params, 
    unsigned int                     thickness_layers,
    const std::pair<float, Point>   &direction, 
    ExPolygon                        expolygon,
    Polylines                       &polylines_out)
{
    expolygon.rotate(-direction.first);

    //FIXME Vojtech: We are not sure whether the user expects the fill patterns on visible surfaces to be aligned across all the islands of a single layer.
    // One may align for this->centered() to align the patterns for Archimedean Chords and Octagram Spiral patterns.
    const bool align = params.density < 0.995;

    BoundingBox snug_bounding_box = get_extents(expolygon).inflated(SCALED_EPSILON);

    // Expand the bounding box to avoid artifacts at the edges
    snug_bounding_box.offset(scale_(this->spacing)*params.multiline); 

    // Rotated bounding box of the area to fill in with the pattern.
    BoundingBox bounding_box = align ?
        // Sparse infill needs to be aligned across layers. Align infill across layers using the object's bounding box.
        this->bounding_box.rotated(-direction.first) :
        // Solid infill does not need to be aligned across layers, generate the infill pattern
        // around the clipping expolygon only.
        snug_bounding_box;

    Point shift = this->centered() ? 
        bounding_box.center() :
        bounding_box.min;
    expolygon.translate(-shift.x(), -shift.y());
    bounding_box.translate(-shift.x(), -shift.y());

    Polyline polyline;
    {
        auto distance_between_lines = scaled<double>(this->spacing) * params.multiline / params.density;
        auto min_x = coord_t(ceil(coordf_t(bounding_box.min.x()) / distance_between_lines));
        auto min_y = coord_t(ceil(coordf_t(bounding_box.min.y()) / distance_between_lines));
        auto max_x = coord_t(ceil(coordf_t(bounding_box.max.x()) / distance_between_lines));
        auto max_y = coord_t(ceil(coordf_t(bounding_box.max.y()) / distance_between_lines));
        auto resolution = scaled<double>(params.resolution) / distance_between_lines;
        if (align) {
            // Filling in a bounding box over the whole object, clip generated polyline against the snug bounding box.
            snug_bounding_box.translate(-shift.x(), -shift.y());
            InfillPolylineClipper output(snug_bounding_box, distance_between_lines);
            this->generate(min_x, min_y, max_x, max_y, resolution, output);
            polyline.points = std::move(output.result());
        } else {
            // Filling in a snug bounding box, no need to clip.
            InfillPolylineOutput output(distance_between_lines);
            this->generate(min_x, min_y, max_x, max_y, resolution, output);
            polyline.points = std::move(output.result());
        }
    }

    Polylines polylines = {polyline};

    // Apply multiline offset if needed
    multiline_fill(polylines, params, spacing);

    if (polyline.size() >= 2) {
        polylines = intersection_pl(std::move(polylines), expolygon);
        if (!polylines.empty()) {
            Polylines chained;
            if (params.dont_connect() || params.density > 0.5) {
                // ORCA: special flag for flow rate calibration
                auto is_flow_calib = params.extrusion_role == erTopSolidInfill &&
                                     this->print_object_config->has("calib_flowrate_topinfill_special_order") &&
                                     this->print_object_config->option("calib_flowrate_topinfill_special_order")->getBool() &&
                                     dynamic_cast<FillArchimedeanChords*>(this);
                if (is_flow_calib) {
                    // We want the spiral part to be printed inside-out
                    // Find the center spiral line first, by looking for the longest one
                    auto     it            = std::max_element(polylines.begin(), polylines.end(),
                                                              [](const Polyline& a, const Polyline& b) { return a.length() < b.length(); });
                    Polyline center_spiral = std::move(*it);

                    // Ensure the spiral is printed from inside to out
                    if (center_spiral.first_point().squaredNorm() > center_spiral.last_point().squaredNorm()) {
                        center_spiral.reverse();
                    }

                    // Chain the other polylines
                    polylines.erase(it);
                    chained = chain_polylines(std::move(polylines));

                    // Then add the center spiral back
                    chained.push_back(std::move(center_spiral));
                } else {
                    chained = chain_polylines(std::move(polylines));
                }
            } else
                connect_infill(std::move(polylines), expolygon, chained, this->spacing, params);
            // paths must be repositioned and rotated back
            for (Polyline& pl : chained) {
                pl.translate(shift.x(), shift.y());
                pl.rotate(direction.first);
            }
            append(polylines_out, std::move(chained));
        }
    }
}

// Follow an Archimedean spiral, in polar coordinates: r=a+b\theta
template<typename Output>
static void generate_archimedean_chords(coord_t min_x, coord_t min_y, coord_t max_x, coord_t max_y, const double resolution, Output &output)
{
    // Radius to achieve.
    coordf_t rmax = std::sqrt(coordf_t(max_x)*coordf_t(max_x)+coordf_t(max_y)*coordf_t(max_y)) * std::sqrt(2.) + 1.5;
    // Now unwind the spiral.
    coordf_t a = 1.;
    coordf_t b = 1./(2.*M_PI);
    coordf_t theta = 0.;
    coordf_t r = 1;
    Pointfs out;
    //FIXME Vojtech: If used as a solid infill, there is a gap left at the center.
    output.add_point({ 0, 0 });
    output.add_point({ 1, 0 });
    while (r < rmax) {
        // Discretization angle to achieve a discretization error lower than resolution.
        theta += 2. * acos(1. - resolution / r);
        r = a + b * theta;
        output.add_point({ r * cos(theta), r * sin(theta) });
    }
}

void FillArchimedeanChords::generate(coord_t min_x, coord_t min_y, coord_t max_x, coord_t max_y, const double resolution, InfillPolylineOutput &output)
{
    if (output.clips())
        generate_archimedean_chords(min_x, min_y, max_x, max_y, resolution, static_cast<InfillPolylineClipper&>(output));
    else
        generate_archimedean_chords(min_x, min_y, max_x, max_y, resolution, output);
}

// Adapted from 
// http://cpansearch.perl.org/src/KRYDE/Math-PlanePath-122/lib/Math/PlanePath/HilbertCurve.pm
//
// state=0    3--2   plain
//               |
//            0--1
//
// state=4    1--2  transpose
//            |  |
//            0  3
//
// state=8
//
// state=12   3  0  rot180 + transpose
//            |  |
//            2--1
//
static inline Point hilbert_n_to_xy(const size_t n)
{
    static constexpr const int next_state[16] { 4,0,0,12, 0,4,4,8, 12,8,8,4, 8,12,12,0 };
    static constexpr const int digit_to_x[16] { 0,1,1,0, 0,0,1,1, 1,0,0,1, 1,1,0,0 };
    static constexpr const int digit_to_y[16] { 0,0,1,1, 0,1,1,0, 1,1,0,0, 1,0,0,1 };

    // Number of 2 bit digits.
    size_t ndigits = 0;
    {
        size_t nc = n;
        while(nc > 0) {
            nc >>= 2;
            ++ ndigits;
        }
    }
    int state = (ndigits & 1) ? 4 : 0;
    coord_t x = 0;
    coord_t y = 0;
    for (int i = (int)ndigits - 1; i >= 0; -- i) {
        int digit = (n >> (i * 2)) & 3;
        state += digit;
        x |= digit_to_x[state] << i;
        y |= digit_to_y[state] << i;
        state = next_state[state];
    }
    return Point(x, y);
}

template<typename Output>
static void generate_hilbert_curve(coord_t min_x, coord_t min_y, coord_t max_x, coord_t max_y, Output &output)
{
    // Minimum power of two square to fit the domain.
    size_t sz = 2;
    size_t pw = 1;
    {
        size_t sz0 = std::max(max_x + 1 - min_x, max_y + 1 - min_y);
        while (sz < sz0) {
            sz = sz << 1;
            ++ pw;
        }
    }

    size_t sz2 = sz * sz;
    output.reserve(sz2);
    for (size_t i = 0; i < sz2; ++ i) {
        Point p = hilbert_n_to_xy(i);
        output.add_point({ p.x() + min_x, p.y() + min_y });
    }
}

void FillHilbertCurve::generate(coord_t min_x, coord_t min_y, coord_t max_x, coord_t max_y, const double /* resolution */, InfillPolylineOutput &output)
{
    if (output.clips())
        generate_hilbert_curve(min_x, min_y, max_x, max_y, static_cast<InfillPolylineClipper&>(output));
    else
        generate_hilbert_curve(min_x, min_y, max_x, max_y, output);
}

template<typename Output>
static void generate_octagram_spiral(coord_t min_x, coord_t min_y, coord_t max_x, coord_t max_y, Output &output)
{
    // Radius to achieve.
    coordf_t rmax = std::sqrt(coordf_t(max_x)*coordf_t(max_x)+coordf_t(max_y)*coordf_t(max_y)) * std::sqrt(2.) + 1.5;
    // Now unwind the spiral.
    coordf_t r = 0;
    coordf_t r_inc = sqrt(2.);
    output.add_point({ 0., 0. });
    while (r < rmax) {
        r += r_inc;
        coordf_t rx = r / sqrt(2.);
        coordf_t r2 = r + rx;
        output.add_point({ r,   0. });
        output.add_point({ r2,  rx });
        output.add_point({ rx,  rx });
        output.add_point({ rx,  r2 });
        output.add_point({ 0.,  r  });
        output.add_point({-rx,  r2 });
        output.add_point({-rx,  rx });
        output.add_point({-r2,  rx });
        output.add_point({- r,  0. });
        output.add_point({-r2, -rx });
        output.add_point({-rx, -rx });
        output.add_point({-rx, -r2 });
        output.add_point({ 0., -r  });
        output.add_point({ rx, -r2 });
        output.add_point({ rx, -rx });
        output.add_point({ r2+r_inc, -rx });
    }
}

void FillOctagramSpiral::generate(coord_t min_x, coord_t min_y, coord_t max_x, coord_t max_y, const double /* resolution */, InfillPolylineOutput &output)
{
    if (output.clips())
        generate_octagram_spiral(min_x, min_y, max_x, max_y, static_cast<InfillPolylineClipper&>(output));
    else
        generate_octagram_spiral(min_x, min_y, max_x, max_y, output);
}

} // namespace Slic3r
