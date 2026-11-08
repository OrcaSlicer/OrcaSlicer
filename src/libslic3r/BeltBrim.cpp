#include "BeltBrim.hpp"

#include "ClipperUtils.hpp"
#include "Flow.hpp"
#include "Geometry.hpp"
#include "Layer.hpp"
#include "Polygon.hpp"
#include "Print.hpp"
#include "ShortestPath.hpp"

#include <algorithm>
#include <boost/log/trivial.hpp>

namespace Slic3r {

// ---------------------------------------------------------------- scaling

static inline Point scale_u_point(const Point &p, int from_axis, double factor)
{
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

    // One parallelogram per boundary edge. Together with P and P + t these cover the Minkowski
    // sum exactly. Hole edges must be included, or holes narrower than t along t would wrongly
    // survive the sweep.
    Polygons quads;
    for (const ExPolygon &ex : src)
        for (size_t c = 0; c < ex.num_contours(); ++ c)
            for (const Line &e : ex.contour_or_hole(c).lines()) {
                if (e.a == e.b)
                    continue;
                Polygon q;
                q.points = { e.a, e.b, e.b + t, e.a + t };
                // The non-zero fill rule counts a clockwise ring as -1, which would punch a hole.
                if (q.is_clockwise())
                    q.reverse();
                quads.emplace_back(std::move(q));
            }

    ExPolygons shifted = src;
    for (ExPolygon &ex : shifted)
        ex.translate(t);

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
        // Offset the outer ring from the contours only, so a hole cannot punch through it.
        Polygons contours;
        contours.reserve(footprint_flat.size());
        for (const ExPolygon &ex : footprint_flat)
            contours.emplace_back(ex.contour);

        ExPolygons inner = offset_ex(contours, float(object_gap), jtRound, SCALED_RESOLUTION);

        // Close the interior before offsetting outwards: a belt contact patch is often a narrow,
        // broken-up strip, and the offset rings of two islands less than 2 x brim_width apart
        // would merge and fill the space UNDER the part. Concavity-filling only.
        ExPolygons envelope = brim_width > 0 ? closing_ex(inner, float(brim_width)) : inner;

        ExPolygons base  = envelope;
        if (leading > 0) {
            const Point t = frame.from_axis == 0 ?
                Point(frame.downhill_sign() * leading, 0) :
                Point(0, frame.downhill_sign() * leading);
            base = union_ex(base, sweep_ex(envelope, t));
        }
        if (lateral > 0) {
            const Point t = frame.from_axis == 0 ? Point(0, lateral) : Point(lateral, 0);
            ExPolygons widened = union_ex(sweep_ex(base, t), sweep_ex(base, Point(-t.x(), -t.y())));
            base = union_ex(base, to_polygons(widened));
        }
        ExPolygons outer = offset_ex(base, float(brim_width), jtRound, SCALED_RESOLUTION);
        expolygons_append(out, diff_ex(outer, envelope));
    }

