// Breakaway fin support (support type "fins").
//
// A fin is a thin solid wall standing on the bed beside the object, a small XY gap away from it,
// with a one-layer "tine" every few layers that bridges the gap and fuses into the object. The
// wall props the object up; the tines stop the object from leaning away from the wall, which
// matters most for parts printed tilted onto an edge. Because a tine is a single horizontal bead
// in the plane of the layer, it snaps off cleanly when the fin is bent away.
// The technique and its dimensions (0.2 mm standoff, one-layer tines about 0.5 mm wide,
// a 1 mm base at the bed) follow Slant3D's designed-in support fins (YouTube @slant3d,
// "How to Design Better Support Fins"). CNC Kitchen (YouTube @CNCKitchen, "Stop Printing Flat:
// The 45 Degree Secret for Stronger Parts") measured why printing tilted pays off: PLA printed at
// 45 degrees tested 27% stronger than upright. Fins are what make that orientation printable.
// The slicer-side design draws on Support Fins by Matthew Trahan (gittrahan,
// https://github.com/gittrahan/support-fins, MIT), which adds these fins to STL files;
// the default fin thickness and tine depth come from its geometry spec.
//
// Fins hold the object up rather than an overhang surface, so they do not use overhang
// detection. For every object layer up to support_fin_height:
//  1. The fin region is the area under the object (the projection of all layers above) that
//     is clear of the object down to the bed, kept support_object_xy_distance off the object
//     and support_top_z_distance below it. Support blockers are subtracted.
//  2. The region is clipped to parallel strips (the fins), support_fin_thickness wide and one
//     every support_fin_spacing. The strips are fixed in XY for the whole object, so each fin
//     is one continuous wall. They run across the long axis of the fin regions, so they end
//     at the edge the object leans over. A column between two strips gets its own fin.
//     With support_fin_lean_side_only, only the fin columns on the side the object leans toward
//     are kept (see keep_lean_side()).
//     With support_fin_cross_spacing, fins also run across the main fins, bracing them into a scaffold.
//  3. Within the first kBaseHeight above the bed the strips are widened into a foot.
//  4. On tine layers, each fin that ends at the object is extended across the XY gap and
//     support_fin_tine_depth into the object slice. Tine rows are support_fin_tine_spacing apart; with
//     support_fin_tine_base_rows, extra rows are packed just above the foot, where the object is least
//     stable (Slant3D). Spacing by distance keeps the number of tines proportional to the fin height.
//  5. With support_fin_interface_layers, the fin region right under the object is printed as solid
//     interface layers across the fin tops.
// The result is printed through the common support toolpath generator.

#include "SupportFins.hpp"
#include "SupportCommon.hpp"
#include "../ClipperUtils.hpp"
#include "../Layer.hpp"
#include "../Print.hpp"

#include <algorithm>
#include <cmath>
#include <boost/log/trivial.hpp>
#include <tbb/parallel_for.h>

