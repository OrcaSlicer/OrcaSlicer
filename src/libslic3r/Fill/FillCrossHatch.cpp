#include "../ClipperUtils.hpp"
#include "../ShortestPath.hpp"
#include "../Surface.hpp"
#include <cmath>
#include "FillBase.hpp"
#include "FillCrossHatch.hpp"

namespace Slic3r {

// CrossHatch Infill: Enhances 3D Printing Speed & Reduces Noise
// CrossHatch, as its name hints, alternates line direction by 90 degrees every few layers to improve adhesion.
// It introduces transform layers between direction shifts for better line cohesion, which fixes the weakness of line infill.
// The transform technique is inspired by David Eccles, improved 3D honeycomb but we made a more flexible implementation.
// This method notably increases printing speed, meeting the demands of modern high-speed 3D printers, and reduces noise for most layers.
// By Bambu Lab

// graph credits: David Eccles (gringer).
// But we made a different definition for points.
/*    o---o
 *   /     \
 *  /       \
 *           \       /
 *            \     /
 *             o---o
 *   p1   p2  p3   p4
 */

static Pointfs generate_one_cycle(double progress, coordf_t period)
{
    Pointfs out;
    double  offset = progress * 1. / 8. * period;
    out.reserve(4);
    out.push_back(Vec2d(0.25 * period - offset, offset));
    out.push_back(Vec2d(0.25 * period + offset, offset));
    out.push_back(Vec2d(0.75 * period - offset, -offset));
    out.push_back(Vec2d(0.75 * period + offset, -offset));
    return out;
}

static Polylines generate_transform_pattern(double inprogress, int direction, coordf_t ingrid_size, coordf_t inwidth, coordf_t inheight)
{
    coordf_t  width     = inwidth;
    coordf_t  height    = inheight;
    coordf_t  grid_size = ingrid_size * 2; // we due with odd and even saparately.
    double    progress  = inprogress;
    Polylines out_polylines;

    // generate template patterns;
    Pointfs one_cycle_points = generate_one_cycle(progress, grid_size);

    Polyline one_cycle;
    one_cycle.points.reserve(one_cycle_points.size());
    for (size_t i = 0; i < one_cycle_points.size(); i++) one_cycle.points.push_back(Point(one_cycle_points[i]));

    // swap if vertical
    if (direction < 0) {
        width  = height;
        height = inwidth;
    }

    // replicate polylines;
    Polylines odd_polylines;
    Polyline  odd_poly;
    int       num_of_cycle = width / grid_size + 2;
    odd_poly.points.reserve(num_of_cycle * one_cycle.size());

    // replicate to odd line
    Point translate = Point(0, 0);
    for (size_t i = 0; i < num_of_cycle; i++) {
        Polyline odd_points;
        odd_points = Polyline(one_cycle);
        odd_points.translate(Point(i * grid_size, 0.0));
        odd_poly.points.insert(odd_poly.points.end(), odd_points.begin(), odd_points.end());
    }

    // fill the height
    int num_of_lines = height / grid_size + 2;
    odd_polylines.reserve(num_of_lines * odd_poly.size());
    for (size_t i = 0; i < num_of_lines; i++) {
        Polyline poly = odd_poly;
        poly.translate(Point(0.0, grid_size * i));
        odd_polylines.push_back(poly);
    }
    // save to output
    out_polylines.insert(out_polylines.end(), odd_polylines.begin(), odd_polylines.end());

    // replicate to even lines
    Polylines even_polylines;
    even_polylines.reserve(odd_polylines.size());
    for (size_t i = 0; i < odd_polylines.size(); i++) {
        Polyline even = odd_poly;
        even.translate(Point(-0.5 * grid_size, (i + 0.5) * grid_size));
        even_polylines.push_back(even);
    }

    // save for output
    out_polylines.insert(out_polylines.end(), even_polylines.begin(), even_polylines.end());

    // change to vertical if need
    if (direction < 0) {
        // swap xy, see if we need better performance method
        for (Polyline &poly : out_polylines) {
            for (Point &p : poly) { std::swap(p.x(), p.y()); }
        }
    }

    return out_polylines;
}

static Polylines generate_repeat_pattern(int direction, coordf_t grid_size, coordf_t inwidth, coordf_t inheight)
{
    coordf_t  width  = inwidth;
    coordf_t  height = inheight;
    Polylines out_polylines;

    // swap if vertical
    if (direction < 0) {
        width  = height;
        height = inwidth;
    }

    int num_of_lines = height / grid_size + 1;
    out_polylines.reserve(num_of_lines);

    for (int i = 0; i < num_of_lines; i++) {
        Polyline poly;
        poly.points.reserve(2);
        poly.append(Point(coordf_t(0), coordf_t(grid_size * i)));
        poly.append(Point(width, coordf_t(grid_size * i)));
        out_polylines.push_back(poly);
    }

    // change to vertical if needed
    if (direction < 0) {
        // swap xy
        for (Polyline &poly : out_polylines) {
            for (Point &p : poly) { std::swap(p.x(), p.y()); }
        }
    }

    return out_polylines;
}

// it makes the real patterns that overlap the bounding box
// repeat_ratio define the ratio between the height of repeat pattern and grid
static Polylines generate_infill_layers(coordf_t z_height, double repeat_ratio, coordf_t grid_size, coordf_t width, coordf_t height)
{
    Polylines result;
    coordf_t  trans_layer_size  = grid_size * 0.4;          // upper.
    coordf_t  repeat_layer_size = grid_size * repeat_ratio; // lower.
    z_height                    += repeat_layer_size / 2 + trans_layer_size;   // offset to improve first few layer strength and reduce the risk of warpping.
    coordf_t  period            = trans_layer_size + repeat_layer_size;
    coordf_t  remains           = z_height - std::floor(z_height / period) * period;
    coordf_t  trans_z           = remains - repeat_layer_size; // put repeat layer first.
    coordf_t  repeat_z          = remains;

    int phase     = fmod(z_height, period * 2) - (period - 1); // add epsilon
    int direction = phase <= 0 ? -1 : 1;

    // this is a repeat layer
    if (trans_z < 0) {
        result = generate_repeat_pattern(direction, grid_size, width, height);
    }
    // this is a transform layer
    else {
        double progress = fmod(trans_z, trans_layer_size) / trans_layer_size;

        // split the progress to forward and backward, with a opposite direction.
        if (progress < 0.5)
            result = generate_transform_pattern((progress + 0.1) * 2, direction, grid_size, width, height); // increase overlapping.
        else
            result = generate_transform_pattern((1.1 - progress) * 2, -1 * direction, grid_size, width, height);
    }

    return result;
}

void FillCrossHatch ::_fill_surface_single(
    const FillParams &params, unsigned int thickness_layers, const std::pair<float, Point> &direction, ExPolygon expolygon, Polylines &polylines_out)
{
    // rotate angle
    auto infill_angle = float(this->angle);
    if (std::abs(infill_angle) >= EPSILON) expolygon.rotate(-infill_angle);

    // get the rotated bounding box
    BoundingBox bb = expolygon.contour.bounding_box();

    // linespace modifier
    double density_adjusted = params.density / params.multiline;
    coord_t line_spacing = coord_t(scale_(this->spacing) / density_adjusted);

    // reduce density
    if (params.density < 0.999) line_spacing *= 1.08;

    bb.merge(align_to_grid(bb.min, Point(line_spacing * 4, line_spacing * 4)));

    // generate pattern
    //Orca: optimize the cross-hatch infill pattern to improve strength when low infill density is used.
    double repeat_ratio = 1.0;
    if (params.density < 0.3)
        repeat_ratio = std::clamp(1.0 - std::exp(-5 * params.density), 0.2, 1.0);

    Polylines polylines = generate_infill_layers(scale_(this->z), repeat_ratio, line_spacing, bb.size()(0), bb.size()(1));

    // shift the pattern to the actual space
    for (Polyline &pl : polylines) { pl.translate(bb.min); }

    // Apply multiline offset if needed
    multiline_fill(polylines, params, spacing);

    polylines = intersection_pl(polylines, to_polygons(expolygon));

    // --- remove small remains from gyroid infill
    if (!polylines.empty()) {
        // Remove very small bits, but be careful to not remove infill lines connecting thin walls!
        // The infill perimeter lines should be separated by around a single infill line width.
        const double minlength = scale_(0.8 * this->spacing);
        polylines.erase(std::remove_if(polylines.begin(), polylines.end(), [minlength](const Polyline &pl)
            { return pl.length() < minlength; }), polylines.end());
    }

    if (!polylines.empty()) {
        int infill_start_idx = polylines_out.size(); // only rotate what belongs to us.
        // connect lines
        chain_or_connect_infill(std::move(polylines), expolygon, polylines_out, this->spacing, params);

        // rotate back
        if (std::abs(infill_angle) >= EPSILON) {
            for (auto it = polylines_out.begin() + infill_start_idx; it != polylines_out.end(); ++it) it->rotate(infill_angle);
        }
    }
}

} // namespace Slic3r