    if (has_inner) {
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

std::vector<coord_t> belt_brim_line_positions(coord_t u_lo, coord_t u_hi, coord_t pitch_u, coord_t u_anchor)
{
    std::vector<coord_t> out;
    if (pitch_u <= 0 || u_hi <= u_lo)
        return out;
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

// ---------------------------------------------------------------- frame

bool belt_brim_frame(const PrintObject &object, BeltBrimFrame &frame)
{
    const Print &print = *object.print();
    if (! print.config().belt_printer.value || ! object.support_floor_is_tilted())
        return false;
    // The belt plane over layer-local XY: the G-code stage evaluates it on plate coordinates
    // (layer XY + instance shift), and bed_plane_in_slice_frame() folds that shift into k.
    Vec3d  n;
    double k;
    object.bed_plane_in_slice_frame(n, k);
    // The belt height must depend on one slice-XY axis only, and vary with Z at all.
    const double ax = std::abs(n.x()), ay = std::abs(n.y()), az = std::abs(n.z());
    if (az < 1e-6 || (ax > 1e-6 && ay > 1e-6))
        return false;
    const int    from_axis = ax > ay ? 0 : 1;
    const double n_u       = from_axis == 0 ? n.x() : n.y();
    if (std::abs(n_u) < 1e-6)
        return false;
    // bed_z = n_u u + n_z z + k = 0  ->  z = -(n_u u + k) / n_z
    frame.from_axis = from_axis;
    frame.shear     = - n_u / n.z();
    frame.z0        = - k / n.z();
    const double tilt_deg = Geometry::rad2deg(std::atan(std::abs(frame.shear)));
    return tilt_deg >= BELT_BRIM_MIN_TILT_DEG && tilt_deg <= BELT_BRIM_MAX_TILT_DEG;
}

// ---------------------------------------------------------------- pipeline

// A band of the belt surface as an explicit box, clamped to `bounds` along the shear axis.
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

struct BeltBrimContext
{
    BeltBrimFrame frame;
    ExPolygons    region;      // brim region, object-local slicing XY
    BoundingBox   region_bbox;
    // A placeholder: Flow's constructor computes the spacing and rejects a zero bead, so it cannot
    // be default-constructed; make_belt_brim() replaces it with the print's brim flow.
    Flow          brim_flow { 0.4f, 0.2f, 0.4f };
    coord_t       pitch_u   = 0;
    coord_t       u_anchor  = 0;
    double        in_plane_pitch = 0.;   // mm
};

// Emit the cross-belt brim lines that belong to the band [print_z - height, print_z].
static void belt_brim_band_paths(const BeltBrimContext      &bc,
                                 coordf_t                    print_z,
                                 coordf_t                    height,
                                 const Polygons             &obstacles,
                                 ExtrusionEntityCollection  &out,
                                 ExPolygons                 &areas_out)
{
    coordf_t u_lo = bc.frame.cutoff_u(print_z - height);
    coordf_t u_hi = bc.frame.cutoff_u(print_z);
    if (u_lo > u_hi)
        std::swap(u_lo, u_hi);

    const double band_in_plane = (u_hi - u_lo) * bc.frame.u_stretch();

    // Fraction of the layer height at which a line sits above the belt: toward the downhill edge,
    // so the sheet is reasonably thick while the nozzle stays clear of the belt itself.
    static constexpr double BAND_CLEARANCE_FRACTION = 0.75;

    std::vector<coord_t> us;
    double uniform_clearance = 0.;
    double line_pitch        = bc.in_plane_pitch;
    if (band_in_plane <= bc.in_plane_pitch + EPSILON) {
        // Steep belt, the normal case: one line per band, at a FIXED fraction of the band so every
        // bead has the same clearance and width; the flow is matched to the band pitch.
        us.push_back(scale_(bc.frame.cutoff_u(print_z - BAND_CLEARANCE_FRACTION * height)));
        uniform_clearance = BAND_CLEARANCE_FRACTION * height;
        line_pitch        = band_in_plane;
    } else {
        // Shallow belt: several lines on the nominal lattice.
        us = belt_brim_line_positions(scale_(u_lo), scale_(u_hi), bc.pitch_u, bc.u_anchor);
    }
    if (us.empty())
        return;

    const Polygons region_polys = to_polygons(bc.region);
    const coord_t  margin       = coord_t(SCALED_EPSILON) + 1;
    coord_t u_prev = std::numeric_limits<coord_t>::min();
    for (coord_t u : us) {
        // Nozzle-to-belt clearance for this line: constant along it (the belt height depends on
        // u alone). Lattice lines each get their own. A lattice line may fall where the belt is
        // only a hair below the band's print_z; such a bead would be laid scraping the belt (and
        // the writer refuses it as "very low z"), so it is moved uphill to the same fraction of the
        // band the single-line case uses. Note the clearance is measured along slice z; the true
        // gap under the nozzle is clearance x cos(tilt), 0.53 h at 45 degrees for the 0.75 fraction,
        // which is why half the band (0.35 h, under 0.1 mm for a 0.28 mm layer) is not enough. Found
        // by fuzzing: every 0.28-0.3 mm first layer put its bands on the lattice and lost its brim.
        double clearance = uniform_clearance;
        if (clearance <= 0.) {
            clearance = print_z - bc.frame.floor_z(unscale<double>(u));
            if (clearance < BAND_CLEARANCE_FRACTION * height) {
                clearance = BAND_CLEARANCE_FRACTION * height;
                u         = scale_(bc.frame.cutoff_u(print_z - clearance));
            }
            clearance = std::min(clearance, height);
        }
        if (u == u_prev)
            continue;
        u_prev = u;

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

        // The bead fills one pitch x clearance cell of the sheet: that volume per mm, at the pitch
        // as its width and the clearance as its height. Stated directly rather than through
        // Flow::with_cross_section(), whose spacing arithmetic rejects beads thinner than the nozzle.
        const double mm3_per_mm = line_pitch * clearance;
        const double bead_width = std::max(line_pitch, 0.5 * double(bc.brim_flow.nozzle_diameter()));

        for (const Polygon &p : offset(pieces, 0.5f * float(scale_(bead_width))))
            areas_out.emplace_back(ExPolygon(p));

        extrusion_entities_append_paths(out.entities, chain_polylines(std::move(pieces)),
                                        erBrim, float(mm3_per_mm), float(bead_width), float(clearance));
    }
}

// Union of everything extruded at `print_z` that the brim must keep clear of, in `self`'s local
// slice frame. Includes `self` itself (its slice at this Z can overhang into the ring).
//
// THREADING: runs inside posSupportMaterial, executed for all objects in parallel. Object slices
// are finished by then and safe to read across objects, but SUPPORT layers are not: another
// object's thread may be inside clear_support_layers() right now. Only this object's own supports
// are consulted; they are complete, because make_belt_brim() runs at the tail of this object's own
// generate_support_material().
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
        return out;
    return union_(out);
}

// ---------------------------------------------------------------- PrintObject

bool PrintObject::has_belt_brim() const
{
    if (! m_print->has_tilted_belt() || this->is_segment())
        return false;
    // The purge prism is sacrificial and sits at the plate's edge; its generator sets no_brim, and
    // this keeps it brimless whatever its config says, so a brim on the parts never blocks purging.
    if (m_config.belt_purge_tower_object.value)
        return false;
    if (! this->belt_brim_instances_compatible())
        return false;
    if (m_config.brim_type == btNoBrim)
        return false;
    // An inner-only brim has no leading/extra geometry (both widen the OUTER ring), so it needs
    // a positive brim_width; every other type is satisfied by any one of the three widths.
    if (m_config.brim_type == btInnerOnly) {
        if (m_config.brim_width.value <= 0.)
            return false;
    } else if (m_config.brim_width.value <= 0. && m_config.leading_brim_length.value <= 0.
               && m_config.extra_brim_width.value <= 0.) {
        return false;
    }
    return ! this->has_raft();
}

unsigned int PrintObject::belt_brim_filament() const
{
    // Lowest positive outer-wall filament over the printing regions; 1 when none is set.
    unsigned int brim_filament = 0;
    for (size_t i = 0; i < this->num_printing_regions(); ++ i) {
        const unsigned int f = this->printing_region(i).config().outer_wall_filament_id.value;
        if (f > 0 && (brim_filament == 0 || f < brim_filament))
            brim_filament = f;
    }
    return brim_filament == 0 ? 1u : brim_filament;
}

bool PrintObject::belt_brim_instances_compatible() const
{
    // One set of bands is shared by every instance, so they must all sit at the same height on
    // the belt: copies may sit side by side ACROSS the belt, not spaced ALONG it. (The belt
    // schedule gives one PrintObject per instance anyway; this covers the rest.)
    if (m_instances.size() <= 1)
        return true;
    BeltBrimFrame frame;
    if (! belt_brim_frame(*this, frame))
        return true;
    const Point &ref = m_instances.front().shift;
    for (const PrintInstance &inst : m_instances) {
        const coord_t along = frame.from_axis == 0 ? inst.shift.x() - ref.x() : inst.shift.y() - ref.y();
        if (std::abs(along) > SCALED_EPSILON)
            return false;
    }
    return true;
}

void PrintObject::clear_belt_brim()
{
    m_belt_brim_by_layer.clear();
    m_belt_brim_areas_by_layer.clear();
    m_belt_brim_prologue.clear();
}

void PrintObject::set_belt_brim(std::vector<ExtrusionEntityCollection> &&by_layer,
                                std::vector<ExPolygons>                &&areas,
                                std::vector<BeltBrimBand>              &&prologue)
{
    m_belt_brim_by_layer       = std::move(by_layer);
    m_belt_brim_areas_by_layer = std::move(areas);
    m_belt_brim_prologue       = std::move(prologue);
}

void make_belt_brim(PrintObject &object)
{
    object.clear_belt_brim();
    if (! object.has_belt_brim())
        return;

    const Print &print = *object.print();
    BeltBrimContext bc;
    if (! belt_brim_frame(object, bc.frame))
        return;

    const size_t nlayers = object.layers().size();
    if (nlayers == 0)
        return;

    // 1. Belt footprint: the union of each layer's slice clipped to that layer's own contact band.
    //    On a belt the object's bottom face is spread over every layer.
    ExPolygons footprint_acc;
    for (size_t i = 0; i < nlayers; ++ i) {
        const Layer &layer = *object.layers()[i];
        if (layer.lslices.empty())
            continue;
        coordf_t u_lo = bc.frame.cutoff_u(layer.print_z - layer.height);
        coordf_t u_hi = bc.frame.cutoff_u(layer.print_z);
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

    // The object's first layer WITH MATERIAL. Not layers().front(): an object the belt schedule
    // raised (Print::update_belt_schedule) starts its frame with empty layers below its material,
    // which the G-code layer collector drops. Anchoring the apron on the empty front layer put its
    // top at slice z 0, below the region's leading edge, so no apron band was made, and the bands
    // riding on the empty layers were dropped with them: the brim began at the part.
    const size_t   first_idx = size_t(std::find_if(object.layers().begin(), object.layers().end(),
        [](const Layer *l) { return ! l->lslices.empty(); }) - object.layers().begin());
    const Layer   &first     = *object.layers()[first_idx];

    // 2. Brim region, offset in the flattened (true on-belt) metric.
    const PrintObjectConfig &cfg = object.config();
    bc.brim_flow = print.brim_flow();
    const double  flow_w = bc.brim_flow.scaled_spacing() * SCALING_FACTOR;
    const coord_t width   = scale_(std::floor(cfg.brim_width.value / flow_w / 2) * flow_w * 2);
    const coord_t leading = scale_(cfg.leading_brim_length.value);
    const coord_t lateral = scale_(cfg.extra_brim_width.value);
    const coord_t gap     = scale_(cfg.brim_object_gap.value);

    // Belt printers collapse Auto / Mouse ear / Painted to outer-only: the auto width heuristic
    // and flat ear discs have no meaning on a tilted plane.
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
        // Keep only what lies at or downhill of the object's FIRST contact with the belt.
        const coordf_t u_cut   = bc.frame.cutoff_u(first.print_z);
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

    // 3. Line lattice: fixed pitch in the flattened metric, anchored at the footprint's
    //    leading-most edge so lines stay collinear across islands and across the apron.
    bc.pitch_u = std::max<coord_t>(1, coord_t(bc.brim_flow.scaled_spacing() * bc.frame.cos_tilt()));
    bc.in_plane_pitch = unscale<double>(bc.pitch_u) * bc.frame.u_stretch();
    {
        const BoundingBox fbb = get_extents(footprint);
        const bool low_side = bc.frame.shear > 0.;
        bc.u_anchor = bc.frame.from_axis == 0 ? (low_side ? fbb.min.x() : fbb.max.x())
                                              : (low_side ? fbb.min.y() : fbb.max.y());
    }

    // 4. Bands coincident with an object layer, from the first one with material; the apron below
    //    covers the empty layers under it.
    std::vector<ExtrusionEntityCollection> by_layer(nlayers);
    std::vector<ExPolygons>                areas_by_layer(nlayers);
    for (size_t i = first_idx; i < nlayers; ++ i) {
        const Layer &layer = *object.layers()[i];
        const Polygons obstacles = belt_brim_obstacles(print, object, bc.region_bbox, layer.print_z, 0.5 * layer.height);
        belt_brim_band_paths(bc, layer.print_z, layer.height, obstacles, by_layer[i], areas_by_layer[i]);
    }

    // 5. Apron prologue: the part of the region downhill of the object's first layer.
    std::vector<BeltBrimBand> prologue;
    {
        const coordf_t h      = first.height;
        const bool     low_side = bc.frame.shear > 0.;
        const coord_t  u_lead_s = bc.frame.from_axis == 0
            ? (low_side ? bc.region_bbox.min.x() : bc.region_bbox.max.x())
            : (low_side ? bc.region_bbox.min.y() : bc.region_bbox.max.y());
        const coordf_t u_lead  = unscale<double>(u_lead_s);
        const coordf_t z_lead  = bc.frame.floor_z(u_lead);   // where the belt crosses the region's leading edge
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
        std::reverse(prologue.begin(), prologue.end());   // lowest Z first
    }

    BOOST_LOG_TRIVIAL(debug) << "belt brim: axis " << bc.frame.from_axis << " shear " << bc.frame.shear << " z0 " << bc.frame.z0
        << " first layer band u [" << bc.frame.cutoff_u(first.print_z - first.height)
        << ", " << bc.frame.cutoff_u(first.print_z) << "] footprint u ["
        << unscale<double>(bc.frame.from_axis == 0 ? get_extents(footprint).min.x() : get_extents(footprint).min.y()) << ", "
        << unscale<double>(bc.frame.from_axis == 0 ? get_extents(footprint).max.x() : get_extents(footprint).max.y())
        << "] bands on layers " << std::count_if(by_layer.begin(), by_layer.end(), [](const ExtrusionEntityCollection &c) { return ! c.empty(); })
        << " apron bands " << prologue.size();
    object.set_belt_brim(std::move(by_layer), std::move(areas_by_layer), std::move(prologue));
}

} // namespace Slic3r