namespace Slic3r {

// Height of the widened fin foot on the bed, and its width as a multiple of the fin thickness.
static constexpr double kBaseHeight     = 1.0;
static constexpr double kBaseWidthRatio = 3.0;
// A fin taller than this many times its length is reported as slender.
static constexpr double kSlenderRatio   = 10.0;

// Extra rows near the bed: the first just above the foot, then gaps from kTineGapMin growing by
// kTineGapGrowth until they reach the regular tine spacing.
static constexpr double kTineGapMin    = 0.8;
static constexpr double kTineGapGrowth = 1.6;

std::vector<bool> fin_tine_layers(const PrintObject &object)
{
    const PrintObjectConfig &config = object.config();
    const auto   layers  = object.layers();
    const double spacing = config.support_fin_tine_spacing.value;
    std::vector<bool> rows(layers.size(), false);
    if (spacing <= 0.)
        return rows;
    // Regular rows, starting just above the foot.
    double next = kBaseHeight;
    double gap  = config.support_fin_tine_base_rows.value ? std::min(kTineGapMin, spacing) : spacing;
    for (size_t i = 1; i < layers.size(); ++ i)
        if (layers[i]->bottom_z() > next - EPSILON) {
            rows[i] = true;
            next    = layers[i]->print_z + gap;
            gap     = std::min(gap * kTineGapGrowth, spacing);
        }
    return rows;
}

// Strips are built horizontal in a frame rotated by -angle, then rotated back to run along `angle`.
static BoundingBox rotated_extents(const BoundingBox &bbox, double angle, coord_t margin)
{
    Polygon frame = bbox.polygon();
    frame.rotate(-angle);
    BoundingBox rbox = frame.bounding_box();
    rbox.offset(margin);
    return rbox;
}

static Polygon fin_strip(const BoundingBox &rbox, double angle, coord_t y, coord_t width)
{
    const coord_t half = width / 2;
    Polygon strip({ { rbox.min.x(), y - half }, { rbox.max.x(), y - half }, { rbox.max.x(), y + half }, { rbox.min.x(), y + half } });
    strip.rotate(angle);
    return strip;
}

// Parallel strips `width` wide with centers on a global grid of `pitch`, running along `angle`
// and covering `bbox`.
static Polygons fin_strips(const BoundingBox &bbox, double angle, coord_t pitch, coord_t width)
{
    const BoundingBox rbox = rotated_extents(bbox, angle, width);
    Polygons strips;
    for (coord_t k = coord_t(std::floor(double(rbox.min.y()) / pitch)); k * pitch <= rbox.max.y() + pitch; ++ k)
        strips.emplace_back(fin_strip(rbox, angle, k * pitch, width));
    return strips;
}

// Direction the fins run, in radians: across the long axis of the fin regions.
// Second moments are summed per island about the island's own centroid, so islands on
// opposite sides of the object do not read as one region spread across the object.
static double fin_angle(const std::vector<Polygons> &regions)
{
    double sxx = 0., syy = 0., sxy = 0.;
    for (const Polygons &region : regions)
        for (const ExPolygon &island : union_ex(region)) {
            double a = 0., cx = 0., cy = 0., ixx = 0., iyy = 0., ixy = 0.;
            auto add = [&](const Polygon &poly) {
                const Points &pts = poly.points;
                for (size_t i = 0; i < pts.size(); ++ i) {
                    const Point &p0 = pts[i], &p1 = pts[(i + 1) % pts.size()];
                    const double x0 = unscale<double>(p0.x()), y0 = unscale<double>(p0.y());
                    const double x1 = unscale<double>(p1.x()), y1 = unscale<double>(p1.y());
                    const double c  = x0 * y1 - x1 * y0;
                    a   += c;
                    cx  += (x0 + x1) * c;
                    cy  += (y0 + y1) * c;
                    ixx += (x0 * x0 + x0 * x1 + x1 * x1) * c;
                    iyy += (y0 * y0 + y0 * y1 + y1 * y1) * c;
                    ixy += (x0 * y1 + 2. * x0 * y0 + 2. * x1 * y1 + x1 * y0) * c;
                }
            };
            // Contours are CCW and holes CW, so holes subtract.
            add(island.contour);
            for (const Polygon &hole : island.holes)
                add(hole);
            a *= 0.5;
            if (a <= 0.)
                continue;
            cx /= 6. * a;
            cy /= 6. * a;
            sxx += ixx / 12. - a * cx * cx;
            syy += iyy / 12. - a * cy * cy;
            sxy += ixy / 24. - a * cx * cy;
        }
    // A round or square footprint has no long axis; keep a fixed direction instead of rounding noise.
    if (std::hypot(sxx - syy, 2. * sxy) < 1e-3 * (sxx + syy))
        return 0.;
    return 0.5 * std::atan2(2. * sxy, sxx - syy) + 0.5 * M_PI;
}

// Area-weighted centroid of `expolys` (holes subtract) in scaled coordinates; `area` receives the total area.
static Vec2d area_centroid(const ExPolygons &expolys, double &area)
{
    Vec2d sum = Vec2d::Zero();
    area = 0.;
    for (const ExPolygon &expoly : expolys) {
        // Polygon::area() is signed: contours are CCW (positive) and holes CW (negative).
        sum  += expoly.contour.area() * expoly.contour.centroid().cast<double>();
        area += expoly.contour.area();
        for (const Polygon &hole : expoly.holes) {
            sum  += hole.area() * hole.centroid().cast<double>();
            area += hole.area();
        }
    }
    return area > 0. ? Vec2d(sum / area) : sum;
}

// Keeps only the fin columns on the side the object leans toward: the side of its bed contact
// that its center of mass lies on. Those fins hold the object up in compression; a tine is too
// thin to hold it in tension from the other side. A balanced object keeps its largest column's side.
static void keep_lean_side(const LayerPtrs &layers, std::vector<Polygons> &regions)
{
    // Center of mass in XY: slice centroids weighted by slice area times layer height.
    Vec2d  com  = Vec2d::Zero();
    double mass = 0.;
    for (const Layer *layer : layers) {
        double      area;
        const Vec2d c = area_centroid(layer->lslices, area);
        com  += c * area * layer->height;
        mass += area * layer->height;
    }
    double      base_area;
    const Vec2d base = area_centroid(layers.front()->lslices, base_area);
    if (mass <= 0. || base_area <= 0.)
        return;
    com /= mass;

    // Fin regions only narrow going up, so the lowest one holds every fin column.
    auto lowest = std::find_if(regions.begin(), regions.end(), [](const Polygons &region) { return ! region.empty(); });
    if (lowest == regions.end())
        return;
    const ExPolygons columns = union_ex(*lowest);
    auto column_offset = [&base](const ExPolygon &column) { double area; return Vec2d(area_centroid({ column }, area) - base); };

    Vec2d lean = com - base;
    if (lean.norm() < scale_(0.5)) {
        const auto largest = std::max_element(columns.begin(), columns.end(),
            [](const ExPolygon &a, const ExPolygon &b) { return a.area() < b.area(); });
        lean = column_offset(*largest);
    }
    Polygons keep;
    for (const ExPolygon &column : columns)
        if (column_offset(column).dot(lean) > 0.)
            append(keep, to_polygons(column));
    if (keep.empty())
        return;
    // Upper regions sit inside the bed-level columns; allow for rounding at their edges.
    keep = offset(keep, float(scale_(0.1)));
    for (Polygons &region : regions)
        if (! region.empty())
            region = intersection(region, keep);
}

bool generate_fin_support(PrintObject &object)
{
    const PrintObjectConfig &config         = object.config();
    const SlicingParameters &slicing_params = object.slicing_parameters();
    SupportParameters        support_params(object);
    const LayerPtrs         &layers         = object.layers();
    const size_t             num_layers     = layers.size();
    if (num_layers < 2)
        return false;

    BOOST_LOG_TRIVIAL(info) << "Fin support - start";

    // Object outlines with holes filled: fins never stand inside the object.
    std::vector<Polygons> outlines(num_layers);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, num_layers), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t i = range.begin(); i < range.end(); ++ i)
            for (const ExPolygon &expoly : layers[i]->lslices)
                outlines[i].emplace_back(expoly.contour);
    });

    std::vector<Polygons> blockers = object.slice_support_blockers();
    object.project_and_append_custom_facets(false, EnforcerBlockerType::BLOCKER, blockers);

    // 1. Fin regions, only for the layers up to the fin height.
    const coordf_t fin_height = config.support_fin_height.value;
    const coordf_t top_gap    = config.support_top_z_distance.value;
    size_t         num_fin_layers = num_layers;
    if (fin_height > 0.)
        while (num_fin_layers > 0 && layers[num_fin_layers - 1]->print_z > fin_height + EPSILON)
            -- num_fin_layers;
    // `shadow` is the projection of everything above a layer.
    std::vector<Polygons> shadow(num_fin_layers);
    {
        Polygons above;
        for (size_t i = num_layers; i -- > 0;) {
            if (i < num_fin_layers)
                shadow[i] = above;
            if (! outlines[i].empty())
                above = union_(above, outlines[i]);
            if (object.print()->canceled())
                return false;
        }
    }
    // A fin stands on the bed, so it may not pass through anything at or under its layer:
    // the object (`below`) or a support blocker (`blocked`).
    std::vector<Polygons> regions(num_layers);
    {
        Polygons below, blocked;
        for (size_t i = 0; i < num_fin_layers; ++ i) {
            below = union_(below, outlines[i]);
            if (i < blockers.size() && ! blockers[i].empty())
                blocked = union_(blocked, blockers[i]);
            const Layer &layer = *layers[i];
            if (! shadow[i].empty()) {
                Polygons keep_out = below;
                // Stop fins support_top_z_distance below any part of the object that hangs over them.
                for (size_t j = i + 1; j < num_layers && layers[j]->bottom_z() < layer.print_z + top_gap - EPSILON; ++ j)
                    append(keep_out, outlines[j]);
                const float gap = float(scale_(i == 0 ? support_params.gap_xy_first_layer : support_params.gap_xy));
                regions[i] = diff(shadow[i], offset(union_(keep_out), gap));
                if (! blocked.empty())
                    regions[i] = diff(regions[i], blocked);
            }
            Polygons().swap(shadow[i]);
            if (object.print()->canceled())
                return false;
        }
    }

    if (config.support_fin_lean_side_only.value)
        keep_lean_side(layers, regions);

    BoundingBox bbox;
    for (const Polygons &region : regions)
        if (! region.empty())
            bbox.merge(get_extents(region));
    if (! bbox.defined)
        return false;

    // 2.-3. Clip the regions to fins.
    const coord_t  spacing     = support_params.support_material_flow.scaled_spacing();
    // Fins are filled with concentric loops, two lines per loop. Round the thickness to whole loops:
    // a leftover narrower than a line is not filled and leaves a slot down the middle of the fin.
    const coord_t  thickness   = 2 * spacing * std::max<coord_t>(1, coord_t(std::lround(scaled<double>(config.support_fin_thickness.value) / (2. * spacing))));
    const coord_t  pitch       = scaled<coord_t>(config.support_fin_spacing.value) + thickness;
    // A tine is one bead out and one bead back: two extrusions wide, never wider than the fin.
    const coord_t  tine_width  = std::min(thickness, 2 * spacing);
    const double   angle       = fin_angle(regions);
    Polygons       fins        = fin_strips(bbox, angle, pitch, thickness);
    Polygons       feet        = fin_strips(bbox, angle, pitch, coord_t(kBaseWidthRatio * thickness));
    Polygons       tine_strips = fin_strips(bbox, angle, pitch, tine_width);
    const BoundingBox rbox     = rotated_extents(bbox, angle, thickness);
    // Pieces narrower than half an extrusion cannot be printed; drop them.
    const float    min_width   = 0.25f * float(spacing);
    // Scaffold: cross fins at right angles to the main fins. Tines stay on the main fins only.
    const double      cross_angle = angle + 0.5 * M_PI;
    const coord_t     cross_pitch = config.support_fin_cross_spacing.value > 0. ? scaled<coord_t>(config.support_fin_cross_spacing.value) + thickness : 0;
    const BoundingBox cross_rbox  = rotated_extents(bbox, cross_angle, thickness);
    const Polygons    main_fins   = fins;
    if (cross_pitch > 0) {
        append(fins, fin_strips(bbox, cross_angle, cross_pitch, thickness));
        append(feet, fin_strips(bbox, cross_angle, cross_pitch, coord_t(kBaseWidthRatio * thickness)));
    }
    // Regions only narrow going up, so the lowest layer holds every fin column. A column narrower
    // than the fin spacing may fall between two grid fins: give it its own fin through its center,
    // fixed for all layers so the fin stands on itself all the way down to the bed.
    for (const Polygons &region : regions)
        if (! region.empty()) {
            for (const ExPolygon &island : union_ex(region))
                if (intersection(to_polygons(island), main_fins).empty()) {
                    Point c = island.contour.centroid();
                    c.rotate(-angle);
                    fins.emplace_back(fin_strip(rbox, angle, c.y(), thickness));
                    feet.emplace_back(fin_strip(rbox, angle, c.y(), coord_t(kBaseWidthRatio * thickness)));
                    tine_strips.emplace_back(fin_strip(rbox, angle, c.y(), tine_width));
                    if (cross_pitch > 0) {
                        // Brace it with a cross fin through the same center.
                        Point cc = island.contour.centroid();
                        cc.rotate(-cross_angle);
                        fins.emplace_back(fin_strip(cross_rbox, cross_angle, cc.y(), thickness));
                        feet.emplace_back(fin_strip(cross_rbox, cross_angle, cc.y(), coord_t(kBaseWidthRatio * thickness)));
                    }
                }
            break;
        }
    fins        = union_(fins);
    feet        = union_(feet);
    tine_strips = union_(tine_strips);
    const std::vector<bool> tine_row = fin_tine_layers(object);
    const float    depth       = float(scale_(config.support_fin_tine_depth.value));

    const int      interface_layers = config.support_fin_interface_layers.value;
    // The deck is solid, with lines across the fins (a fill angle is the line direction), so its
    // first layer bridges from fin to fin and every layer above it rests on the one below.
    support_params.support_interface_pattern = smipRectilinear;
    support_params.support_style             = smsGrid; // snug would turn the interface lines by 45 degrees
    support_params.interface_angle           = float(angle + 0.5 * M_PI);
    support_params.top_interface_spacing     = support_params.support_material_interface_flow.spacing();
    support_params.top_interface_density     = 1.;
    support_params.interface_fill_pattern    = ipRectilinear;
    const coordf_t top_gap_below    = config.support_top_z_distance.value;

    std::vector<Polygons> shaped(num_layers);
    std::vector<Polygons> interfaces(num_layers);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, num_layers), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t i = range.begin(); i < range.end(); ++ i) {
            if (regions[i].empty())
                continue;
            const Layer &layer = *layers[i];
            const bool   foot  = layer.bottom_z() < kBaseHeight - EPSILON;
            Polygons     out   = opening(intersection(regions[i], foot ? feet : fins), min_width);

            // 4. Tines. The first object layer, which sits on the bed, never gets them.
            if (! out.empty() && tine_row[i]) {
                const float    gap           = float(scale_(support_params.gap_xy));
                const Polygons object_slices = to_polygons(layer.lslices);
                // Ring from `depth` inside the object surface out to just past the XY gap.
                const Polygons ring      = diff(offset(object_slices, gap + float(spacing)), offset(object_slices, - depth));
                Polygons       tines     = intersection(intersection(tine_strips, ring), offset(out, gap + depth + float(spacing)));
                const Polygons fin_touch = offset(out, float(SCALED_EPSILON));
                tines.erase(std::remove_if(tines.begin(), tines.end(), [&](const Polygon &tine) {
                    // A tine must bridge from its fin into the object; one that misses either end
                    // (e.g. a short piece cut off at a corner of the ring) prints in mid-air.
                    if (intersection(Polygons{ tine }, fin_touch).empty() || intersection(Polygons{ tine }, object_slices).empty())
                        return true;
                    // A fin running along the object surface would leave a long strip inside the ring:
                    // a welded seam, not a tine. Crossings down to about 20 degrees are kept.
                    Polygon along = tine;
                    along.rotate(-angle);
                    const BoundingBox b = along.bounding_box();
                    return double(b.max.x() - b.min.x()) > 3. * (gap + depth + float(spacing));
                }), tines.end());
                if (! tines.empty())
                    out = union_(out, tines);
            }

            // 5. Interface: the part of the fin region within interface_layers layers under the object
            // (above the top Z gap) becomes a solid deck; the fins stop under it.
            if (interface_layers > 0) {
                Polygons over;
                for (size_t j = i + 1; j < num_layers && layers[j]->bottom_z() < layer.print_z + top_gap_below + interface_layers * layer.height - EPSILON; ++ j)
                    append(over, outlines[j]);
                Polygons deck = opening(intersection(regions[i], over), min_width);
                if (! deck.empty()) {
                    out           = diff(out, deck);
                    interfaces[i] = std::move(deck);
                }
            }
            shaped[i] = std::move(out);
        }
    });
    regions.clear();

    // A fin much taller than it is long is a free-standing blade that can wobble or snap mid-print,
    // e.g. under a small feature high above the bed. Measure each fin above the foot.
    bool slender = false;
    {
        size_t first = 0;
        while (first < num_layers && (shaped[first].empty() || layers[first]->bottom_z() < kBaseHeight - EPSILON))
            ++ first;
        if (first < num_layers)
            for (const ExPolygon &fin : union_ex(shaped[first])) {
                Polygon along = fin.contour;
                along.rotate(-angle);
                const BoundingBox b      = along.bounding_box();
                // A braced fin is stiff in both directions, so its shorter side is the one that counts.
                const coord_t     xlen   = b.max.x() - b.min.x();
                const coord_t     ylen   = b.max.y() - b.min.y();
                const coord_t     length = cross_pitch > 0 && ylen > 2 * thickness ? std::min(xlen, ylen) : xlen;
                const Polygons    column = offset(fin, float(spacing));
                size_t            top    = first;
                while (top + 1 < num_layers && ! intersection(shaped[top + 1], column).empty())
                    ++ top;
                if (layers[top]->print_z > kSlenderRatio * unscale<double>(length))
                    slender = true;
            }
    }

    // Hand the fins to the common support pipeline as base layers, one per object layer.
    SupportGeneratorLayerStorage layer_storage;
    SupportGeneratorLayersPtr    base_layers;
    SupportGeneratorLayersPtr    interface_layers_out;
    for (size_t i = 0; i < num_layers; ++ i)
        if (! interfaces[i].empty()) {
            SupportGeneratorLayer &support_layer = layer_storage.allocate_unguarded(SupporLayerType::TopInterface);
            support_layer.print_z  = layers[i]->print_z;
            support_layer.bottom_z = layers[i]->bottom_z();
            support_layer.height   = layers[i]->height;
            support_layer.polygons = std::move(interfaces[i]);
            // The lowest deck layer spans the gaps between fins.
            support_layer.bridging = i == 0 || interface_layers_out.empty() || interface_layers_out.back()->print_z < layers[i - 1]->print_z - EPSILON;
            interface_layers_out.push_back(&support_layer);
        }
    for (size_t i = 0; i < num_layers; ++ i)
        if (! shaped[i].empty()) {
            SupportGeneratorLayer &support_layer = layer_storage.allocate_unguarded(SupporLayerType::Base);
            support_layer.print_z  = layers[i]->print_z;
            support_layer.bottom_z = layers[i]->bottom_z();
            support_layer.height   = layers[i]->height;
            support_layer.polygons = std::move(shaped[i]);
            base_layers.push_back(&support_layer);
        }
    if (base_layers.empty() && interface_layers_out.empty())
        return false;

    const SupportGeneratorLayersPtr empty;
    generate_support_layers(object, empty, empty, empty, base_layers, interface_layers_out, empty);
    generate_support_toolpaths(object.support_layers(), config, support_params, slicing_params, empty, empty, empty, base_layers, interface_layers_out, empty);

    BOOST_LOG_TRIVIAL(info) << "Fin support - end";
    return slender;
}

} // namespace Slic3r
