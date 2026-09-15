#include "BeltBrim.hpp"

#include "ClipperUtils.hpp"
#include "Flow.hpp"
#include "Layer.hpp"
#include "Polygon.hpp"
#include "Print.hpp"
#include "ShortestPath.hpp"
#include "Support/BeltFloorContext.hpp"

#include <algorithm>

namespace Slic3r {

// ---------------------------------------------------------------- scaling

static inline Point scale_u_point(const Point &p, int from_axis, double factor)
{
    // llround, not a cast: casting truncates toward zero, so a round trip would
    // walk every vertex toward the origin by up to one unit per pass.
    return from_axis == 0 ?
        Point(coord_t(std::llround(double(p.x()) * factor)), p.y()) :
        Point(p.x(), coord_t(std::llround(double(p.y()) * factor)));
}

static inline void scale_u_polygon(Polygon &poly, int from_axis, double factor)
{
    for (Point &p : poly.points)
        p = scale_u_point(p, from_axis, factor);
}

ExPolygons belt_scale_u(const ExPolygons &src, const BeltBrimFrame &frame, double factor)
{
    ExPolygons out = src;
    for (ExPolygon &ex : out) {
        scale_u_polygon(ex.contour, frame.from_axis, factor);
        for (Polygon &hole : ex.holes)
            scale_u_polygon(hole, frame.from_axis, factor);
    }
    return out;
}

Polylines belt_scale_u(const Polylines &src, const BeltBrimFrame &frame, double factor)
{
    Polylines out = src;
    for (Polyline &pl : out)
        for (Point &p : pl.points)
            p = scale_u_point(p, frame.from_axis, factor);
    return out;
}

// ---------------------------------------------------------------- sweep

ExPolygons sweep_ex(const ExPolygons &src, const Point &t)
{
    if (src.empty())
        return {};
    if (t == Point(0, 0))
        return src;

    // One parallelogram per boundary edge.  Together with P and P + t these
    // cover the Minkowski sum exactly: for any q = p + s*t with p in P and
    // s in [0, 1], let s* be the smallest lambda >= 0 with q - lambda*t in P.
    // Either s* == 0 (so q is in P) or q - s* * t lies on some boundary edge e,
    // putting q in that edge's parallelogram.  Hole edges must be included, or
    // holes narrower than t along t would wrongly survive the sweep.
    Polygons quads;
    for (const ExPolygon &ex : src)
        for (size_t c = 0; c < ex.num_contours(); ++ c)
            for (const Line &e : ex.contour_or_hole(c).lines()) {
                if (e.a == e.b)
                    continue;
                Polygon q;
                q.points = { e.a, e.b, e.b + t, e.a + t };
                // The non-zero fill rule counts a clockwise ring as -1, which
                // would punch a hole instead of adding material.  Edges parallel
                // to t give a zero-area quad; Clipper discards those harmlessly.
                if (q.is_clockwise())
                    q.reverse();
                quads.emplace_back(std::move(q));
            }

    ExPolygons shifted = src;
    for (ExPolygon &ex : shifted)
        ex.translate(t);

    // union_ex(ExPolygons, Polygons) uses pftNonZero, which is the fill rule the
    // argument above relies on.
    return union_ex(union_ex(src, shifted), quads);
}

// ---------------------------------------------------------------- brim region

ExPolygons belt_brim_region(const ExPolygons    &footprint_flat,
                            bool                 has_outer,
                            bool                 has_inner,
                            coord_t              brim_width,
                            coord_t              object_gap,
                            coord_t              leading,
                            coord_t              lateral,
                            const BeltBrimFrame &frame)
{
    if (footprint_flat.empty() || (! has_outer && ! has_inner))
        return {};

    ExPolygons out;

    if (has_outer) {
        // Offset the outer ring from the contours only, so a hole cannot punch
        // through it.  Same reasoning as the plate brim in Brim.cpp.
        Polygons contours;
        contours.reserve(footprint_flat.size());
        for (const ExPolygon &ex : footprint_flat)
            contours.emplace_back(ex.contour);

        // Inner and outer boundary offset from the same polygon, to avoid
        // round-off mismatch between them.
        ExPolygons inner = offset_ex(contours, float(object_gap), jtRound, SCALED_RESOLUTION);

        // Close the interior before offsetting outwards.  A belt contact patch is often a
        // narrow, broken-up strip, and the offset rings of two islands less than
        // 2 x brim_width apart merge and fill the space between them - space that lies
        // UNDER the part, which is not what "outer brim" means.  Closing also swallows
        // holes in the patch for the same reason.  Concavity-filling only, so an apron or
        // any other outward protrusion is untouched.
        ExPolygons envelope = brim_width > 0 ? closing_ex(inner, float(brim_width)) : inner;

        ExPolygons base  = envelope;
        if (leading > 0) {
            // Sweep downhill from the gapped keep-out, so the apron is contiguous with
            // the ring instead of starting inside the gap.
            const Point t = frame.from_axis == 0 ?
                Point(frame.downhill_sign() * leading, 0) :
                Point(0, frame.downhill_sign() * leading);
            base = union_ex(base, sweep_ex(envelope, t));
        }
        if (lateral > 0) {
            // Across the belt, both ways.  Swept from `base` so the apron is widened
            // too, and in the flattened frame the cross-belt axis is unscaled, so this
            // distance is already a true on-belt distance.
            const Point t = frame.from_axis == 0 ? Point(0, lateral) : Point(lateral, 0);
            ExPolygons widened = union_ex(sweep_ex(base, t), sweep_ex(base, Point(-t.x(), -t.y())));
            base = union_ex(base, to_polygons(widened));
        }
        ExPolygons outer = offset_ex(base, float(brim_width), jtRound, SCALED_RESOLUTION);
        expolygons_append(out, diff_ex(outer, envelope));
    }

    if (has_inner) {
        // Holes reversed so a negative offset grows inward, mirroring Brim.cpp.
        // No apron here: an apron growing into a hole interior is never useful.
        Polygons holes;
        for (const ExPolygon &ex : footprint_flat)
            polygons_append(holes, ex.holes);
        polygons_reverse(holes);
        if (! holes.empty()) {
            ExPolygons hole_inner = offset_ex(holes, - float(brim_width + object_gap));
            ExPolygons hole_outer = offset_ex(holes, - float(object_gap));
            expolygons_append(out, intersection_ex(diff_ex(hole_outer, hole_inner), holes));
        }
    }

    return union_ex(out);
}

// ---------------------------------------------------------------- line lattice

std::vector<coord_t> belt_brim_line_positions(coord_t u_lo,
                                              coord_t u_hi,
                                              coord_t pitch_u,
                                              coord_t u_anchor)
{
    std::vector<coord_t> out;
    if (pitch_u <= 0 || u_hi <= u_lo)
        return out;

    // Walk the lattice from just below u_lo.  Integer arithmetic throughout, so
    // the half-open interval needs no epsilon: a point landing exactly on u_hi
    // belongs to the next band.
    int64_t k = int64_t(std::floor(double(u_lo - u_anchor) / double(pitch_u))) - 1;
    while (u_anchor + coord_t(k) * pitch_u < u_lo)
        ++ k;
    for (;; ++ k) {
        const coord_t u = u_anchor + coord_t(k) * pitch_u;
        if (u >= u_hi)
            break;
        out.emplace_back(u);
    }
    return out;
}

// ---------------------------------------------------------------- pipeline

// A band of the belt surface as an explicit box, clamped to `bounds` along the
// shear axis.  Deliberately not BeltFloorContext::surface_polygon(): those
// half-planes span +-1000 mm, which is wasteful to clip against and dangerous to
// feed through the flattening scale.
static Polygon band_box(const BoundingBox &bounds, int from_axis, coordf_t u_lo, coordf_t u_hi)
{
    coord_t lo = scale_(u_lo);
    coord_t hi = scale_(u_hi);
    const coord_t bmin = from_axis == 0 ? bounds.min.x() : bounds.min.y();
    const coord_t bmax = from_axis == 0 ? bounds.max.x() : bounds.max.y();
    lo = std::max(lo, bmin);
    hi = std::min(hi, bmax);
    Polygon poly;
    if (hi <= lo)
        return poly;
    if (from_axis == 0)
        poly.points = { Point(lo, bounds.min.y()), Point(hi, bounds.min.y()),
                        Point(hi, bounds.max.y()), Point(lo, bounds.max.y()) };
    else
        poly.points = { Point(bounds.min.x(), lo), Point(bounds.max.x(), lo),
                        Point(bounds.max.x(), hi), Point(bounds.min.x(), hi) };
    return poly;
}

// Everything the per-band line generator needs, gathered once per object.
struct BeltBrimContext
{
    BeltFloorContext ctx;
    BeltBrimFrame    frame;
    ExPolygons       region;      // brim region, object-local slicing XY
    BoundingBox      region_bbox;
    Flow             brim_flow;
    coord_t          pitch_u   = 0;
    coord_t          u_anchor  = 0;
    double           in_plane_pitch = 0.;   // mm
};

// Emit the cross-belt brim lines that belong to the band [print_z - height, print_z].
static void belt_brim_band_paths(const BeltBrimContext      &bc,
                                 coordf_t                    print_z,
                                 coordf_t                    height,
                                 const Polygons             &obstacles,
                                 ExtrusionEntityCollection  &out,
                                 ExPolygons                 &areas_out)
{
    coordf_t u_lo = bc.ctx.cutoff_u(print_z - height);
    coordf_t u_hi = bc.ctx.cutoff_u(print_z);
    if (u_lo > u_hi)
        std::swap(u_lo, u_hi);

    // How wide this band is measured ON the belt, versus one nominal bead.
    const double band_in_plane = (u_hi - u_lo) * bc.frame.u_stretch();

    // Fraction of the layer height at which a line sits above the belt.  Toward the
    // downhill edge, so the sheet is reasonably thick while the nozzle stays clear of
    // the belt itself.
    static constexpr double BAND_CLEARANCE_FRACTION = 0.75;

    std::vector<coord_t> us;
    double uniform_clearance = 0.;   // 0 => derive per line from its own position
    double line_pitch        = bc.in_plane_pitch;
    if (band_in_plane <= bc.in_plane_pitch + EPSILON) {
        // Steep belt, which is the normal case: the band is narrower than one bead, so
        // exactly one line fits.  Place it at a FIXED fraction of the band rather than
        // on a nominal-spacing lattice.  On a lattice each line lands at an arbitrary
        // point in its band, the clearance sweeps [0, height] from band to band, and the
        // bead width therefore varies by 2x - visible as ragged, uneven brim lines.
        // Anchoring to the band makes the clearance identical everywhere, so every bead
        // is the same width.
        //
        // The spacing is then whatever the bands give (height / sin(tilt) on the belt)
        // rather than the nominal bead spacing, so the flow below is matched to THAT
        // pitch.  Matched flow at the real pitch is what keeps the sheet uniform and
        // gap-free; using nominal flow at band spacing would over-feed it.
        us.push_back(scale_(bc.ctx.cutoff_u(print_z - BAND_CLEARANCE_FRACTION * height)));
        uniform_clearance = BAND_CLEARANCE_FRACTION * height;
        line_pitch        = band_in_plane;
    } else {
        // Shallow belt: the band is wider than a bead, so it takes several lines and they
        // have to sit on the nominal lattice.  Their clearances then differ, and so do
        // their widths - unavoidable here, but shallow belts are the rare case.
        us = belt_brim_line_positions(scale_(u_lo), scale_(u_hi), bc.pitch_u, bc.u_anchor);
    }
    if (us.empty())
        return;

    const Polygons region_polys = to_polygons(bc.region);

    // One lattice line at a time: the clearance - and therefore the extrusion
    // volume - is a property of the line's u, so the pieces of different lines
    // must not be pooled before the flow is resolved.
    // Overshoot the region so the clip, not the line's ends, decides the extent.
    const coord_t margin = coord_t(SCALED_EPSILON) + 1;
    for (const coord_t u : us) {
        Polyline line;
        if (bc.frame.from_axis == 0)
            line.points = { Point(u, coord_t(bc.region_bbox.min.y() - margin)),
                            Point(u, coord_t(bc.region_bbox.max.y() + margin)) };
        else
            line.points = { Point(coord_t(bc.region_bbox.min.x() - margin), u),
                            Point(coord_t(bc.region_bbox.max.x() + margin), u) };

        Polylines pieces = intersection_pl(Polylines{ line }, region_polys);
        if (! obstacles.empty())
            pieces = diff_pl(pieces, obstacles);
        if (pieces.empty())
            continue;

        // Nozzle-to-belt clearance for this line.  Constant along the line, because the
        // belt height depends only on the shear-axis coordinate.  Band-anchored lines
        // share one clearance by construction; lattice lines (shallow belts) each get
        // their own, clamped so neither end of a band yields an unprintable bead.
        double clearance = uniform_clearance;
        if (clearance <= 0.) {
            const Point probe = bc.frame.from_axis == 0 ? Point(u, 0) : Point(0, u);
            clearance = print_z - bc.ctx.floor_print_z(probe);
            clearance = std::min(std::max(clearance, 0.5 * height), height);
        }

        // with_cross_section, not with_height: it reaches the prescribed volume while
        // KEEPING the extrusion spacing, so the bead is sized to fill exactly one
        // pitch x clearance cell of the sheet.
        const Flow f = bc.brim_flow.with_cross_section(float(line_pitch * clearance));

        // Footprint of these beads, for the first-layer convex hull and bbox.
        for (const Polygon &p : offset(pieces, 0.5f * float(f.scaled_width())))
            areas_out.emplace_back(ExPolygon(p));

        extrusion_entities_append_paths(out.entities, chain_polylines(std::move(pieces)),
                                        erBrim, f.mm3_per_mm(), f.width(), float(clearance));
    }
}

// Union of everything extruded at `print_z` that the brim must keep clear of, expressed
// in `self`'s local slicing frame.  Includes `self` itself: its slice at this Z can
// overhang outside the belt footprint and land in the brim ring, which the flattened
// brim_object_gap - a belt-plane separation - does not cover.
//
// THREADING: this runs inside posSupportMaterial, which Print::process() executes for all
// objects in a tbb::parallel_for (Print.cpp).  Object slices are finished by then and safe
// to read across objects, but SUPPORT layers are not: another object's thread may be
// inside clear_support_layers() - which deletes the SupportLayer pointers - right now, so
// touching a foreign object's support_layers() here is a use-after-free.  Only this
// object's own supports are consulted; they are complete, because make_belt_brim() runs at
// the tail of this object's own generate_support_material().  The cost is that the brim
// does not dodge a *different* object's support at the same Z, which needs the objects to
// overlap in the belt direction in the first place.
// `region_bbox` bounds the brim; anything outside it cannot clip a brim line, so whole
// objects are skipped without materialising their polygons.  On a typical plate the
// objects do not overlap and every foreign object drops out here, which matters because
// this runs once per band - hundreds of times per object.
static Polygons belt_brim_obstacles(const Print &print, const PrintObject &self,
                                    const BoundingBox &region_bbox, coordf_t print_z, coordf_t tol)
{
    const Point shift_self = self.instances().empty() ? Point(0, 0)
                                                      : self.instances().front().shift_without_plate_offset();
    Polygons out;
    for (const PrintObject *o : print.objects()) {
        const bool is_self = (o == &self);
        for (const PrintInstance &inst : o->instances()) {
            const Point delta = inst.shift_without_plate_offset() - shift_self;
            if (const Layer *l = o->get_layer_at_printz(print_z, tol)) {
                BoundingBox lb = get_extents(l->lslices);
                lb.translate(delta.x(), delta.y());
                if (lb.overlap(region_bbox)) {
                    Polygons ps = to_polygons(l->lslices);
                    for (Polygon &p : ps)
                        p.translate(delta);
                    polygons_append(out, std::move(ps));
                }
            }
            if (! is_self)
                continue;
            if (const SupportLayer *sl = o->get_support_layer_at_printz(print_z, tol)) {
                Polygons ps = sl->support_fills.polygons_covered_by_spacing();
                for (Polygon &p : ps)
                    p.translate(delta);
                polygons_append(out, std::move(ps));
            }
        }
    }
    if (out.size() < 2)
        return out;   // union_() of 0 or 1 polygons is pure overhead
    return union_(out);
}

void make_belt_brim(PrintObject &object)
{
    object.clear_belt_brim();
    if (! object.has_belt_brim())
        return;

    const Print &print = *object.print();
    BeltBrimContext bc;
    if (! bc.ctx.init(object.slicing_parameters(), print.config()))
        return;
    bc.frame = BeltBrimFrame{ bc.ctx.shear_factor(), bc.ctx.from_axis() };

    const size_t nlayers = object.layers().size();
    if (nlayers == 0)
        return;

    // 1. Belt footprint: the union of each layer's slice clipped to that layer's
    //    own contact band.  This is the object's bottom face, which on a belt is
    //    spread over every layer instead of sitting in layer 0.
    ExPolygons footprint_acc;
    for (size_t i = 0; i < nlayers; ++ i) {
        const Layer &layer = *object.layers()[i];
        if (layer.lslices.empty())
            continue;
        // print_z - height, not the previous layer's print_z: variable layer
        // heights make the latter wrong.
        coordf_t u_lo = bc.ctx.cutoff_u(layer.print_z - layer.height);
        coordf_t u_hi = bc.ctx.cutoff_u(layer.print_z);
        if (u_lo > u_hi)
            std::swap(u_lo, u_hi);
        BoundingBox bb = get_extents(layer.lslices);
        bb.offset(scale_(1.));
        const Polygon band = band_box(bb, bc.frame.from_axis, u_lo, u_hi);
        if (band.empty())
            continue;
        expolygons_append(footprint_acc, intersection_ex(layer.lslices, Polygons{ band }));
    }
    const ExPolygons footprint = union_ex(footprint_acc);
    if (footprint.empty())
        return;

    // 2. Brim region, offset in the flattened (true on-belt) metric.
    const PrintObjectConfig &cfg = object.config();
    bc.brim_flow = print.brim_flow();
    const double  flow_w = bc.brim_flow.scaled_spacing() * SCALING_FACTOR;
    // Quantize to an even number of lines, as the plate brim does.
    const coord_t width   = scale_(std::floor(cfg.brim_width.value / flow_w / 2) * flow_w * 2);
    const coord_t leading = scale_(cfg.leading_brim_length.value);
    const coord_t lateral = scale_(cfg.extra_brim_width.value);
    const coord_t gap     = scale_(cfg.brim_object_gap.value);

    // Belt printers collapse Auto / Mouse ear / Painted to outer-only: the auto width
    // heuristic and flat ear discs have no meaning on a tilted plane.  Leading-edge-only
    // is an outer brim too; it is narrowed down to the first contact below.
    const BrimType bt = cfg.brim_type.value;
    const bool has_outer = bt == btOuterOnly || bt == btOuterAndInner
                        || bt == btAutoBrim  || bt == btEar || bt == btPainted
                        || bt == btLeadingEdgeOnly;
    const bool has_inner = bt == btInnerOnly || bt == btOuterAndInner;

    bc.region = belt_unflatten(
        belt_brim_region(belt_flatten(footprint, bc.frame), has_outer, has_inner,
                         width, gap, leading, lateral, bc.frame),
        bc.frame);

    if (bt == btLeadingEdgeOnly && ! bc.region.empty()) {
        // Keep only what lies at or downhill of the object's FIRST contact with the
        // belt, so the part is supported as it lands and nothing is printed alongside
        // it afterwards.  The cut is the uphill edge of the first layer's contact band:
        // everything past it belongs to later contacts.
        const coordf_t u_cut   = bc.ctx.cutoff_u(object.layers().front()->print_z);
        BoundingBox    keep_bb = get_extents(bc.region);
        keep_bb.offset(scale_(1.));
        const bool     low_side = bc.frame.shear > 0.;   // downhill is -u
        const Polygon  keep = band_box(keep_bb, bc.frame.from_axis,
            low_side ? unscale<double>(bc.frame.from_axis == 0 ? keep_bb.min.x() : keep_bb.min.y()) : u_cut,
            low_side ? u_cut : unscale<double>(bc.frame.from_axis == 0 ? keep_bb.max.x() : keep_bb.max.y()));
        bc.region = keep.empty() ? ExPolygons{} : intersection_ex(bc.region, Polygons{ keep });
    }

    if (bc.region.empty())
        return;
    bc.region_bbox = get_extents(bc.region);

    // 3. Line lattice.  Fixed pitch in the flattened metric, anchored at the
    //    footprint's leading-most edge so lines stay collinear across
    //    disconnected islands and across the apron prologue.
    bc.pitch_u = std::max<coord_t>(1, coord_t(bc.brim_flow.scaled_spacing() * bc.frame.cos_tilt()));
    bc.in_plane_pitch = unscale<double>(bc.pitch_u) * bc.frame.u_stretch();
    {
        const BoundingBox fbb = get_extents(footprint);
        const bool low_side = bc.frame.shear > 0.;
        bc.u_anchor = bc.frame.from_axis == 0 ? (low_side ? fbb.min.x() : fbb.max.x())
                                              : (low_side ? fbb.min.y() : fbb.max.y());
    }

    // 4. Bands coincident with an object layer.
    std::vector<ExtrusionEntityCollection> by_layer(nlayers);
    std::vector<ExPolygons>                areas_by_layer(nlayers);
    for (size_t i = 0; i < nlayers; ++ i) {
        const Layer &layer = *object.layers()[i];
        const Polygons obstacles = belt_brim_obstacles(print, object, bc.region_bbox, layer.print_z, 0.5 * layer.height);
        belt_brim_band_paths(bc, layer.print_z, layer.height, obstacles, by_layer[i], areas_by_layer[i]);
    }

    // 5. Apron prologue: the part of the region downhill of the object's first
    //    layer, which has no object layer to ride on.
    std::vector<BeltBrimBand> prologue;
    {
        const Layer   &first  = *object.layers().front();
        const coordf_t h      = first.height;
        const bool     low_side = bc.frame.shear > 0.;
        const coord_t  u_lead_s = bc.frame.from_axis == 0
            ? (low_side ? bc.region_bbox.min.x() : bc.region_bbox.max.x())
            : (low_side ? bc.region_bbox.min.y() : bc.region_bbox.max.y());
        const coordf_t u_lead  = unscale<double>(u_lead_s);
        // print_z at which the belt surface crosses the region's leading edge.
        const coordf_t z_lead  = bc.ctx.shear_factor() * u_lead
                               + bc.ctx.floor_offset() + bc.ctx.z_shift();
        if (h > EPSILON)
            for (coordf_t z = first.print_z - h; z > z_lead - h; z -= h) {
                const Polygons obstacles = belt_brim_obstacles(print, object, bc.region_bbox, z, 0.5 * h);
                BeltBrimBand band;
                band.print_z = z;
                band.height  = h;
                belt_brim_band_paths(bc, z, h, obstacles, band.fills, band.areas);
                if (! band.fills.empty())
                    prologue.emplace_back(std::move(band));
            }
        // Lowest Z first, so collect_layers_to_print sees them in print order.
        std::reverse(prologue.begin(), prologue.end());
    }

    object.set_belt_brim(std::move(by_layer), std::move(areas_by_layer), std::move(prologue));
}

} // namespace Slic3r
