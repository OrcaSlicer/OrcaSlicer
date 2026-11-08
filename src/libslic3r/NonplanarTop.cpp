// UNLAYERED_MOD: nonplanar layers -- curved top shells. See NonplanarTop.hpp for the overview.
//
// Design notes (why it is done this way):
//  * Clipping happens on finished extrusions (after infill and ironing, in the Z contouring step),
//    not on surfaces in prepare_infill. The material to remove is bounded by a curved surface,
//    S(x,y) - T, which differs on every layer, and a layer's surfaces are shared by perimeter, infill
//    and ironing generation; cutting extrusions by a per-layer footprint is local, cannot change
//    anything outside a patch (option off or no patch = untouched entities), and is the approach
//    already proven on prints.
//  * Walls: inside a patch footprint every extrusion in the top T is removed, walls included, and
//    each curved layer carries its own edge loop half a bead inside the footprint. Where the patch
//    reaches the object's outer wall those loops ARE the top of the wall, following the surface, so
//    nothing sticks up through the skin and there is no gap; where the patch ends inside the top
//    (steeper area around it) the loop is the seam against the planar stairs.
//  * The planar layers keep material at (x,y) while their top is at most S - T + h/2, so the first
//    curved layer sits on a gap between h/2 and 3h/2; its flow is metered to that gap.
//  * Shell paths go into the HIGHEST layer that intersects the patch, so nothing printed later is
//    under them; their Z offsets are relative to that layer's print_z.

#include "NonplanarTop.hpp"
#include "NonplanarTopSlice.hpp"

#include "ClipperUtils.hpp"
#include "Exception.hpp"
#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "Fill/FillBase.hpp"
#include "Flow.hpp"
#include "Geometry.hpp"
#include "Layer.hpp"
#include "Model.hpp"
#include "Print.hpp"
#include "PrintConfig.hpp"
#include "SLA/IndexedMesh.hpp"
#include "Surface.hpp"
#include "TriangleMeshSlicer.hpp"
#include "libslic3r.h"

#include <boost/log/trivial.hpp>
#include <tbb/parallel_for.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace Slic3r {

void (*nonplanar_top_band_test_hook)() = nullptr;

namespace {

constexpr double kFlatSlopeDeg = 0.5;   // a max angle below this is "planar only"
constexpr double kResample     = 0.15;  // mm between lifted samples
constexpr double kCollinearTol = 0.005; // mm, collinear merge of lifted samples
constexpr double kFlowStep     = 0.03;  // a new first-layer piece when flow drifts by this much
constexpr double kMinFlow      = 0.5;   // first curved layer flow clamp (x nominal)
constexpr double kMaxFlow      = 1.6;

struct Params
{
    const PrintRegionConfig *region = nullptr; // the first region that enables the feature
    double h          = 0.2;  // nominal layer height
    int    n          = 1;    // curved layers
    double T          = 0.2;  // shell depth = n * h
    double max_angle  = 12.;  // effective, degrees
    double max_height = 5.;
    double env_tan    = 1.;   // tan of the head envelope's smallest side angle (45 deg if unknown)
    double env_reach  = 40.;  // envelope radius, mm
    double flow_min   = kMinFlow; // first curved layer gap clamp, x h (also min/max layer height)
    double flow_max   = kMaxFlow;
    double tip_r      = 0.5;  // radius of the nozzle's flat tip face, mm
    bool   hold_back  = false; // nonplanar_hold_back: islands next to a top print after its shell
    // UNLAYERED_MOD -- a tilted slice frame (cut-plane chunk, layer rotation): the upright nozzle is `tilt` degrees off the
    // frame's Z, leaning toward `lean` (unit, frame XY). Slopes are measured against `up`, and every limit that checks a
    // rise in one direction takes it there: limit - tilt * (direction . lean), so it is tighter leaning toward the nozzle's
    // tilt and looser away from it. tilt 0 is the old symmetric behaviour exactly.
    Vec3d  up         = Vec3d::UnitZ();
    Vec2d  lean       = Vec2d::Zero();
    double tilt       = 0.;
    double env_deg    = 45.;  // the head envelope's smallest side angle, degrees
    // tan of `base_deg` for a rise toward (dx, dy) (any length; zero = the plain base)
    double tan_dir(double base_deg, double dx, double dy) const {
        const double len = std::hypot(dx, dy);
        const double a   = len > 0. && tilt > 0. ? base_deg - tilt * (dx * lean.x() + dy * lean.y()) / len : base_deg;
        return std::tan(std::clamp(a, 0.5, 89.) * M_PI / 180.);
    }
};

struct Cell
{
    float s     = 0.f;  // top surface Z, print frame
    float slope = 90.f; // degrees
    bool  hit   = false;
    bool  ok    = false; // eligible: printable curved
    int   patch = -1;
    // Lowest curved layer printed over this cell (0 = the whole shell). Near the edge of a patch
    // the deeper curved layers would put the nozzle tip below the planar material next to it, so
    // there the shell gets thinner and the planar layers reach correspondingly higher: a ramp.
    int   kmin  = 0;
    // kmin for clipping the planar layers: the smallest kmin within one cell (about half a bead),
    // so the outer half of a curved layer's edge bead never overhangs a planar step left higher.
    int   kclip = 0;
    // Why a cell is not curved (debug map): 0 curved, 1 no surface, 2 cut face, 3 too steep, 4 too thin,
    // 5 patch flatter than a layer, 6 deeper than max height, 7 head clearance, 8 foot of a cliff.
    uint8_t why = 1;
    // 0.1.9: a cell is one top surface of one grid column (a node). A column has one node per up-facing surface
    // its down-ray meets (a deck under a roof is the second node of its column), topmost first.
    int     col   = 0;     // column index j * nx + i
    int     level = 0;     // 0 = the topmost surface of the column
    float   bot   = 0.f;   // print frame: bottom of the material span under this top (next down-facing surface)
    int     band  = 0;     // a top taller than the max height is split into height bands; patches never cross bands
    // Curved layers k < kmax are printed with this node's own top; the ones above by the next band (a shingled seam
    // between two height bands, see cov_of in np_build). n everywhere else.
    int     kmax  = 0;
};

struct Grid
{
    double x0 = 0, y0 = 0, g = 0.25; // mm, cell centres at x0 + i*g
    int    nx = 0, ny = 0;
    std::vector<Cell> cells;         // nodes, grouped by column, topmost first
    std::vector<int>  head, cnt;     // per column: first node, node count
    double cx(int i) const { return x0 + i * g; }
    double cy(int j) const { return y0 + j * g; }
    int    ncols() const { return nx * ny; }
    int    ci(const Cell &c) const { return c.col % nx; }
    int    cj(const Cell &c) const { return c.col / nx; }
    // Nodes of column (i, j): [first, last)
    int    first(int i, int j) const { return head[size_t(j) * nx + i]; }
    int    last(int i, int j) const { return head[size_t(j) * nx + i] + cnt[size_t(j) * nx + i]; }
    bool   inside(int i, int j) const { return i >= 0 && i < nx && j >= 0 && j < ny; }
};

struct Patch
{
    std::vector<int> cells;      // node indices
    double smin = 0, smax = 0;   // print frame
    int    host = -1;            // layer index the shell is printed in
    double host_ref = 0;         // max(host print_z, smax): the highest material when the shell prints
    ExPolygons footprint;        // of the top curved layer
    std::vector<ExPolygons> layer_footprint; // per curved layer k (cells with kmin <= k)
    ExPolygons fp0;              // the footprint before a shingled seam moved layers between bands (the clip's zone)
    ExPolygons outer;            // everything any curved layer of this top covers (walls run along its edge)
    std::vector<ExPolygons> handoff; // per curved layer k: where the neighbouring band prints layer k instead
    double     smin_lift = 0;    // lowest surface under any of its curved paths
    ExPolygons real;             // the top's real outline (slices of the highest layers under it, near its cells)
};

// Top envelope from above, in mesh coordinates, bounded by a segment's ceilings.
struct Surface3
{
    const sla::IndexedMesh                    &mesh;
    const std::vector<Layer::ContourZCeiling> &ceilings;
    double top;    // ray origin Z, mesh frame
    double ground; // mesh.ground_level()
    double zmin;   // object_print_z_min: print = mesh - ground + zmin

    struct Hit { bool hit = false; bool cut = false; double z = 0; Vec3d n = Vec3d::UnitZ(); double floor = -1e9; };

    Hit down(double x, double y) const
    {
        Hit r;
        sla::IndexedMesh::hit_result h = mesh.query_ray_hit(Vec3d(x, y, top), -Vec3d::UnitZ());
        // A ray exactly on a mesh edge can slip between the two facets; nudge it off the edge.
        if (! h.is_hit())
            h = mesh.query_ray_hit(Vec3d(x + 1.3e-4, y + 0.7e-4, top), -Vec3d::UnitZ());
        if (! h.is_hit())
            return r;
        r.hit = true;
        r.z   = top - h.distance();
        r.n   = h.normal();
        for (const Layer::ContourZCeiling &c : ceilings) {
            if (std::abs(c.normal.z()) < EPSILON)
                continue;
            const double zc = (c.offset - c.normal.x() * x - c.normal.y() * y) / c.normal.z();
            if (c.normal.z() > 0) {
                if (zc < r.z - 1e-4) { r.z = zc; r.n = c.normal; r.cut = true; }
            } else
                r.floor = std::max(r.floor, zc);
        }
        return r;
    }
    double to_print(double z_mesh) const { return z_mesh - ground + zmin; }
};

double slope_deg(const Vec3d &n, const Vec3d &up = Vec3d::UnitZ())
{
    const double len = n.norm();
    if (len < EPSILON)
        return 90.;
    return std::acos(std::clamp(n.dot(up) / len, -1., 1.)) * 180. / M_PI;
}

ExPolygons cells_to_expolygons(const Grid &grid, const std::function<bool(const Cell &)> &pred)
{
    // Row runs as rectangles, then one union. Rectangles are grown by a micron so neighbours merge.
    // A column is in when any of its nodes is.
    Polygons rects;
    const coord_t half = coord_t(scale_(grid.g * 0.5)) + 1000;
    auto col_in = [&](int i, int j) {
        for (int u = grid.first(i, j); u < grid.last(i, j); ++ u)
            if (pred(grid.cells[u])) return true;
        return false;
    };
    for (int j = 0; j < grid.ny; ++ j) {
        int i = 0;
        while (i < grid.nx) {
            if (! col_in(i, j)) { ++ i; continue; }
            int k = i;
            while (k + 1 < grid.nx && col_in(k + 1, j)) ++ k;
            const coord_t xa = coord_t(scale_(grid.cx(i))) - half, xb = coord_t(scale_(grid.cx(k))) + half;
            const coord_t ya = coord_t(scale_(grid.cy(j))) - half, yb = coord_t(scale_(grid.cy(j))) + half;
            rects.emplace_back(Points{ Point(xa, ya), Point(xb, ya), Point(xb, yb), Point(xa, yb) });
            i = k + 1;
        }
    }
    return union_ex(rects);
}

// Grows `tiles` out to the outline where it is within `reach` of it, so the cell raster does not
// leave a strip of planar material along the wall.
ExPolygons reach_outline(const ExPolygons &tiles, const ExPolygons &outline, double reach)
{
    const float r = float(scale_(reach));
    const ExPolygons band = diff_ex(outline, offset_ex(outline, -r));
    ExPolygons grown = intersection_ex(offset_ex(tiles, r), band);
    append(grown, tiles);
    return intersection_ex(union_ex(grown), outline);
}

void collect_paths(const ExtrusionEntity *e, std::vector<const ExtrusionPath *> &out)
{
    if (const auto *p = dynamic_cast<const ExtrusionPath *>(e))
        out.push_back(p);
    else if (const auto *mp = dynamic_cast<const ExtrusionMultiPath *>(e))
        for (const ExtrusionPath &p : mp->paths) out.push_back(&p);
    else if (const auto *lp = dynamic_cast<const ExtrusionLoop *>(e))
        for (const ExtrusionPath &p : lp->paths) out.push_back(&p);
}

bool is_shell(const ExtrusionEntity *e)
{
    if (const auto *c = dynamic_cast<const ExtrusionEntityCollection *>(e)) {
        if (c->entities.empty())
            return false;
        for (const ExtrusionEntity *x : c->entities)
            if (! is_shell(x)) return false;
        return true;
    }
    std::vector<const ExtrusionPath *> paths;
    collect_paths(e, paths);
    return ! paths.empty() && std::all_of(paths.begin(), paths.end(), [](const ExtrusionPath *p) { return p->nonplanar_shell; });
}

// Removes `cut` (scaled) from every extrusion under `e`, appending the result to `out`.
// Entities that do not touch `cut` are copied unchanged. Returns true if anything was cut.
bool clip_entity(const ExtrusionEntity *e, const ExPolygons &cut, const BoundingBox &cut_bb, ExtrusionEntityCollection &out)
{
    if (const auto *c = dynamic_cast<const ExtrusionEntityCollection *>(e)) {
        auto sub = std::make_unique<ExtrusionEntityCollection>();
        sub->no_sort = c->no_sort;
        bool changed = false;
        for (const ExtrusionEntity *x : c->entities)
            changed |= clip_entity(x, cut, cut_bb, *sub);
        if (! changed)
            out.append(*c);
        else if (! sub->entities.empty())
            out.entities.push_back(sub.release());
        return changed;
    }
    std::vector<const ExtrusionPath *> paths;
    collect_paths(e, paths);
    if (paths.empty()) {
        out.append(*e);
        return false;
    }
    BoundingBox bb;
    for (const ExtrusionPath *p : paths)
        for (const Point3 &pt : p->polyline.points) bb.merge(Point(pt.x(), pt.y()));
    bool touches = bb.defined && bb.overlap(cut_bb);
    if (touches)
        touches = std::any_of(paths.begin(), paths.end(), [&cut](const ExtrusionPath *p) {
            return ! intersection_pl(Polylines{ p->polyline.to_polyline() }, cut).empty();
        });
    if (! touches) {
        out.append(*e);
        return false;
    }
    for (const ExtrusionPath *p : paths) {
        Polylines kept = diff_pl(Polylines{ p->polyline.to_polyline() }, cut);
        for (Polyline &pl : kept) {
            // Slivers left by the cut (shorter than ~2 beads) are not worth printing and would stand in
            // the way of the curved layers printed later over the same spot.
            if (pl.points.size() < 2 || pl.length() < scale_(0.8))
                continue;
            out.entities.push_back(new ExtrusionPath(Polyline3(pl), *p));
        }
    }
    return true;
}

// Largest layer print_z <= z (the planar top under a point), or -1 if none.
double planar_top_below(const std::vector<double> &print_zs, double z)
{
    auto it = std::upper_bound(print_zs.begin(), print_zs.end(), z + 1e-6);
    return it == print_zs.begin() ? -1. : *(it - 1);
}

// Curved shells built at slice time (with the slice-level clip), put into the fills by make_nonplanar_top() after
// infill. Keyed by the PrintObject; valid while its layers are the ones they were built for.
struct NpCached
{
    struct Shell { size_t layer; size_t region; std::unique_ptr<ExtrusionEntityCollection> shell; };
    std::vector<Shell>   shells;
    std::vector<const Layer *> layers;
    std::vector<ExPolygons>    cut; // per layer: what the slice-level clip took away (the shells' band)
    ExPolygons                 cover; // what the top curved layers cover (their beads, closed)
    std::vector<ExPolygons>    held_under; // per layer: where it stands on a held island printed with a shell (not the clipped layer below)
    std::vector<ExPolygons>    held_above; // per layer: held islands on the layer above (printed later), so not a real top here
    std::shared_ptr<const NonplanarZaaLimits> zaa; // Z contouring limits beside the shells
};

// Shell material per 0.1 mm cell, already grown by the nozzle's reach: (host layer, highest z) pairs.
class NpZaaMap final : public NonplanarZaaLimits
{
public:
    double x0 = 0, y0 = 0, cell = 0.1;
    int    nx = 0, ny = 0;
    std::vector<std::vector<std::pair<int, float>>> v;
    // Per cell directly under a shell bead (half its width, no nozzle reach): (host layer, lowest bead z). The band's
    // bottom there is that bead's z less one layer: S - T where all n curved layers are printed.
    std::vector<std::vector<std::pair<int, float>>> band;
    double layer_h = 0.2;

    bool band_bottom(size_t layer_id, double x, double y, double &z) const override
    {
        const int i = int(std::floor((x - x0) / cell)), j = int(std::floor((y - y0) / cell));
        if (i < 0 || j < 0 || i >= nx || j >= ny)
            return false;
        bool found = false;
        z = 1e30;
        for (const auto &[l, zl] : band[size_t(j) * nx + i])
            if (size_t(l) >= layer_id) {
                z     = std::min(z, double(zl) - layer_h);
                found = true;
            }
        return found;
    }

    bool query(size_t layer_id, double print_z, double x, double y, double &z_floor, bool &no_raise) const override
    {
        const int i = int(std::floor((x - x0) / cell)), j = int(std::floor((y - y0) / cell));
        if (i < 0 || j < 0 || i >= nx || j >= ny)
            return false;
        const auto &e = v[size_t(j) * nx + i];
        if (e.empty())
            return false;
        z_floor  = -1e30;
        no_raise = false;
        for (const auto &[l, z] : e) {
            if (size_t(l) < layer_id)
                z_floor = std::max(z_floor, double(z));
            else if (double(z) <= print_z + 0.5)
                no_raise = true;
        }
        return true;
    }
};

std::shared_ptr<const NonplanarZaaLimits> np_build_zaa(const PrintObject &object, const NpCached &c)
{
    const PrintConfig &pc     = object.print()->config();
    const double       nozzle = pc.nozzle_diameter.values.empty() ? 0.4 : pc.nozzle_diameter.values.front();
    struct S { double x, y, z, r; int l; };
    std::vector<S> samples;
    for (const NpCached::Shell &sh : c.shells) {
        if (sh.layer >= object.layers().size()) continue;
        const double pz = object.layers()[sh.layer]->print_z;
        std::function<void(const ExtrusionEntity *)> visit = [&](const ExtrusionEntity *e) {
            if (const auto *cc = dynamic_cast<const ExtrusionEntityCollection *>(e)) { for (const ExtrusionEntity *x : cc->entities) visit(x); return; }
            std::vector<const ExtrusionPath *> paths;
            collect_paths(e, paths);
            for (const ExtrusionPath *p : paths) {
                // a bead's half width plus the nozzle's reach beyond its centre
                const double r = 0.5 * p->width + nozzle;
                const Points3 &pts = p->polyline.points;
                for (size_t i = 0; i + 1 < pts.size(); ++ i) {
                    const Vec3d a(unscale<double>(pts[i].x()), unscale<double>(pts[i].y()), pz + unscale<double>(pts[i].z()));
                    const Vec3d b(unscale<double>(pts[i + 1].x()), unscale<double>(pts[i + 1].y()), pz + unscale<double>(pts[i + 1].z()));
                    const int   ns = std::max(1, int(std::ceil((b - a).head<2>().norm() / 0.1)));
                    for (int s = 0; s <= ns; ++ s) {
                        const Vec3d q = a + (b - a) * (double(s) / ns);
                        samples.push_back({ q.x(), q.y(), q.z(), r, int(sh.layer) });
                    }
                }
            }
        };
        visit(sh.shell.get());
    }
    if (samples.empty())
        return nullptr;
    auto m = std::make_shared<NpZaaMap>();
    double xa = 1e30, ya = 1e30, xb = -1e30, yb = -1e30, rmax = 0;
    for (const S &s : samples) { xa = std::min(xa, s.x); ya = std::min(ya, s.y); xb = std::max(xb, s.x); yb = std::max(yb, s.y); rmax = std::max(rmax, s.r); }
    m->x0 = xa - rmax - m->cell;
    m->y0 = ya - rmax - m->cell;
    m->nx = int(std::ceil((xb - xa + 2. * rmax) / m->cell)) + 3;
    m->ny = int(std::ceil((yb - ya + 2. * rmax) / m->cell)) + 3;
    m->v.assign(size_t(m->nx) * m->ny, {});
    m->band.assign(size_t(m->nx) * m->ny, {});
    m->layer_h = object.config().layer_height.value;
    for (const S &s : samples) {
        const int ci = int(std::floor((s.x - m->x0) / m->cell)), cj = int(std::floor((s.y - m->y0) / m->cell));
        const int R  = int(std::ceil(s.r / m->cell));
        for (int dj = -R; dj <= R; ++ dj)
            for (int di = -R; di <= R; ++ di) {
                const int i = ci + di, j = cj + dj;
                if (i < 0 || j < 0 || i >= m->nx || j >= m->ny) continue;
                if (std::hypot(m->x0 + (i + 0.5) * m->cell - s.x, m->y0 + (j + 0.5) * m->cell - s.y) > s.r) continue;
                auto &e  = m->v[size_t(j) * m->nx + i];
                auto  it = std::find_if(e.begin(), e.end(), [&s](const std::pair<int, float> &q) { return q.first == s.l; });
                if (it == e.end()) e.emplace_back(s.l, float(s.z));
                else it->second = std::max(it->second, float(s.z));
            }
        // The band table: only under the bead itself.
        const double rb = s.r - nozzle;
        const int    Rb = int(std::ceil(rb / m->cell));
        for (int dj = -Rb; dj <= Rb; ++ dj)
            for (int di = -Rb; di <= Rb; ++ di) {
                const int i = ci + di, j = cj + dj;
                if (i < 0 || j < 0 || i >= m->nx || j >= m->ny) continue;
                if (std::hypot(m->x0 + (i + 0.5) * m->cell - s.x, m->y0 + (j + 0.5) * m->cell - s.y) > rb) continue;
                auto &e  = m->band[size_t(j) * m->nx + i];
                auto  it = std::find_if(e.begin(), e.end(), [&s](const std::pair<int, float> &q) { return q.first == s.l; });
                if (it == e.end()) e.emplace_back(s.l, float(s.z));
                else it->second = std::min(it->second, float(s.z));
            }
    }
    return m;
}
std::mutex                                                          g_np_cache_mutex;
std::unordered_map<const PrintObject *, std::shared_ptr<NpCached>> g_np_cache;

} // namespace

// The planar top shell's layer count (see nonplanar_layer_count).
static int planar_top_layer_count(const PrintRegionConfig &region, double layer_height)
{
    // PrintObject::discover_vertical_shells() keeps a layer solid while a
    // top surface is less than top_shell_layers layers OR less than top_shell_thickness above it, so the
    // count is the larger of the two (0.7 mm at 0.2 mm layers = 4 layers, not 3).
    int n = region.top_shell_layers.value;
    if (n > 0 && region.top_shell_thickness.value > 0. && layer_height > EPSILON)
        n = std::max(n, int(std::ceil(region.top_shell_thickness.value / layer_height - 1e-6)));
    return n;
}

int nonplanar_layer_count(const PrintRegionConfig &region, double layer_height)
{
    if (region.nonplanar_layer_count.value > 0)
        return region.nonplanar_layer_count.value;
    // As many as the planar top shell: PrintObject::discover_vertical_shells() keeps a layer solid while a
    // top surface is less than top_shell_layers layers OR less than top_shell_thickness above it, so the
    // count is the larger of the two (0.7 mm at 0.2 mm layers = 4 layers, not 3).
    int n = region.top_shell_layers.value;
    if (n > 0 && region.top_shell_thickness.value > 0. && layer_height > EPSILON)
        n = std::max(n, int(std::ceil(region.top_shell_thickness.value / layer_height - 1e-6)));
    return std::max(1, n);
}

// Internal solid planar layers directly under a shell: the planar top shell's layer count minus the curved layer count,
// never negative, so solid + curved layers make a normal top shell (0 with the automatic curved layer count). Taken from
// the first region that asks for nonplanar layers, like the shell itself.
static int solid_layers_under_shell(const PrintObject &object)
{
    const double h = object.config().layer_height.value;
    for (size_t r = 0; r < object.num_printing_regions(); ++ r) {
        const PrintRegionConfig &rc = object.printing_region(r).config();
        if (rc.nonplanar_layers.value)
            return h > EPSILON ? std::max(0, planar_top_layer_count(rc, h) - nonplanar_layer_count(rc, h)) : 0;
    }
    return 0;
}

bool nonplanar_top_requested(const PrintObject &object)
{
    for (size_t r = 0; r < object.num_printing_regions(); ++ r)
        if (object.printing_region(r).config().nonplanar_layers.value)
            return true;
    return false;
}

double nonplanar_effective_max_angle(const PrintRegionConfig &region, const ConfigBase &printer)
{
    double a = region.nonplanar_max_angle.value;
    const auto *en = printer.option<ConfigOptionBool>("enable_nonplanar");
    if (en != nullptr && en->value) {
        const HotendEnvelope env = HotendEnvelope::from_config(printer);
        if (env.enabled())
            for (double side : env.angle)
                a = std::min(a, side);
    }
    return std::clamp(a, 0., 89.);
}

// UNLAYERED_MOD -- how far the slice frame is tilted from the world (degrees): a tilted cut-plane chunk or layer rotation.
// object_to_slice() is that frame's rotation times the object's own placement (object.trafo()), divided out here so a
// part merely rotated on the plate counts as 0. On a 3-axis or virtual 5-axis machine the nozzle stays upright in the
// world, i.e. this far off the frame's Z, so the nonplanar angle limits shrink by it (np_build).
static double np_frame_tilt_deg(const PrintObject &object)
{
    // A real rotary machine (AB / BC / AC, not virtual) turns the part or the head so the nozzle follows the chunk's build
    // direction: the clearance cone rotates with the frame, so there is no tilt to allow for (2026-11-02).
    const PrintConfig &pc = object.print()->config();
    if (! pc.virtual_5axis.value && pc.axis_selection.value != AxisSelection::Virtual)
        return 0.;
    const Eigen::Matrix3d frame = object.object_to_slice().linear() * object.trafo().linear().inverse();
    const double          cz    = std::clamp((frame * Vec3d::UnitZ()).normalized().z(), -1., 1.);
    return std::acos(cz) * 180. / M_PI;
}

// Analysis, eligibility, shells and the slice-level clip of one object (see nonplanar_top_clip_slices).
// `its` is the same mesh as `mesh` (slice frame), for slicing it at the shell's floor.
static NonplanarTopStats np_build(PrintObject &object, const sla::IndexedMesh &mesh, const indexed_triangle_set &its,
                                  const std::vector<Layer::ContourZCeiling> &ceilings, NpCached &out, std::string &skip)
{
    NonplanarTopStats stats;
    const auto t_begin = std::chrono::steady_clock::now();
    const LayerPtrs &layers = object.layers();
    if (layers.size() < 2 || ! nonplanar_top_requested(object) || object.print()->config().spiral_mode.value)
        return stats;

    // ---- parameters (object level: the first region that asks for it)
    Params P;
    for (size_t r = 0; r < object.num_printing_regions(); ++ r)
        if (object.printing_region(r).config().nonplanar_layers.value) {
            P.region = &object.printing_region(r).config();
            break;
        }
    const PrintRegionConfig &rc = *P.region;
    const PrintConfig       &pc = object.print()->config();
    P.h = object.config().layer_height.value;
    if (P.h <= EPSILON)
        return stats;
    P.n          = nonplanar_layer_count(rc, P.h);
    P.T          = P.n * P.h;
    // In a tilted frame the upright nozzle is `tilt` off the frame's Z: slopes are measured against it (P.up) and the
    // per-direction limits lean with it (Params::tan_dir).
    {
        // np_frame_tilt_deg() is 0 on a real rotary machine: there the cone follows the frame and nothing is skewed.
        if (np_frame_tilt_deg(object) > 1e-6) {
            const Eigen::Matrix3d frame = object.object_to_slice().linear() * object.trafo().linear().inverse();
            P.up   = (frame * Vec3d::UnitZ()).normalized();
            P.tilt = std::acos(std::clamp(P.up.z(), -1., 1.)) * 180. / M_PI;
            const Vec2d xy(P.up.x(), P.up.y());
            P.lean = xy.norm() > 1e-12 ? Vec2d(xy / xy.norm()) : Vec2d::Zero();
        }
    }
    P.max_angle  = nonplanar_effective_max_angle(rc, pc);
    P.max_height = rc.nonplanar_max_height.value;
    P.hold_back  = rc.nonplanar_hold_back.value;
    // A nozzle's flat tip is about 2.5 orifice diameters across. (nonplanar_nozzle_tip_diameter and
    // nonplanar_top_surface_tip_lift are kept as options so presets that carry them still load; they do nothing.)
    if (! pc.nozzle_diameter.values.empty())
        P.tip_r = 1.25 * pc.nozzle_diameter.values.front();
    if (! pc.max_layer_height.values.empty() && pc.max_layer_height.values.front() > EPSILON)
        P.flow_max = std::max(1., std::min(kMaxFlow, pc.max_layer_height.values.front() / P.h));
    if (! pc.min_layer_height.values.empty() && pc.min_layer_height.values.front() > EPSILON)
        P.flow_min = std::min(1., std::max(kMinFlow, pc.min_layer_height.values.front() / P.h));
    {
        const HotendEnvelope env = HotendEnvelope::from_config(pc);
        double a = 45.;
        if (pc.enable_nonplanar.value && env.enabled()) {
            a = *std::min_element(env.angle.begin(), env.angle.end());
            P.env_reach = env.height;
        }
        P.env_deg = std::clamp(a, 1., 89.);
        P.env_tan = std::tan(P.env_deg * M_PI / 180.);
    }
    if (P.max_angle - P.tilt < kFlatSlopeDeg || P.max_height < P.T + P.h)
        return stats;

    const double zmin = object.slicing_parameters().object_print_z_min;
    std::vector<double> print_zs;
    for (const Layer *l : layers) print_zs.push_back(l->print_z);

    // ---- height field: every top surface of every column. The topmost one is cast exactly as in 0.1.8 (cut-plane
    // ceilings included); the ones under it (a deck under a roof, a slope under a shelf) come from all the hits of
    // the same down-ray. Each node knows the bottom of its material span, so the clearance analysis knows what is
    // printed when, and the clip only cuts a top's own material.
    BoundingBox obb;
    for (const Layer *l : layers)
        for (const ExPolygon &ex : l->lslices) obb.merge(get_extents(ex.contour));
    if (! obb.defined)
        return stats;
    const double area = unscale<double>(obb.size().x()) * unscale<double>(obb.size().y());
    Grid G;
    G.g  = std::max(0.25, std::sqrt(area / 4e6));
    G.x0 = unscale<double>(obb.min.x()) + 0.5 * G.g;
    G.y0 = unscale<double>(obb.min.y()) + 0.5 * G.g;
    G.nx = std::max(1, int(std::ceil(unscale<double>(obb.size().x()) / G.g)));
    G.ny = std::max(1, int(std::ceil(unscale<double>(obb.size().y()) / G.g)));

    double mesh_top = -std::numeric_limits<double>::max();
    for (const Vec3f &v : mesh.vertices()) mesh_top = std::max(mesh_top, double(v.z()));
    const Surface3 surf{ mesh, ceilings, mesh_top + 1., mesh.ground_level(), zmin };
    constexpr int kMaxLevels = 4;

    {
        std::vector<std::vector<Cell>> colnodes(size_t(G.ncols()));
        tbb::parallel_for(tbb::blocked_range<int>(0, G.ny), [&](const tbb::blocked_range<int> &range) {
            for (int j = range.begin(); j < range.end(); ++ j)
                for (int i = 0; i < G.nx; ++ i) {
                    std::vector<Cell> &nodes = colnodes[size_t(j) * G.nx + i];
                    const Surface3::Hit h = surf.down(G.cx(i), G.cy(j));
                    if (! h.hit)
                        continue;
                    Cell c;
                    c.col   = j * G.nx + i;
                    c.hit   = true;
                    c.s     = float(surf.to_print(h.z));
                    c.slope = float(slope_deg(h.n, P.up));
                    c.why   = h.cut ? 2 : 3;
                    // The column's material spans, top down, from all hits of the down-ray with a winding count, so
                    // overlapping parts (several volumes of one object) count as their union: a top is where the count
                    // goes 0 -> 1 going down, a bottom where it goes back to 0.
                    struct Span { double top, bot, nz; Vec3d n; };
                    std::vector<Span> spans;
                    {
                        const std::vector<sla::IndexedMesh::hit_result> hits =
                            mesh.query_ray_hits(Vec3d(G.cx(i) + 1.3e-4, G.cy(j) + 0.7e-4, surf.top), -Vec3d::UnitZ());
                        int    depth = 0;
                        double last_t = -1.;
                        int    last_sign = 0;
                        for (const auto &hh : hits) {
                            if (! hh.is_hit()) continue;
                            const double nz   = hh.normal().z() / std::max(1e-12, hh.normal().norm());
                            const int    sign = nz > 1e-6 ? 1 : nz < -1e-6 ? -1 : 0;
                            if (sign == 0) continue;
                            if (sign == last_sign && std::abs(hh.distance() - last_t) < 1e-5) continue; // a shared edge, hit twice
                            last_t = hh.distance(); last_sign = sign;
                            const double z = surf.top - hh.distance();
                            if (sign > 0) {
                                if (depth == 0) spans.push_back({ z, surf.ground, nz, hh.normal().cast<double>() });
                                ++ depth;
                            } else if (depth > 0 && -- depth == 0)
                                spans.back().bot = z;
                        }
                    }
                    // Material under the topmost top: its span, or (no usable spans) the ray continues from just under it.
                    double thick;
                    if (! spans.empty() && std::abs(spans.front().top - h.z) < 1e-3)
                        thick = h.z - spans.front().bot;
                    else {
                        const sla::IndexedMesh::hit_result below = mesh.query_ray_hit(Vec3d(G.cx(i), G.cy(j), h.z - 1e-3), -Vec3d::UnitZ());
                        thick = below.is_hit() ? below.distance() + 1e-3 : h.z - surf.ground;
                    }
                    // Nothing under the bed is printed: a sunk object's top counts only its material above the plate
                    // (surf.ground is the plate in the slice frame), or its curved layers would reach below z 0.
                    const double floor = std::max(h.floor, surf.ground);
                    thick = std::min(thick, h.z - floor);
                    c.bot = float(surf.to_print(h.z - thick));
                    if (! h.cut && h.n.z() > 0. && c.slope <= P.max_angle) {
                        c.ok  = thick >= P.T + P.h - 1e-6;
                        c.why = c.ok ? 0 : 4;
                    }
                    nodes.push_back(c);
                    if (h.cut)
                        continue;
                    // Tops under the topmost one.
                    for (const Span &sp : spans) {
                        if (int(nodes.size()) >= kMaxLevels) break;
                        if (sp.top >= h.z - 0.05) continue;
                        if (sp.top < floor) break;
                        Cell u;
                        u.col   = c.col;
                        u.level = int(nodes.size());
                        u.hit   = true;
                        u.s     = float(surf.to_print(sp.top));
                        u.slope = float(slope_deg(sp.n, P.up));
                        u.bot   = float(surf.to_print(std::max(sp.bot, floor)));
                        u.why   = 3;
                        if (u.slope <= P.max_angle) {
                            u.ok  = sp.top - std::max(sp.bot, floor) >= P.T + P.h - 1e-6;
                            u.why = u.ok ? 0 : 4;
                        }
                        nodes.push_back(u);
                    }
                }
        });
        G.head.assign(size_t(G.ncols()), 0);
        G.cnt.assign(size_t(G.ncols()), 0);
        // (one reserve: append() reserves the exact size on every call, which is quadratic here; the coaster's
        // 130k columns took 74 s)
        size_t total = 0;
        for (const std::vector<Cell> &v : colnodes) total += v.size();
        G.cells.reserve(total);
        for (int c = 0; c < G.ncols(); ++ c) {
            G.head[c] = int(G.cells.size());
            G.cnt[c]  = int(colnodes[c].size());
            G.cells.insert(G.cells.end(), colnodes[c].begin(), colnodes[c].end());
        }
        for (Cell &c : G.cells) c.kmax = P.n;
    }
    const size_t NN = G.cells.size();
    const auto   t_grid = std::chrono::steady_clock::now();
    // Hold-back islands (see the passes around the patch loop): per column, the highest printed point of material that is not a
    // top's while the top that holds it (host_ref colcap_ref and earlier) prints. +inf where nothing is held.
    std::vector<float> colcap(size_t(G.ncols()), 1e30f), colcap_ref(size_t(G.ncols()), -1e30f);
    // The node of column (i, j) on the same surface as height s (within tol), or -1.
    auto node_near = [&G](int i, int j, double s, double tol) {
        int best = -1;
        double bd = tol;
        for (int u = G.first(i, j); u < G.last(i, j); ++ u)
            if (std::abs(G.cells[u].s - s) <= bd) { bd = std::abs(G.cells[u].s - s); best = u; }
        return best;
    };

    // A column on the very outline can hit the side wall where it meets the top, and so reads as
    // vertical. It is not a steep spot: if a neighbour inside is eligible, so is it. (Left out, it
    // stays planar, its stair overshoot fails the clearance of the curved cell downhill of it, and the
    // exclusion cascades down the slope along the edge: tt_5 lost four 15 mm triangles that way.)
    // "Outline": the neighbour column has no surface within 1 mm (off the part, or a top far above or below).
    {
        std::vector<char> edge_ok(NN, 0);
        for (size_t u = 0; u < NN; ++ u) {
            const Cell &c = G.cells[u];
            if (! c.hit || c.ok || c.why != 3)
                continue;
            const int i = G.ci(c), j = G.cj(c);
            bool outline = false, inner_ok = false;
            const int nb[4][2] = { {i + 1, j}, {i - 1, j}, {i, j + 1}, {i, j - 1} };
            for (const auto &q : nb) {
                if (! G.inside(q[0], q[1])) { outline = true; continue; }
                const int v = node_near(q[0], q[1], c.s, 1.0);
                if (v < 0) outline = true;
                else if (G.cells[v].ok) inner_ok = true;
            }
            edge_ok[u] = outline && inner_ok;
        }
        for (size_t u = 0; u < NN; ++ u)
            if (edge_ok[u]) { G.cells[u].ok = true; G.cells[u].why = 0; }
    }

    // ---- patches (iterated: removing cells can change a patch's host and its neighbours)
    // A cliff between two gentle cells (the side of a ramp standing on a gentle base, a tower wall one cell wide)
    // must not lie inside a top: a curved line across it would drop through material printed beside it (angle
    // test: 0.33 mm). Both sides of it stay planar, so the curved tops end there. (A neighbour column with another
    // gentle node on the same surface continues the top: that is not a cliff.)
    const double dz_cliff = G.g * std::tan(P.max_angle * M_PI / 180.) + 0.015;
    {
        std::vector<char> cliff(NN, 0);
        for (size_t u = 0; u < NN; ++ u) {
            const Cell &c = G.cells[u];
            if (! c.ok) continue;
            const int i = G.ci(c), j = G.cj(c);
            const int nb[4][2] = { {i + 1, j}, {i - 1, j}, {i, j + 1}, {i, j - 1} };
            for (const auto &q : nb) {
                if (! G.inside(q[0], q[1])) continue;
                bool cont = false, big = false;
                for (int v = G.first(q[0], q[1]); v < G.last(q[0], q[1]); ++ v) {
                    if (! G.cells[v].ok) continue;
                    // Only the foot of a cliff: the cell on top of it is at its own top's outline (a rim over a deck),
                    // and dropping it took the clearance of the whole narrow top with it (0.1.9).
                    // the rise toward q, or toward this cell when q is lower (P.tan_dir: tilted frames)
                    const double dz  = double(G.cells[v].s) - double(c.s);
                    const double sgn = dz >= 0. ? 1. : -1.;
                    const double lim = P.tilt > 0. ? G.g * P.tan_dir(P.max_angle, sgn * (q[0] - i), sgn * (q[1] - j)) + 0.015 : dz_cliff;
                    if (std::abs(dz) <= lim) cont = true; else if (G.cells[v].s > c.s) big = true;
                }
                if (big && ! cont) cliff[u] = 1;
            }
        }
        for (size_t u = 0; u < NN; ++ u)
            if (cliff[u]) { G.cells[u].ok = false; G.cells[u].why = 8; }
    }

    std::vector<Patch> patches;
    size_t      dropped_cells = 0;   // the largest top left planar, and why (for the message)
    const char *dropped_why   = nullptr;
    int         next_band     = 1;
    // A band's deepest point stays within max height of the band's host: host_ref <= smax + h.
    const double band_h = P.max_height - P.T - P.h;
    // Head clearance, per curved layer. When curved layer k prints over cell p the nozzle tip is
    // at S_p - (n-1-k) h, and everything printed before must stay under the head: the tip face
    // (radius tip_r) and, beyond it, the envelope rising at env_tan. Already printed are planar
    // columns (to S, capped by the host layer), the planar part of shell columns
    // (up to S - (n - kmin) h + h/2) and their curved layers below k, and the whole shell of every top printed
    // earlier (a lower band). Only spans whose bottom is under the host count (a roof over a deck is not printed
    // when the deck's shell is). kmin only grows, so this settles; a cell where even the top curved layer collides
    // leaves the patch.
    //
    // "Everything printed stays under the head" is max_q [top(q) - env_tan * max(0, |pq| - tip_r)] <= tip(p)
    // + allow: a grey-scale dilation of the printed-height field by a flat disk (the tip face) and then a
    // cone. Both are O(N): a small max filter and a two-pass chamfer cone. The (1, sqrt 2) chamfer
    // distance overestimates Euclidean distance by at most 8.24 %, so the cone slope is divided by
    // that to stay conservative. Up to h/2 (+0.05) of contact is accepted: the first curved layer over
    // a column sits as little as h/2 above its planar top by design, and a stair corner stands up to
    // h/2 above S. Without it a ramp could never step by one layer inside the tip face.
    //
    // Shingled seams between height bands: cov_of[u] is the later top that prints node u's curved layers from its kmax
    // up (u keeps its own top's layers below kmax). For that later top the node is one of its own, starting on the
    // earlier top's layer kmax - 1; for everything else the earlier top stands at S - (n - kmax) h until the later one
    // prints. A handed-over layer that collides goes back to the earlier top (kmax rises), so this settles too.
    std::vector<int> cov_of(NN, -1);
    int probe_node = -1;
    if (const char *pr = std::getenv("NPTOP_PROBE_NODE")) {
        // "x,y" in the slice frame (mm): the topmost node of that column
        double px = 0, py = 0;
        if (std::sscanf(pr, "%lf,%lf", &px, &py) == 2) {
            const int a = int(std::lround((px - G.x0) / G.g)), b = int(std::lround((py - G.y0) / G.g));
            if (G.inside(a, b) && G.first(a, b) < G.last(a, b)) probe_node = G.first(a, b);
        }
    }
    double clr_ms = 0.; size_t clr_calls = 0, clr_passes = 0;
    // pass[k][node]: curved layer k clears at that node (pass_cov: printed by the top it is handed over to). Kept between
    // calls: a warm call (after the shingle) starts from the settled state and only redoes the hosts near what changed.
    std::vector<std::vector<char>> pass, pass_cov;
    auto clearance = [&](const std::vector<int> *warm_changed) {
        bool warm = warm_changed != nullptr;
        const auto    t_clr0   = std::chrono::steady_clock::now();
        ++ clr_calls;
        const float   kNone    = -1e30f;
        const double  allow    = 0.5 * P.h + 0.05;
        // The top curved layer may brush a planar stair corner by up to a layer: a stair stands up to
        // h/2 above S, plus the rise of the surface across one cell (0.12 mm at 25 deg). With only h/2
        // the top layer next to any planar cell fails, and the exclusion cascades downhill.
        const double  allow_top = P.h + 0.05;
        const double  tan_eff  = P.env_tan / 1.0824;
        const float   step1    = float(tan_eff * G.g), step2 = float(tan_eff * G.g * std::sqrt(2.));
        // per neighbour (da, db) = the column the value comes from, i.e. the direction from here toward it
        auto step_to = [&](int da, int db) {
            return P.tilt > 0. ? float(P.tan_dir(P.env_deg, da, db) / 1.0824 * G.g * std::hypot(double(da), double(db)))
                               : (da != 0 && db != 0 ? step2 : step1);
        };
        const int     flat_r   = int(std::floor(P.tip_r / G.g + 1e-9));
        const size_t  N        = size_t(G.ncols());
        const bool    any_cov  = std::any_of(cov_of.begin(), cov_of.end(), [](int v) { return v >= 0; });
        std::vector<double> hosts;
        for (const Patch &p : patches) hosts.push_back(p.host_ref);
        std::sort(hosts.begin(), hosts.end());
        hosts.erase(std::unique(hosts.begin(), hosts.end()), hosts.end());
        std::vector<float> top(N, kNone), flat(N, kNone), dil(N, kNone), rel(N, kNone), flat_rel(N, kNone);
        // Per host: the nodes it checks and the columns that can reach them. Everything printed when a host prints stands
        // at most at H, and a column farther than the tip face plus (H - lowest tip) / cone slope cannot reach the head.
        struct HostBox { int a0, a1, b0, b1; std::vector<int> nodes; };
        std::vector<HostBox> boxes(hosts.size(), HostBox{ G.nx, -1, G.ny, -1, {} });
        {
            std::vector<double> tip_lo(hosts.size(), 1e30);
            auto host_of = [&](double H) { return size_t(std::lower_bound(hosts.begin(), hosts.end(), H) - hosts.begin()); };
            for (size_t u = 0; u < NN; ++ u) {
                const Cell &c = G.cells[u];
                if (! c.ok || c.patch < 0)
                    continue;
                for (int pass_i = 0; pass_i < 2; ++ pass_i) {
                    const int pp = pass_i == 0 ? c.patch : cov_of[u];
                    if (pp < 0) continue;
                    HostBox &hb = boxes[host_of(patches[pp].host_ref)];
                    hb.nodes.push_back(int(u));
                    hb.a0 = std::min(hb.a0, G.ci(c)); hb.a1 = std::max(hb.a1, G.ci(c));
                    hb.b0 = std::min(hb.b0, G.cj(c)); hb.b1 = std::max(hb.b1, G.cj(c));
                    tip_lo[host_of(patches[pp].host_ref)] = std::min(tip_lo[host_of(patches[pp].host_ref)], double(c.s) - (P.n - 1) * P.h);
                }
            }
            for (size_t hi = 0; hi < hosts.size(); ++ hi) {
                HostBox &hb = boxes[hi];
                if (hb.nodes.empty()) continue;
                const int reach = flat_r + 2 + int(std::ceil(std::max(0., hosts[hi] - tip_lo[hi] + allow_top) / (tan_eff * G.g)));
                hb.a0 = std::max(0, hb.a0 - reach); hb.a1 = std::min(G.nx - 1, hb.a1 + reach);
                hb.b0 = std::max(0, hb.b0 - reach); hb.b1 = std::min(G.ny - 1, hb.b1 + reach);
            }
        }
        // Hosts to (re)do this sweep: all on a cold start; warm, those whose columns hold a node that changed.
        std::vector<char> dirty(hosts.size(), 1);
        if (! warm || pass.size() != size_t(P.n)) {
            warm = false;
            pass.assign(P.n, std::vector<char>(NN, 0));
        }
        if (any_cov && pass_cov.size() != size_t(P.n)) pass_cov.assign(P.n, std::vector<char>(NN, 0));
        auto mark_dirty = [&](const std::vector<int> &changed) {
            for (size_t hi = 0; hi < hosts.size(); ++ hi) {
                const HostBox &bx = boxes[hi];
                dirty[hi] = 0;
                for (int u : changed) {
                    const int a = G.ci(G.cells[u]), b = G.cj(G.cells[u]);
                    if (a >= bx.a0 && a <= bx.a1 && b >= bx.b0 && b <= bx.b1) { dirty[hi] = 1; break; }
                }
            }
        };
        if (warm)
            mark_dirty(*warm_changed);
        for (int sweep = 0; sweep < 4 * P.n + 4; ++ sweep) {
            for (size_t hi = 0; hi < hosts.size(); ++ hi) {
                const double   H  = hosts[hi];
                const HostBox &bx = boxes[hi];
                if (! dirty[hi] || bx.nodes.empty())
                    continue;
                for (int k = 0; k < P.n; ++ k) {
                    ++ clr_passes;
                    tbb::parallel_for(tbb::blocked_range<int>(bx.b0, bx.b1 + 1), [&](const tbb::blocked_range<int> &rb) {
                        for (int bb = rb.begin(); bb < rb.end(); ++ bb)
                        for (int aa = bx.a0; aa <= bx.a1; ++ aa) {
                            const size_t ci = size_t(bb) * G.nx + aa;
                            float t = kNone, rl = kNone;
                            for (int u = G.head[ci]; u < G.head[ci] + G.cnt[ci]; ++ u) {
                                const Cell &q = G.cells[u];
                                if (! q.hit || double(q.bot) >= H - 1e-6)
                                    continue; // nothing of this span is printed yet
                                // a held island: printed up to its pause so far
                                const double cap = H <= double(colcap_ref[ci]) + 1e-6 ? double(colcap[ci]) : 1e30;
                                if (double(q.bot) >= cap - 1e-6)
                                    continue;
                                if (q.ok && q.patch >= 0 && q.kmin < P.n) {
                                    const double hr = patches[q.patch].host_ref;
                                    const int    cb = cov_of[u];
                                    const double hb = cb >= 0 ? patches[cb].host_ref : 0.;
                                    if (hr == H || (cb >= 0 && hb == H)) {
                                        double v;
                                        if (hr == H) {
                                            v = q.s - (P.n - q.kmin) * P.h + 0.5 * P.h;
                                            if (q.kmin <= k - 1)
                                                v = std::max(v, double(q.s) - (P.n - std::min(k, q.kmax)) * P.h);
                                        } else {
                                            // handed over: the earlier top's curved layers up to kmax - 1, then this top's
                                            v = double(q.s) - (P.n - q.kmax) * P.h;
                                            if (q.kmax <= k - 1)
                                                v = std::max(v, double(q.s) - (P.n - k) * P.h);
                                        }
                                        // Shell columns are measured against the surface plane under the tip
                                        // (rel, exact within the tip face); the cone beyond takes them lowered
                                        // by the surface's own rise across the tip face.
                                        rl = std::max(rl, float(v - q.s));
                                        t  = std::max(t, float(v - P.tip_r * std::tan(std::min(double(q.slope), P.max_angle) * M_PI / 180.)));
                                    } else if (hr < H)
                                        // a top printed earlier: its whole shell (less what a later top prints there)
                                        t = std::max(t, float(cb >= 0 && hb < H ? double(q.s) : double(q.s) - (P.n - q.kmax) * P.h));
                                    else
                                        t = std::max(t, float(std::min(double(q.s) - (P.n - q.kmin) * P.h + 0.5 * P.h, H))); // later: its planar part so far
                                } else
                                    t = std::max(t, float(std::min({ double(q.s) + 0.5 * P.h, H, cap })));
                            }
                            top[ci] = t;
                            rel[ci] = rl;
                        }
                    });
                    // Flat disk max filter (tip face).
                    tbb::parallel_for(tbb::blocked_range<int>(bx.b0, bx.b1 + 1), [&](const tbb::blocked_range<int> &r) {
                        for (int b = r.begin(); b < r.end(); ++ b)
                            for (int a = bx.a0; a <= bx.a1; ++ a) {
                                float m = kNone, mr = kNone;
                                for (int dj = -flat_r; dj <= flat_r; ++ dj)
                                    for (int di = -flat_r; di <= flat_r; ++ di) {
                                        if (di * di + dj * dj > flat_r * flat_r) continue;
                                        const int qa = a + di, qb = b + dj;
                                        if (qa < bx.a0 || qa > bx.a1 || qb < bx.b0 || qb > bx.b1) continue;
                                        m  = std::max(m, top[size_t(qb) * G.nx + qa]);
                                        mr = std::max(mr, rel[size_t(qb) * G.nx + qa]);
                                    }
                                flat[size_t(b) * G.nx + a]    = m;
                                flat_rel[size_t(b) * G.nx + a] = mr;
                            }
                    });
                    // Chamfer cone dilation, forward then backward.
                    auto at = [&](int a, int b) -> float & { return dil[size_t(b) * G.nx + a]; };
                    for (int b = bx.b0; b <= bx.b1; ++ b)
                        std::copy(flat.begin() + size_t(b) * G.nx + bx.a0, flat.begin() + size_t(b) * G.nx + bx.a1 + 1, dil.begin() + size_t(b) * G.nx + bx.a0);
                    for (int b = bx.b0; b <= bx.b1; ++ b)
                        for (int a = bx.a0; a <= bx.a1; ++ a) {
                            float &v = at(a, b);
                            if (a > bx.a0)                   v = std::max(v, at(a - 1, b) - step_to(-1, 0));
                            if (b > bx.b0)                   v = std::max(v, at(a, b - 1) - step_to(0, -1));
                            if (a > bx.a0 && b > bx.b0)      v = std::max(v, at(a - 1, b - 1) - step_to(-1, -1));
                            if (a < bx.a1 && b > bx.b0)      v = std::max(v, at(a + 1, b - 1) - step_to(1, -1));
                        }
                    for (int b = bx.b1; b >= bx.b0; -- b)
                        for (int a = bx.a1; a >= bx.a0; -- a) {
                            float &v = at(a, b);
                            if (a < bx.a1)                   v = std::max(v, at(a + 1, b) - step_to(1, 0));
                            if (b < bx.b1)                   v = std::max(v, at(a, b + 1) - step_to(0, 1));
                            if (a < bx.a1 && b < bx.b1)      v = std::max(v, at(a + 1, b + 1) - step_to(1, 1));
                            if (a > bx.a0 && b < bx.b1)      v = std::max(v, at(a - 1, b + 1) - step_to(-1, 1));
                        }
                    for (int ui : bx.nodes) {
                        const size_t u = size_t(ui);
                        const Cell &c = G.cells[u];
                        if (! c.ok || c.patch < 0)
                            continue;
                        const bool own = patches[c.patch].host_ref == H;
                        const bool cov = cov_of[u] >= 0 && patches[cov_of[u]].host_ref == H;
                        if (! own && ! cov)
                            continue;
                        const double tip = c.s - (P.n - 1 - k) * P.h;
                        const double hi  = std::max(double(dil[c.col]), flat_rel[c.col] > kNone ? c.s + double(flat_rel[c.col]) : -1e30);
                        const bool   ok  = hi - tip <= (k == P.n - 1 ? allow_top : allow);
                        if (own) pass[k][u] = k >= c.kmax || ok; // layers from kmax up are another top's
                        if (cov) pass_cov[k][u] = ok;
                        if (probe_node == int(u))
                            std::fprintf(stderr, "np probe node %zu sweep %d H %.3f k %d: s %.3f tip %.3f dil %.3f flat %.3f top %.3f rel %.3f kmin %d kmax %d cov %d -> %d\n", u, sweep, H, k,
                                         c.s, tip, dil[c.col], flat[c.col], top[c.col], flat_rel[c.col], c.kmin, c.kmax, int(cov), int(ok));
                    }
                }
            }
            std::vector<int> changed;
            for (size_t u = 0; u < NN; ++ u) {
                Cell &c = G.cells[u];
                if (! c.ok || c.patch < 0 || c.kmin >= P.n)
                    continue;
                int k = c.kmin;
                while (k < P.n && ! pass[k][u]) ++ k;
                if (k != c.kmin) { c.kmin = k; changed.push_back(int(u)); }
            }
            if (any_cov)
                for (size_t u = 0; u < NN; ++ u) {
                    Cell &c = G.cells[u];
                    if (cov_of[u] < 0)
                        continue;
                    int kt = c.kmax;
                    for (int k = P.n - 1; k >= c.kmax; -- k)
                        if (! pass_cov[k][u]) { kt = k + 1; break; }
                    if (kt != c.kmax) {
                        changed.push_back(int(u));
                        c.kmax = std::min(kt, P.n);
                        if (c.kmax >= P.n) cov_of[u] = -1;
                    }
                }
            if (changed.empty())
                break;
            // Only hosts whose columns hold a node that changed can come out differently next sweep.
            mark_dirty(changed);
        }
        clr_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_clr0).count();
    };

    // ---- hold-back islands (0.1.9). A separate island of a layer (one ExPolygon of its slices) that stands next to a top but
    // holds none of a top's own material (Benchy's chimney over the roof, the cabin and the box on the deck) pauses: from the
    // first layer where it is safe to print it after the top's shell (the shells and the planar material printed by then stay
    // under it, nozzle cone included) up to the top's host it is printed after the shell, flat, on its own layer below.
    // Printing it first made the shell clear it: a planar ring all around it. Only the cells the paused island is still too
    // close to stay planar. The tops are found again with the held islands printed only up to their pause (colcap), until the
    // held set settles; if it does not, nothing is held (the old way).
    struct HeldIsland { int pid; int layer; ExPolygon area; std::vector<int> cols; double host_ref; };
    std::vector<HeldIsland> held_islands;
    // Grid columns inside an ExPolygon.
    auto island_cols = [&G](const ExPolygon &ex) {
        std::vector<int> cols;
        const BoundingBox bb = get_extents(ex.contour);
        const int a0 = std::max(0, int(std::floor((unscale<double>(bb.min.x()) - G.x0) / G.g))), a1 = std::min(G.nx - 1, int(std::ceil((unscale<double>(bb.max.x()) - G.x0) / G.g)));
        const int b0 = std::max(0, int(std::floor((unscale<double>(bb.min.y()) - G.y0) / G.g))), b1 = std::min(G.ny - 1, int(std::ceil((unscale<double>(bb.max.y()) - G.y0) / G.g)));
        for (int b = b0; b <= b1; ++ b)
            for (int a = a0; a <= a1; ++ a)
                if (ex.contains(Point::new_scale(G.cx(a), G.cy(b)))) cols.push_back(b * G.nx + a);
        return cols;
    };
    // Grid columns as area: one cell square per column (grown a micron so neighbours merge, as cells_to_expolygons).
    auto cols_area = [&G](const std::vector<int> &cols) {
        Polygons      rects;
        const coord_t half = coord_t(scale_(G.g * 0.5)) + 1000;
        rects.reserve(cols.size());
        for (int c : cols) {
            const coord_t x = coord_t(scale_(G.cx(c % G.nx))), y = coord_t(scale_(G.cy(c / G.nx)));
            rects.emplace_back(Points{ Point(x - half, y - half), Point(x + half, y - half), Point(x + half, y + half), Point(x - half, y + half) });
        }
        return union_ex(rects);
    };
    // The nozzle around printed material: each column's height spread over the tip face, then down the hotend cone.
    const float  kHoldNone    = -1e30f;
    const double hold_tan_eff = P.env_tan / 1.0824;
    const float  hold_step1   = float(hold_tan_eff * G.g), hold_step2 = float(hold_tan_eff * G.g * std::sqrt(2.));
    auto hold_step_to = [&](int da, int db) {
        return P.tilt > 0. ? float(P.tan_dir(P.env_deg, da, db) / 1.0824 * G.g * std::hypot(double(da), double(db)))
                           : (da != 0 && db != 0 ? hold_step2 : hold_step1);
    };
    const int hold_flat_r = int(std::floor(P.tip_r / G.g + 1e-9));
    auto hold_spread = [&](const std::vector<float> &top) {
        const int          flat_r = hold_flat_r;
        std::vector<float> flat(top.size(), kHoldNone);
        for (int b = 0; b < G.ny; ++ b)
            for (int a = 0; a < G.nx; ++ a) {
                float m = kHoldNone;
                for (int db = -flat_r; db <= flat_r; ++ db)
                    for (int da = -flat_r; da <= flat_r; ++ da)
                        if (da * da + db * db <= flat_r * flat_r && G.inside(a + da, b + db)) m = std::max(m, top[size_t(b + db) * G.nx + a + da]);
                flat[size_t(b) * G.nx + a] = m;
            }
        auto at = [&](int a, int b) -> float & { return flat[size_t(b) * G.nx + a]; };
        for (int b = 0; b < G.ny; ++ b)
            for (int a = 0; a < G.nx; ++ a) {
                float &v = at(a, b);
                if (a > 0)                  v = std::max(v, at(a - 1, b) - hold_step_to(-1, 0));
                if (b > 0)                  v = std::max(v, at(a, b - 1) - hold_step_to(0, -1));
                if (a > 0 && b > 0)         v = std::max(v, at(a - 1, b - 1) - hold_step_to(-1, -1));
                if (a < G.nx - 1 && b > 0)  v = std::max(v, at(a + 1, b - 1) - hold_step_to(1, -1));
            }
        for (int b = G.ny - 1; b >= 0; -- b)
            for (int a = G.nx - 1; a >= 0; -- a) {
                float &v = at(a, b);
                if (a < G.nx - 1)                 v = std::max(v, at(a + 1, b) - hold_step_to(1, 0));
                if (b < G.ny - 1)                 v = std::max(v, at(a, b + 1) - hold_step_to(0, 1));
                if (a < G.nx - 1 && b < G.ny - 1) v = std::max(v, at(a + 1, b + 1) - hold_step_to(1, 1));
                if (a > 0 && b < G.ny - 1)        v = std::max(v, at(a - 1, b + 1) - hold_step_to(-1, 1));
            }
        return flat;
    };
    // A held layer prints beside finished curved tops: the nozzle may not touch them (the shell's own h + 0.05 brush of a
    // planar stair corner dragged held walls 0.25 mm through tt_5's finished top).
    const double hold_allow_top = 0.05;
    // The candidates of the last find_held_islands (layer, columns), in layer order: what of them is not held prints normally.
    std::vector<std::pair<int, std::vector<int>>> hold_cands;
    // A held island goes down after its top's shell, beside everything printed normally up to the host, the candidates'
    // material that is not held included (the rest of a partly held piece, a piece that was not safe). The selection's
    // fields leave all candidates out; here a held island is trimmed where that material (and what islands held for an
    // earlier top left), spread by the tip face and the cone, stands over it. Then held layers must run without a gap up to
    // the host: one with material of its own column printed normally over it would float, so it is printed normally too
    // (from the top down). Both steps only take columns away, so this ends; it repeats until neither changes anything.
    // NPTOP_PARTIAL_HOLD=1: hold the safe part of a piece (cut on the analysis grid; experimental, see find_held_islands)
    const bool partial_hold = std::getenv("NPTOP_PARTIAL_HOLD") != nullptr;
    auto settle_held = [&](std::vector<HeldIsland> &res) {
        const size_t NC = size_t(G.ncols());
        for (int it = 0; it < 256; ++ it) {
            bool changed = false;
            std::map<double, int> hosts; // host_ref -> host layer
            for (const HeldIsland &h : res) hosts.emplace(h.host_ref, patches[h.pid].host);
            for (const auto &[H, hl] : hosts) {
                // per layer: the islands not printed yet when H's held islands go down (held for H or a later top)
                std::map<int, std::vector<const HeldIsland *>> held_at;
                for (const HeldIsland &h : res) if (h.host_ref >= H - 1e-6) held_at[h.layer].push_back(&h);
                std::vector<float> top(NC, kHoldNone);
                std::vector<int>   stamp(NC, -1);
                int  stamped = -1;
                bool any     = false;
                for (const auto &[l, cols] : hold_cands) {
                    if (l > hl) continue;
                    if (l != stamped) {
                        stamped = l;
                        if (auto hit = held_at.find(l); hit != held_at.end())
                            for (const HeldIsland *h : hit->second) for (int c : h->cols) stamp[c] = l;
                    }
                    const float z = float(layers[l]->print_z);
                    for (int c : cols)
                        if (stamp[c] != l && top[c] < z) { top[c] = z; any = true; }
                }
                if (! any) continue;
                const std::vector<float> F = hold_spread(top);
                std::vector<HeldIsland> next;
                for (HeldIsland &h : res) {
                    if (std::abs(h.host_ref - H) > 1e-6) { next.push_back(std::move(h)); continue; }
                    const double     thr = layers[h.layer]->print_z + hold_allow_top;
                    std::vector<int> bad;
                    for (int c : h.cols) if (double(F[c]) > thr) bad.push_back(c);
                    if (bad.empty()) { next.push_back(std::move(h)); continue; }
                    changed = true;
                    // without partial holds an island that is not safe everywhere is not held at all (no grid-cut walls)
                    if (bad.size() == h.cols.size() || ! partial_hold) continue;
                    for (ExPolygon &e : diff_ex(ExPolygons{ h.area }, cols_area(bad))) {
                        if (std::abs(e.area()) * SCALING_FACTOR * SCALING_FACTOR < 0.5) continue;
                        std::vector<int> cols = island_cols(e);
                        if (! cols.empty()) next.push_back({ h.pid, h.layer, std::move(e), std::move(cols), h.host_ref });
                    }
                }
                res = std::move(next);
            }
            std::sort(res.begin(), res.end(), [](const HeldIsland &x, const HeldIsland &y) { return x.layer > y.layer; });
            std::vector<char> keep(res.size(), 1);
            for (size_t i = 0; i < res.size(); ++ i) {
                const HeldIsland &h = res[i];
                if (h.layer >= patches[h.pid].host) continue;
                // the layer above: held parts of the same top
                ExPolygons above_held;
                for (size_t j = 0; j < res.size(); ++ j)
                    if (keep[j] && std::abs(res[j].host_ref - h.host_ref) < 1e-6 && res[j].layer == h.layer + 1) above_held.push_back(res[j].area);
                if (! intersection_ex(ExPolygons{ h.area }, diff_ex(layers[h.layer + 1]->lslices, above_held)).empty()) { keep[i] = 0; changed = true; }
            }
            std::vector<HeldIsland> kept;
            for (size_t i = 0; i < res.size(); ++ i) if (keep[i]) kept.push_back(std::move(res[i]));
            res = std::move(kept);
            if (! changed) return;
        }
        if (std::getenv("NPTOP_DEBUG") != nullptr)
            std::fprintf(stderr, "np hold settle: no stable held set after 256 rounds (%zu islands left), holding nothing\n", res.size());
        res.clear(); // did not settle: hold nothing rather than guess
    };
    auto find_held_islands = [&]() {
        std::vector<HeldIsland> res;
        hold_cands.clear();
        if (! P.hold_back)
            return res;
        const int    NC        = G.ncols();
        const double tan_eff   = hold_tan_eff;
        const double allow_top = hold_allow_top;
        const float  kNone     = kHoldNone;
        const int    near_r    = int(std::ceil((P.tip_r + 2.) / G.g));
        if (patches.empty()) return res;
        // in_reach[pid]: columns within reach of the top
        std::vector<std::vector<char>> in_reach(patches.size());
        for (size_t pid = 0; pid < patches.size(); ++ pid) {
            in_reach[pid].assign(size_t(NC), 0);
            for (int u : patches[pid].cells) {
                const Cell &c = G.cells[u];
                if (! c.ok || c.patch != int(pid)) continue;
                const int a = G.ci(c), b = G.cj(c);
                for (int db = -near_r; db <= near_r; ++ db)
                    for (int da = -near_r; da <= near_r; ++ da)
                        if (G.inside(a + da, b + db) && da * da + db * db <= near_r * near_r) in_reach[pid][size_t(b + db) * G.nx + a + da] = 1;
            }
        }
        // Candidate islands: per layer up to the highest host, the islands of what is left of the slice without the tops' own
        // material (columns whose gentle top -- in a top, or left planar by the clearance or the max height, not a flat one --
        // is at or above the layer), near some top. (A chimney touches the planar cells the clearance left beside it on the
        // uphill side: as one island with the roof it was never held, and the cells stayed planar because it was not.)
        struct Cand { int layer; ExPolygon area; std::vector<int> cols; std::vector<int> pids; };
        std::vector<Cand> cands;
        int max_host = -1;
        for (const Patch &p : patches) max_host = std::max(max_host, p.host);
        for (int l = 1; l <= max_host && l < int(layers.size()); ++ l) {
            const double sz = layers[l]->slice_z, pz = layers[l]->print_z;
            // (only surfaces within the max height over the layer: a chimney's rim far above is the chimney's, not a top's)
            const double reach_up = P.max_height;
            const ExPolygons topmat = cells_to_expolygons(G, [sz, reach_up](const Cell &c) {
                return (c.ok && c.patch >= 0) || ((c.why == 6 || c.why == 7 || c.why == 8) && c.hit) ?
                    double(c.s) >= sz - 1e-6 && double(c.s) <= sz + reach_up && double(c.bot) < sz : false;
            });
            const ExPolygons pieces = topmat.empty() ? layers[l]->lslices : diff_ex(layers[l]->lslices, topmat);
            if (const char *pr = std::getenv("NPTOP_DEBUG_HOLD_L")) {
                // "layer,x,y": which slice / top-material / piece holds the point
                int pl = -1; double px = 0, py = 0;
                if (std::sscanf(pr, "%d,%lf,%lf", &pl, &px, &py) == 3 && pl == l) {
                    const Point pp = Point::new_scale(px, py);
                    auto in = [&pp](const ExPolygons &e) { int i = 0; for (const ExPolygon &x : e) { if (x.contains(pp)) return i; ++ i; } return -1; };
                    std::fprintf(stderr, "np hold probe layer %d: slice %d topmat %d piece %d of %zu\n", l, in(layers[l]->lslices), in(topmat), in(pieces), pieces.size());
                    for (const ExPolygon &x : pieces) {
                        const BoundingBox b = get_extents(x.contour);
                        if (b.contains(pp) || (unscale<double>(b.min.x()) < px + 5 && unscale<double>(b.max.x()) > px - 5 && unscale<double>(b.min.y()) < py + 5 && unscale<double>(b.max.y()) > py - 5))
                            std::fprintf(stderr, "   piece area %.2f bbox (%.2f,%.2f)-(%.2f,%.2f) holes %zu\n", std::abs(x.area()) * SCALING_FACTOR * SCALING_FACTOR,
                                         unscale<double>(b.min.x()), unscale<double>(b.min.y()), unscale<double>(b.max.x()), unscale<double>(b.max.y()), x.holes.size());
                    }
                }
            }
            for (const ExPolygon &ex : pieces) {
                if (std::abs(ex.area()) * SCALING_FACTOR * SCALING_FACTOR < 0.5)
                    continue;
                std::vector<int> cols = island_cols(ex);
                if (cols.empty()) continue;
                hold_cands.emplace_back(l, cols);
                // (No "highest top in the column" test: the tops' own material near the layer is already taken out above,
                // and a cabin wall whose column ends in the roof's curved top 20 mm higher is not roof material at the deck.)
                std::vector<int> pids;
                for (size_t pid = 0; pid < patches.size(); ++ pid) {
                    const Patch &p = patches[pid];
                    if (l > p.host || pz <= p.smin - P.T) continue;
                    for (int c : cols) if (in_reach[pid][c]) { pids.push_back(int(pid)); break; }
                }
                if (! pids.empty()) cands.push_back({ l, ex, std::move(cols), std::move(pids) });
            }
        }
        if (cands.empty()) return res;
        std::vector<char> cand_col(size_t(NC), 0);
        // Inside a candidate's outer outline, holes included: a chimney's bore widens under its rim, and the down-ray there reads
        // solid rim-to-roof material that the slices do not have.
        for (const Cand &cd : cands)
            for (int c : island_cols(ExPolygon(cd.area.contour))) cand_col[c] = 1;
        // Per host: everything printed when a held island goes down after that host's shells (shells of tops up to it, planar
        // material up to the host except the candidates), dilated by the tip face and the cone.
        std::map<double, std::vector<float>> printed; // host_ref -> dilated field
        auto field_at = [&](double H) -> const std::vector<float> & {
            auto it = printed.find(H);
            if (it != printed.end()) return it->second;
            std::vector<float> top(size_t(NC), kNone);
            for (const Cell &q : G.cells) {
                if (! q.hit) continue;
                if (q.ok && q.patch >= 0) {
                    if (patches[q.patch].host_ref <= H + 1e-6) top[q.col] = std::max(top[q.col], q.s);
                    else top[q.col] = std::max(top[q.col], float(std::min(double(q.s) - P.T + 0.5 * P.h, H)));
                } else if (! cand_col[q.col] && double(q.bot) < H - 1e-6)
                    top[q.col] = std::max(top[q.col], float(std::min(double(q.s) + 0.5 * P.h, H)));
            }
            std::vector<float> flat = hold_spread(top);
            if (const char *pr = std::getenv("NPTOP_DEBUG_HOLD")) {
                // the source of the field at a probe column: "x,y"
                double px = 0, py = 0;
                if (std::sscanf(pr, "%lf,%lf", &px, &py) == 2) {
                    const int pa = int(std::lround((px - G.x0) / G.g)), pb = int(std::lround((py - G.y0) / G.g));
                    double best = -1e30; int bc = -1;
                    for (int c = 0; c < NC; ++ c) {
                        if (top[c] <= kNone) continue;
                        const double d = G.g * std::hypot(double(c % G.nx - pa), double(c / G.nx - pb));
                        const double v = double(top[c]) - tan_eff * std::max(0., d - P.tip_r);
                        if (v > best) { best = v; bc = c; }
                    }
                    if (bc >= 0) {
                        std::fprintf(stderr, "np hold field H %.3f at (%.2f, %.2f): from (%.2f, %.2f) top %.3f cand %d; nodes:", H, px, py, G.cx(bc % G.nx), G.cy(bc / G.nx),
                                     double(top[bc]), int(cand_col[bc]));
                        for (int u = G.head[bc]; u < G.head[bc] + G.cnt[bc]; ++ u)
                            std::fprintf(stderr, " [s %.3f bot %.3f ok %d patch %d]", double(G.cells[u].s), double(G.cells[u].bot), int(G.cells[u].ok), G.cells[u].patch);
                        std::fprintf(stderr, "\n");
                    }
                }
            }
            return printed.emplace(H, std::move(flat)).first->second;
        };
        // Each candidate goes to the earliest top it is safe to follow. A piece no top is safe for as a whole holds the part that
        // is safe for the earliest top it can follow (Benchy's wheelhouse walls join the hull ring at the deck, and one ring
        // column beside the deck's highest point rejected the walls too); the rest prints normally.
        const bool dbg = std::getenv("NPTOP_DEBUG_HOLD") != nullptr;
        for (Cand &cd : cands) {
            std::sort(cd.pids.begin(), cd.pids.end(), [&](int x, int y) { return patches[x].host_ref < patches[y].host_ref; });
            const double pz = layers[cd.layer]->print_z;
            bool done = false;
            for (int pid : cd.pids) {
                const std::vector<float> &D = field_at(patches[pid].host_ref);
                bool safe = true;
                int  worst = -1;
                for (int c : cd.cols) if (worst < 0 || D[c] > D[worst]) worst = c;
                safe = worst < 0 || double(D[worst]) <= pz + allow_top;
                if (dbg && cd.cols.size() >= 20)
                    std::fprintf(stderr, "np hold cand layer %d z %.3f cols %zu top %d host %d: field max %.3f at (%.2f, %.2f) -> %s\n", cd.layer, pz, cd.cols.size(), pid,
                                 patches[pid].host, worst >= 0 ? double(D[worst]) : -1., worst >= 0 ? G.cx(worst % G.nx) : 0., worst >= 0 ? G.cy(worst / G.nx) : 0.,
                                 safe ? "held" : "not safe");
                if (safe) { res.push_back({ pid, cd.layer, cd.area, cd.cols, patches[pid].host_ref }); done = true; break; }
            }
            if (done) continue;
            // Partial holds cut a piece along the 0.25 mm analysis grid: the walls then follow that staircase and loops are
            // cut mid-wall (jagged, fragmented layers on a Benchy bow, 2026-11-02). Off until the cut follows the part.
            if (! partial_hold) continue;
            for (int pid : cd.pids) {
                const std::vector<float> &D = field_at(patches[pid].host_ref);
                std::vector<int> bad;
                for (int c : cd.cols) if (double(D[c]) > pz + allow_top) bad.push_back(c);
                if (bad.size() == cd.cols.size()) continue;
                // the safe part, without slivers, and only pieces of it within reach of the top
                size_t n_held = 0, n_cols = 0;
                for (ExPolygon &e : diff_ex(ExPolygons{ cd.area }, cols_area(bad))) {
                    if (std::abs(e.area()) * SCALING_FACTOR * SCALING_FACTOR < 0.5) continue;
                    std::vector<int> cols = island_cols(e);
                    if (cols.empty() || std::none_of(cols.begin(), cols.end(), [&](int c) { return in_reach[pid][c] != 0; })) continue;
                    n_cols += cols.size();
                    ++ n_held;
                    res.push_back({ pid, cd.layer, std::move(e), std::move(cols), patches[pid].host_ref });
                }
                if (dbg && cd.cols.size() >= 20)
                    std::fprintf(stderr, "np hold cand layer %d z %.3f cols %zu top %d host %d: safe part %zu cols in %zu pieces\n", cd.layer, pz, cd.cols.size(), pid,
                                 patches[pid].host, n_cols, n_held);
                if (n_held > 0) break;
            }
        }
        settle_held(res);
        return res;
    };
    auto set_caps = [&](const std::vector<HeldIsland> &hs) {
        std::fill(colcap.begin(), colcap.end(), 1e30f);
        std::fill(colcap_ref.begin(), colcap_ref.end(), -1e30f);
        for (const HeldIsland &h : hs) {
            const float below = h.layer > 0 ? float(layers[h.layer - 1]->print_z) : 0.f;
            for (int c : h.cols) {
                colcap[c]     = std::min(colcap[c], below);
                colcap_ref[c] = std::max(colcap_ref[c], float(patches[h.pid].host_ref));
            }
        }
    };
    const std::vector<Cell> cells0 = G.cells;
    size_t held_sig = size_t(-1);
    std::vector<HeldIsland> prev_held, final_set;
    bool final_pass = false;
    // Same island in two passes: same layer, same top (host), about the same columns.
    // (A partly held piece can leave several held parts on one layer: they must share most columns, not only their count.)
    auto same_island = [](const HeldIsland &x, const HeldIsland &y) {
        if (x.layer != y.layer || std::abs(x.host_ref - y.host_ref) >= 1e-6) return false;
        const double n = double(std::max(x.cols.size(), y.cols.size()));
        if (std::abs(double(x.cols.size()) - double(y.cols.size())) > 0.1 * n) return false;
        size_t common = 0; // island_cols lists columns in ascending order
        for (size_t i = 0, j = 0; i < x.cols.size() && j < y.cols.size();) {
            if (x.cols[i] < y.cols[j]) ++ i; else if (y.cols[j] < x.cols[i]) ++ j; else { ++ common; ++ i; ++ j; }
        }
        return double(common) >= 0.8 * n;
    };
    for (int hpass = 0; hpass < 5; ++ hpass) {
    if (hpass > 0) { G.cells = cells0; next_band = 1; dropped_cells = 0; dropped_why = nullptr; }
    for (int iter = 0; iter < 30; ++ iter) {
        for (Cell &c : G.cells) c.patch = -1;
        patches.clear();
        for (size_t u0 = 0; u0 < NN; ++ u0) {
            if (! G.cells[u0].ok || G.cells[u0].patch >= 0)
                continue;
            Patch p;
            const int id = int(patches.size());
            std::vector<int> stack{ int(u0) };
            G.cells[u0].patch = id;
            p.smin = p.smax = G.cells[u0].s;
            while (! stack.empty()) {
                const int u = stack.back();
                stack.pop_back();
                const Cell &cu = G.cells[u];
                p.cells.push_back(u);
                p.smin = std::min(p.smin, double(cu.s));
                p.smax = std::max(p.smax, double(cu.s));
                const int a = G.ci(cu), b = G.cj(cu);
                const int nb[4][2] = { {a + 1, b}, {a - 1, b}, {a, b + 1}, {a, b - 1} };
                for (const auto &q : nb)
                    if (G.inside(q[0], q[1]))
                        for (int v = G.first(q[0], q[1]); v < G.last(q[0], q[1]); ++ v) {
                            Cell &qc = G.cells[v];
                            // A cliff between two gentle cells (a tower wall one cell wide) separates two tops.
                            if (qc.ok && qc.patch < 0 && qc.band == cu.band && std::abs(qc.s - cu.s) <= dz_cliff) { qc.patch = id; stack.push_back(v); }
                        }
            }
            patches.push_back(std::move(p));
        }

        bool changed = false;
        for (Patch &p : patches) {
            auto drop_all = [&] { for (int u : p.cells) { G.cells[u].ok = false; G.cells[u].why = 5; } changed = true; };
            // A patch must span more than about one layer, or it is a flat top: leave it planar.
            if (p.smax - p.smin <= 0.75 * P.h) { drop_all(); continue; }
            // A speck of gentle surface (a fillet facet) is not a top surface of its own.
            if (double(p.cells.size()) * G.g * G.g < 2.) { drop_all(); continue; }
            // Host: the highest layer whose bottom is under the patch top.
            p.host = -1;
            for (int l = int(layers.size()) - 1; l >= 0; -- l)
                if (layers[l]->print_z - layers[l]->height < p.smax - 1e-6) { p.host = l; break; }
            if (p.host < 0) { drop_all(); continue; }
            p.host_ref = std::max(layers[p.host]->print_z, p.smax);
            // Depth: the nozzle may reach at most max_height under the highest printed material. A taller top is split
            // into height bands, each printed as soon as the planar part under it is done (its own host layer). A band
            // that still does not fit (its host stands higher than its top) is curved where it fits; its low part stays
            // planar like any other stairs.
            {
                const double s_floor = p.host_ref - P.max_height + P.T;
                if (p.smin < s_floor) {
                    if (band_h > 2. * P.h && p.smax - p.smin > band_h + 1e-6) {
                        const int    nb = int(std::ceil((p.smax - p.smin) / band_h - 1e-9));
                        const double bh = (p.smax - p.smin) / nb;
                        for (int u : p.cells)
                            G.cells[u].band = next_band + std::clamp(int((G.cells[u].s - p.smin) / bh), 0, nb - 1);
                        next_band += nb;
                        changed = true;
                        continue;
                    }
                    for (int u : p.cells)
                        if (G.cells[u].s < s_floor) { G.cells[u].ok = false; G.cells[u].why = 6; }
                    if (p.cells.size() > dropped_cells) { dropped_cells = p.cells.size(); dropped_why = "the top rises more than the maximum nonplanar height"; }
                    changed = true;
                    continue;
                }
            }
            // (0.1.9: a top may have the model over it or under it. The clearance analysis decides: material over a top
            // stands higher, so it prints later unless it is already in the way. Thin tops get as many walls as fit.)
        }
        if (changed) {
            if (iter == 29) patches.clear(); // did not settle: stay planar rather than guess
            continue;
        }

        for (Cell &c : G.cells) c.kmin = 0;
        clearance(nullptr);
        // Cells where even the top curved layer collides: when they lie lower than their top's highest point, an earlier
        // host may let them through (the rim beside a cabin, printed before the cabin rises next to it): they become a
        // band of their own, printed as soon as the planar part under them is done. Otherwise they stay planar.
        for (const Patch &p : patches) {
            bool new_band = false;
            for (int u : p.cells) {
                Cell &c = G.cells[u];
                if (! c.ok || c.kmin < P.n)
                    continue;
                changed = true;
                if (double(c.s) < p.smax - 2. * P.h && c.band >= 0) {
                    c.band   = next_band;
                    new_band = true;
                } else {
                    c.ok  = false;
                    c.why = 7;
                }
            }
            if (new_band)
                ++ next_band;
        }
        if (! changed)
            break;
        if (iter == 29)
            patches.clear();
    }
    if (patches.empty()) { held_islands.clear(); break; }
    if (final_pass) {
        // The tops were found with exactly these caps: keep these islands, each with the top of its host now (an island
        // whose top is gone prints normally; its cap only made that top's clearance stricter).
        for (HeldIsland &h : final_set) {
            h.pid = -1;
            for (size_t pid = 0; pid < patches.size(); ++ pid)
                if (std::abs(patches[pid].host_ref - h.host_ref) < 1e-6 && h.layer <= patches[pid].host) { h.pid = int(pid); break; }
        }
        final_set.erase(std::remove_if(final_set.begin(), final_set.end(), [](const HeldIsland &h) { return h.pid < 0; }), final_set.end());
        held_islands = std::move(final_set);
        break;
    }
    std::vector<HeldIsland> hs = find_held_islands();
    // (order-free: the islands of one layer come in any order)
    size_t sig = hs.size();
    for (const HeldIsland &h : hs) {
        size_t hh = size_t(h.layer) * 131u + h.cols.size() * 1000003u + size_t(patches[h.pid].host_ref * 1000.) * 7919u;
        for (int c : h.cols) hh = hh * 31u + size_t(c);
        sig += hh * 2654435761u;
    }
    if (std::getenv("NPTOP_DEBUG") != nullptr) {
        std::fprintf(stderr, "np hold pass %d: %zu tops, %zu held:", hpass, patches.size(), hs.size());
        for (const HeldIsland &h : hs) std::fprintf(stderr, " L%d/%zu@%.1f", h.layer, h.cols.size(), patches[h.pid].host_ref);
        std::fprintf(stderr, "\n");
    }
    if (hs.empty() && hpass == 0) break;                                            // nothing to hold
    if (hpass > 0 && sig == held_sig) { held_islands = std::move(hs); break; }     // settled: the caps used are these
    // (A partly held piece can reshape the tops a lot -- a rim ring held lets a banded deck become fewer tops -- so the held
    // set gets a few passes to settle before the fallback; pass 4 is then the final one.)
    if (hpass >= 3) {
        // Not settled (fringe pieces move between passes): keep the islands both passes hold, and find the tops once more
        // with exactly their caps.
        final_set.clear();
        for (HeldIsland &h : hs)
            if (std::any_of(prev_held.begin(), prev_held.end(), [&](const HeldIsland &q) { return same_island(h, q); }))
                final_set.push_back(std::move(h));
        // Held layers still run without a gap up to the host (a dropped fringe piece may have sat on one), and a dropped piece
        // prints normally beside the others: settle again.
        settle_held(final_set);
        set_caps(final_set);
        final_pass = true;
        continue;
    }
    prev_held = hs;
    held_sig  = sig;
    set_caps(hs);
    }
    if (std::getenv("NPTOP_DEBUG") != nullptr) {
        std::map<int, int> per_top;
        for (const HeldIsland &h : held_islands) ++ per_top[h.pid];
        std::fprintf(stderr, "np hold-back: %zu held island layers", held_islands.size());
        for (const auto &[pid, n] : per_top) std::fprintf(stderr, ", top %d (host %d): %d", pid, patches[pid].host, n);
        std::fprintf(stderr, "\n");
    }
    if (patches.empty()) {
        skip = dropped_why ? dropped_why : "no top surface is gentle enough";
        return stats;
    }
    // ---- shingled seams. Where two tops at the same height meet (two height bands of one slope), the earlier one hands
    // its upper curved layers over to the later one across a strip: a node at distance d from the later top keeps its
    // layers k < ceil(d / step), the later top prints the rest over it. The layers overlap like roof tiles instead of
    // butting along one line through the whole shell, and the later top's deeper layers no longer ramp away from a
    // full-height shell beside them. A step is the tip face's radius plus a cell: the nozzle printing the later top's
    // layer k at its edge must not have the earlier top's layer k + 1 (one layer higher) under its tip face (with 1.2 mm
    // for 4 layers every handed-over layer collided and went back). The clearance is redone with the strip; a
    // handed-over layer that collides goes back.
    const auto t_patches = std::chrono::steady_clock::now();
    size_t n_shingled = 0;
    {
        const double     step   = P.tip_r + G.g;
        const double     kStrip = (P.n - 1) * step;
        const int        R      = int(std::ceil(kStrip / G.g - 1e-9));
        const double     tmax   = std::tan(P.max_angle * M_PI / 180.);
        for (size_t u = 0; u < NN; ++ u) {
            Cell &a = G.cells[u];
            if (! a.ok || a.patch < 0)
                continue;
            const double hra = patches[a.patch].host_ref;
            const int    i = G.ci(a), j = G.cj(a);
            double best = 1e30;
            int    bp   = -1;
            for (int dj = -R; dj <= R; ++ dj)
                for (int di = -R; di <= R; ++ di) {
                    const double d = G.g * std::hypot(double(di), double(dj));
                    if (d > kStrip + 1e-9 || d >= best || ! G.inside(i + di, j + dj))
                        continue;
                    for (int v = G.first(i + di, j + dj); v < G.last(i + di, j + dj); ++ v) {
                        const Cell &b = G.cells[v];
                        // the same surface, continued by a top printed later
                        if (! b.ok || b.patch < 0 || b.patch == a.patch || patches[b.patch].host_ref <= hra + 1e-6 ||
                            std::abs(double(b.s) - double(a.s)) >
                                (P.tilt > 0. ? P.tan_dir(P.max_angle, (b.s >= a.s ? 1. : -1.) * di, (b.s >= a.s ? 1. : -1.) * dj) : tmax) * d + 0.05)
                            continue;
                        best = d;
                        bp   = b.patch;
                    }
                }
            if (bp < 0)
                continue;
            const int kt = std::clamp(int(std::ceil(best / step - 1e-9)), 1, P.n);
            if (kt >= P.n)
                continue;
            cov_of[u] = bp;
            a.kmax    = kt;
            ++ n_shingled;
        }
        if (n_shingled > 0) {
            std::vector<int> kmin0(NN);
            for (size_t u = 0; u < NN; ++ u) kmin0[u] = G.cells[u].kmin;
            // Only the tops on either side of a strip start over (kmin 0); the clearance redoes the hosts whose columns hold
            // one of their nodes and, sweep by sweep, those near what changed (Benchy: 6.4 s -> 1.2 s for all calls).
            {
                std::vector<char> strip_host(patches.size(), 0);
                for (size_t u = 0; u < NN; ++ u)
                    if (cov_of[u] >= 0) { strip_host[size_t(G.cells[u].patch)] = 1; strip_host[size_t(cov_of[u])] = 1; }
                std::vector<int> changed;
                for (size_t u = 0; u < NN; ++ u) {
                    Cell &c = G.cells[u];
                    if (c.ok && c.patch >= 0 && strip_host[size_t(c.patch)]) { c.kmin = 0; changed.push_back(int(u)); }
                }
                clearance(&changed);
            }
            // The strip only ever lowers what stands beside a top, so nothing that cleared before may fail now; if it
            // does anyway, keep the butt seams.
            bool worse = false;
            for (size_t u = 0; u < NN; ++ u) {
                const Cell &c = G.cells[u];
                if (c.ok && c.patch >= 0 && c.kmin >= P.n && kmin0[u] < P.n) worse = true;
            }
            n_shingled = 0;
            for (size_t u = 0; u < NN; ++ u) {
                Cell &c = G.cells[u];
                if (worse) { c.kmin = kmin0[u]; c.kmax = P.n; cov_of[u] = -1; }
                n_shingled += cov_of[u] >= 0;
            }
            if (std::getenv("NPTOP_DEBUG") != nullptr)
                std::fprintf(stderr, "np shingle: %zu strip nodes%s\n", n_shingled, worse ? " (reverted: a cell failed)" : "");
        }
    }
    if (std::getenv("NPTOP_DEBUG") != nullptr) {
        size_t n6 = 0; double s6min = 1e9, s6max = -1e9;
        for (const Cell &c : G.cells) if (c.why == 6) { ++ n6; s6min = std::min(s6min, double(c.s)); s6max = std::max(s6max, double(c.s)); }
        std::fprintf(stderr, "np max-height cells %zu s %.3f..%.3f; nodes %zu in %d columns\n", n6, s6min, s6max, NN, G.ncols());
        std::fprintf(stderr, "np analysis ms: grid %.0f patches %.0f shingle %.0f\n", std::chrono::duration<double, std::milli>(t_grid - t_begin).count(),
                     std::chrono::duration<double, std::milli>(t_patches - t_grid).count(), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_patches).count());
        std::fprintf(stderr, "np clearance: %zu calls, %zu passes, %.0f ms; grid %d x %d, %zu patches\n", clr_calls, clr_passes, clr_ms, G.nx, G.ny, patches.size());
    }
    for (Cell &c : G.cells) {
        c.kclip = c.kmin;
        if (! c.ok)
            continue;
        const int a = G.ci(c), b = G.cj(c);
        for (int dj = -1; dj <= 1; ++ dj)
            for (int di = -1; di <= 1; ++ di) {
                if (! G.inside(a + di, b + dj))
                    continue;
                for (int v = G.first(a + di, b + dj); v < G.last(a + di, b + dj); ++ v) {
                    const Cell &q = G.cells[v];
                    if (q.ok && q.patch == c.patch)
                        c.kclip = std::min(c.kclip, q.kmin);
                }
            }
    }
    // A patch whose surface has another top over it somewhere (a deck under a roof): its shell lifts to its own surface,
    // not to the topmost one.
    std::vector<char> patch_lower(patches.size(), 0);
    for (const Patch &p : patches)
        for (int u : p.cells)
            if (G.cells[u].level > 0) { patch_lower[&p - patches.data()] = 1; break; }

    // ---- footprints
    std::vector<double> slice_zs;
    for (const Layer *l : layers) slice_zs.push_back(l->slice_z);
    for (Patch &p : patches) {
        const int pid = int(&p - patches.data());
        // The outline under the patch: the lslices of the lowest layer the patch touches.
        const double zbot = p.smin - P.T + 0.5 * P.h;
        int lb = 0;
        while (lb < int(layers.size()) - 1 && layers[lb]->print_z <= zbot) ++ lb;
        // The cell raster is a staircase; its 0.25 mm steps showed as jogs in every wall loop (reported: "the loops
        // step at the seam", where the flattest point of a loop is such a step). Simplified to straight edges,
        // and pinholes left by the raster are closed.
        auto smooth = [&G](const ExPolygons &in) {
            ExPolygons out = expolygons_simplify(in, scale_(0.6 * G.g));
            for (ExPolygon &ex : out)
                ex.holes.erase(std::remove_if(ex.holes.begin(), ex.holes.end(),
                                              [&G](const Polygon &h) { return std::abs(h.area()) < sqr(scale_(2. * G.g)); }),
                               ex.holes.end());
            return out;
        };
        const ExPolygons tiles = smooth(cells_to_expolygons(G, [pid](const Cell &c) { return c.ok && c.patch == pid; }));
        // The real outline of the top (0.1.9, thin tops): a column under a top is solid from its span's bottom up to S,
        // so near its edge the top lies inside the slice of the highest layer under S there. The lowest layer's
        // outline alone let a top that narrows upward (a bow tip) overhang its own wall.
        ExPolygons real;
        // The outline the cell raster grows out to: the slices of those same layers. The lowest layer's (lb) is narrower
        // than the top where the side flares out (a bow): the footprint stopped 0.25 mm short of the rim there, and the
        // planar top under the gap showed 0.2-0.4 mm low.
        ExPolygons outline;
        {
            std::vector<int> lay(G.cells.size(), -1);
            std::vector<int> used;
            for (int u : p.cells) {
                const int L = int(std::upper_bound(slice_zs.begin(), slice_zs.end(), double(G.cells[u].s) - 1e-3) - slice_zs.begin()) - 1;
                lay[u] = std::max(0, L);
                used.push_back(lay[u]);
            }
            sort_remove_duplicates(used);
            for (int L : used) {
                const ExPolygons tl = cells_to_expolygons(G, [&](const Cell &c) { return c.patch == pid && c.ok && lay[&c - G.cells.data()] == L; });
                append(real, intersection_ex(offset_ex(tl, float(scale_(2. * G.g))), layers[L]->lslices));
                append(outline, layers[L]->lslices);
            }
            real    = union_ex(real);
            outline = union_ex(outline);
        }
        p.real      = real;
        p.footprint = smooth(opening_ex(intersection_ex(reach_outline(tiles, outline, 1.5 * G.g), real), float(scale_(0.5 * G.g))));
        // Never over the cells of another top at about the same height (the next height band along a rim): growing to
        // the outline ran 0.4 mm along the rim's edges into the band printed before, 0.22 mm into it (0.1.9).
        {
            const double lo = p.smin - 1., hi = p.smax + 1.;
            const ExPolygons others = cells_to_expolygons(G, [pid, lo, hi](const Cell &c) { return c.ok && c.patch >= 0 && c.patch != pid && c.s >= lo && c.s <= hi; });
            if (! others.empty())
                p.footprint = diff_ex(p.footprint, others);
        }
        p.layer_footprint.assign(P.n, ExPolygons());
        for (int k = 0; k < P.n; ++ k) {
            const ExPolygons tk = smooth(cells_to_expolygons(G, [pid, k](const Cell &c) { return c.ok && c.patch == pid && c.kmin <= k; }));
            p.layer_footprint[k] = k == P.n - 1 ? p.footprint :
                smooth(opening_ex(intersection_ex(reach_outline(tk, outline, 1.5 * G.g), p.footprint), float(scale_(0.5 * G.g))));
        }
        if (const char *probe = std::getenv("NPTOP_PROBE")) {
            double px = 0, py = 0;
            if (std::sscanf(probe, "%lf,%lf", &px, &py) == 2) {
                const Point pp = Point::new_scale(px, py);
                auto in = [&pp](const ExPolygons &e) { for (const ExPolygon &x : e) if (x.contains(pp)) return 1; return 0; };
                auto dist = [&pp](const ExPolygons &e) {
                    double d = 1e9;
                    for (const ExPolygon &x : e) for (const Line &l : x.lines()) d = std::min(d, l.distance_to(pp));
                    return unscale<double>(d);
                };
                if (dist(tiles) < 2.)
                    std::fprintf(stderr, "np probe fp patch %d: tiles %d (%.3f) real %d (%.3f) lslices[lb %d] %d (%.3f) footprint %d (%.3f)\n", pid,
                                 in(tiles), dist(tiles), in(real), dist(real), lb, in(layers[lb]->lslices), dist(layers[lb]->lslices), in(p.footprint), dist(p.footprint));
            }
        }
        p.fp0       = p.footprint;
        p.outer     = p.footprint;
        p.smin_lift = p.smin;
        p.handoff.assign(P.n, ExPolygons());
    }
    // Shingled seams: the strip's layers from kmax up move from the earlier top's footprints to the later one's (the
    // "others" subtraction does not apply to them; they stay inside the earlier top's real outline). The clip keeps the
    // footprints from before: the band removed under the strip is the same [S - T, S] whichever top prints over it.
    if (n_shingled > 0) {
        const float fuse = float(scale_(0.02));
        for (Patch &pb : patches) {
            const int pid = int(&pb - patches.data());
            ExPolygons from; // the earlier tops' outlines
            std::vector<char> src(patches.size(), 0);
            for (size_t u = 0; u < NN; ++ u)
                if (cov_of[u] == pid && ! src[G.cells[u].patch]) {
                    src[G.cells[u].patch] = 1;
                    append(from, patches[G.cells[u].patch].fp0);
                    pb.smin_lift = std::min(pb.smin_lift, patches[G.cells[u].patch].smin);
                }
            if (from.empty())
                continue;
            from = offset_ex(union_ex(from), fuse);
            auto strip = [&](int k) {
                return intersection_ex(offset_ex(cells_to_expolygons(G, [&](const Cell &c) {
                    const size_t u = size_t(&c - G.cells.data());
                    return cov_of[u] == pid && c.kmax <= k; }), fuse), from);
            };
            const ExPolygons all = strip(P.n - 1);
            for (int k = 0; k < P.n; ++ k) {
                const ExPolygons sk = k == P.n - 1 ? all : strip(k);
                if (! sk.empty())
                    pb.layer_footprint[k] = union_ex(pb.layer_footprint[k], sk);
                pb.handoff[k] = diff_ex(all, sk); // the earlier top prints layer k there
            }
            pb.footprint = pb.layer_footprint[P.n - 1];
            pb.outer     = union_ex(pb.outer, all);
        }
        for (Patch &pa : patches) {
            const int pid = int(&pa - patches.data());
            for (int k = 0; k < P.n; ++ k) {
                const ExPolygons sk = offset_ex(cells_to_expolygons(G, [&](const Cell &c) {
                    const size_t u = size_t(&c - G.cells.data());
                    return c.ok && c.patch == pid && cov_of[u] >= 0 && c.kmax <= k; }), fuse);
                if (sk.empty())
                    continue;
                pa.layer_footprint[k] = opening_ex(diff_ex(pa.layer_footprint[k], sk), float(scale_(0.5 * G.g)));
                pa.handoff[k]         = intersection_ex(sk, pa.fp0); // the later top prints layer k there
            }
            pa.footprint = pa.layer_footprint[P.n - 1];
        }
    }

    if (const char *map_path = std::getenv("NPTOP_DEBUG_MAP")) {
        // Binary PPM, one pixel per column (its topmost node), +Y up: reason colour; curved cells shaded by kmin.
        static const unsigned char col[9][3] = { {40, 200, 60}, {0, 0, 0}, {120, 120, 255}, {230, 60, 60},
                                                 {240, 200, 40}, {150, 150, 150}, {200, 80, 220}, {255, 140, 0}, {0, 200, 200} };
        if (FILE *f = std::fopen(map_path, "wb")) {
            std::fprintf(f, "P6 %d %d 255 ", G.nx, G.ny);
            for (int b = G.ny - 1; b >= 0; -- b)
                for (int a = 0; a < G.nx; ++ a) {
                    unsigned char px[3] = { col[1][0], col[1][1], col[1][2] };
                    if (G.first(a, b) < G.last(a, b)) {
                        const Cell &c = G.cells[G.first(a, b)];
                        px[0] = col[c.why][0]; px[1] = col[c.why][1]; px[2] = col[c.why][2];
                        if (c.why == 0 && c.ok) px[1] = (unsigned char)std::max(60, 200 - 40 * c.kmin);
                    }
                    std::fwrite(px, 1, 3, f);
                }
            std::fclose(f);
        }
    }

    if (std::getenv("NPTOP_DEBUG") != nullptr)
        for (const Patch &p : patches) {
            const int pid = int(&p - patches.data());
            std::vector<size_t> kh(P.n + 1, 0);
            size_t ncells = 0, nlow = 0;
            for (const Cell &c : G.cells)
                if (c.ok && c.patch == pid) { ++ ncells; ++ kh[std::min(c.kmin, P.n)]; nlow += c.level > 0; }
            double a = 0;
            for (const ExPolygon &ex : p.footprint) a += ex.area();
            std::fprintf(stderr, "np patch %d: cells %zu (under another top %zu) s %.3f..%.3f host %d ref %.3f area %.1f grid %dx%d g %.3f kmin:",
                         pid, ncells, nlow, p.smin, p.smax, p.host, p.host_ref, a * SCALING_FACTOR * SCALING_FACTOR, G.nx, G.ny, G.g);
            for (size_t v : kh) std::fprintf(stderr, " %zu", v);
            std::fprintf(stderr, "\n");
        }

    const auto t_shell = std::chrono::steady_clock::now();

    // ---- curved shells. Built BEFORE the clipping: planar material is removed only where the top
    // curved layer really covers it (its beads, not just the footprint), so no clipped area is ever
    // left open even if a fill fails or a footprint sliver is too thin to fill.
    // The patch's own surface at p: the topmost hit if it lies within [lo, hi]; for a top under another one, the highest
    // up-facing hit within [lo, hi]. NaN if none.
    auto surface_in = [&surf, &mesh](const Vec2d &p, double lo, double hi, bool lower) -> double {
        const Surface3::Hit h = surf.down(p.x(), p.y());
        if (h.hit) {
            const double s = surf.to_print(h.z);
            if (s >= lo && s <= hi) return s;
        }
        if (lower)
            for (const auto &hh : mesh.query_ray_hits(Vec3d(p.x() + 1.3e-4, p.y() + 0.7e-4, surf.top), -Vec3d::UnitZ())) {
                if (! hh.is_hit() || hh.normal().z() <= 1e-6 * hh.normal().norm()) continue;
                const double s = surf.to_print(surf.top - hh.distance());
                if (s >= lo && s <= hi) return s;
            }
        return std::numeric_limits<double>::quiet_NaN();
    };

    // mm3 / width / height > 0 override the flow (ironing).
    struct Job { Points pts; int k; ExtrusionRole role; const Flow *flow; double flow_scale; bool closed; double mm3 = 0.; float width = 0.f; float height = 0.f; bool gap = false; };
    std::vector<std::pair<LayerRegion *, std::unique_ptr<ExtrusionEntityCollection>>> shells;
    std::vector<int> shell_pid; // the top of each entry in shells
    std::vector<ExPolygons> patch_cover(patches.size()); // per top: what its top curved layer covers

    for (const Patch &p : patches) {
        if (p.outer.empty())
            continue;
        const int    pid    = int(&p - patches.data());
        Layer       *host   = layers[p.host];
        LayerRegion *layerm = nullptr;
        for (LayerRegion *lr : host->regions())
            if (lr->region().config().nonplanar_layers.value) { layerm = lr; break; }
        if (layerm == nullptr && ! host->regions().empty())
            layerm = host->regions().front();
        if (layerm == nullptr)
            continue;
        const PrintRegionConfig &cfg = layerm->region().config();
        const double hz = host->print_z;

        const Flow ext_flow   = layerm->flow(frExternalPerimeter, P.h);
        const Flow perim_flow = layerm->flow(frPerimeter, P.h);
        const Flow solid_flow = layerm->flow(frSolidInfill, P.h);
        const Flow top_flow   = layerm->flow(frTopSolidInfill, P.h);

        // Every curved layer is built like a planar top-shell layer: wall_loops loops along its footprint
        // (outer wall + inner walls, so a patch that reaches the object's side carries the same walls as
        // the planar layers under it, lifted onto the surface) and solid lines inside them.
        const int n_loops = std::max(1, cfg.wall_loops.value);
        std::vector<double> loop_offset; // centreline inset from the footprint edge, scaled
        {
            double d = 0.5 * ext_flow.scaled_width();
            loop_offset.push_back(d);
            for (int i = 1; i < n_loops; ++ i) {
                d += i == 1 ? 0.5 * (ext_flow.scaled_spacing() + perim_flow.scaled_spacing()) : perim_flow.scaled_spacing();
                loop_offset.push_back(d);
            }
        }
        const Flow &last_loop_flow = n_loops == 1 ? ext_flow : perim_flow;
        // Fill bead edge overlaps the last loop's bead edge by 15 % of a solid line.
        auto fill_inset_for = [&](const Flow &fill) {
            return float(loop_offset.back() + 0.5 * last_loop_flow.scaled_width() + 0.5 * fill.scaled_width() - 0.15 * fill.scaled_width());
        };

        double top_angle = cfg.top_layer_direction.value >= 0. ? cfg.top_layer_direction.value : cfg.solid_infill_direction.value;
        if (cfg.nonplanar_top_surface_direction_enable.value)
            top_angle = cfg.nonplanar_top_surface_direction.value;
        auto line_pattern = [](InfillPattern p) {
            return (p == ipMonotonic || p == ipMonotonicLine || p == ipRectilinear || p == ipAlignedRectilinear || p == ipConcentric) ? p : ipMonotonic;
        };
        const InfillPattern top_pattern   = line_pattern(cfg.top_surface_pattern.value);
        const InfillPattern solid_pattern = line_pattern(cfg.internal_solid_infill_pattern.value);
        double top_flow_scale = 1.;
        if (cfg.nonplanar_top_surface_flow_ratio_enable.value && cfg.top_solid_infill_flow_ratio.get_at(0) > EPSILON)
            top_flow_scale = cfg.nonplanar_top_surface_flow_ratio.value / cfg.top_solid_infill_flow_ratio.get_at(0);

        // 2D toolpaths of every curved layer, in print order. Only the outermost curved layer is the top
        // surface (top pattern, speed and flow); the ones under it are internal solid infill, alternating
        // direction, exactly like the planar top shell they replace.
        std::vector<Job> jobs;
        Polylines        top_fill_lines, top_loop_lines;
        // Walls only along the patch's outer edge (the object's side or the seam against the planar stairs).
        // A deeper curved layer ends early where the kmin ramp starts; that edge runs across the inside of the
        // surface, and loops along it showed through the top layer as a diagonal band of inner wall.
        const ExPolygons wall_band = diff_ex(offset_ex(p.outer, float(scale_(0.05))),
                                             offset_ex(p.outer, -float(loop_offset.back() + 0.75 * perim_flow.scaled_spacing())));
        // The fills of a band can throw instead of coming back empty: a sliver of a curved layer's footprint reaches
        // the Voronoi code's resolution limit and the gap fill's medial axis gives up (Slic3r::OutOfRange out of
        // VoronoiUtils). That must cost this top its curved band, never the slice: the band is dropped here and the
        // planar top surface it would have replaced stays, exactly as for a top that was never eligible.
        // CriticalException is the common base of InfillFailedException (a RuntimeError) and of the Voronoi
        // OutOfRange / InvalidArgument (LogicErrors), so one catch covers every failure these fills can raise. Do NOT
        // widen it to std::exception: CanceledException (PrintBase.hpp) and SlicingError are not CriticalException,
        // and both must keep propagating -- a cancel and a deliberate slicing error are not ours to swallow.
        // The whole band goes, not just the layer that failed: the planar part is clipped by what the TOP curved layer
        // covers (patch_cover below), so a band missing a layer would either print its deeper curved layers into uncut
        // planar material or leave a void under its top one.
        bool band_failed = false;
        try {
            for (int k = 0; k < P.n; ++ k) {
                const bool top = k == P.n - 1;
                for (int li = 0; li < n_loops; ++ li) {
                    const ExPolygons    loops = offset_ex(p.layer_footprint[k], -float(loop_offset[li]));
                    const ExtrusionRole role  = li == 0 ? erExternalPerimeter : erPerimeter;
                    const Flow         *lf    = li == 0 ? &ext_flow : &perim_flow;
                    auto add_loop = [&](const Polygon &poly) {
                        if (top) { jobs.push_back({ poly.points, k, role, lf, 1., true }); return; }
                        Polyline closed(poly.points);
                        closed.points.push_back(poly.points.front());
                        Polylines kept = intersection_pl(Polylines{ closed }, wall_band);
                        if (kept.size() == 1 && std::abs(kept.front().length() - closed.length()) < scale_(0.01)) {
                            jobs.push_back({ poly.points, k, role, lf, 1., true });
                            return;
                        }
                        for (Polyline &pl : kept)
                            if (pl.length() > scale_(0.8))
                                jobs.push_back({ pl.points, k, role, lf, 1., false });
                    };
                    for (const ExPolygon &ex : loops) {
                        add_loop(ex.contour);
                        for (const Polygon &hole : ex.holes)
                            add_loop(hole);
                        if (top) {
                            Polyline c(ex.contour.points); c.points.push_back(ex.contour.points.front()); top_loop_lines.push_back(c);
                            for (const Polygon &hole : ex.holes) { Polyline hl(hole.points); hl.points.push_back(hole.points.front()); top_loop_lines.push_back(hl); }
                        }
                    }
                }
                const Flow      &flow      = top ? top_flow : solid_flow;
                // Inside the wall band the fill stops at the last loop; at an inner (kmin ramp) edge it runs to the edge.
                const ExPolygons fill_area = top ? offset_ex(p.layer_footprint[k], -fill_inset_for(flow)) :
                    intersection_ex(offset_ex(p.layer_footprint[k], -float(0.5 * flow.scaled_spacing())), offset_ex(p.outer, -fill_inset_for(flow)));
                const double     angle     = top_angle + (((P.n - 1 - k) % 2) ? 90. : 0.);
                for (const ExPolygon &ex : fill_area) {
                    Polylines lines;
                    // The configured pattern first; a fill that comes back empty or throws falls back to plain monotonic lines.
                    for (InfillPattern pat : { top ? top_pattern : solid_pattern, ipMonotonic, ipRectilinear }) {
                        std::unique_ptr<Fill> f(Fill::new_from_type(pat));
                        f->set_bounding_box(object.bounding_box());
                        f->layer_id        = host->id();
                        f->z               = hz;
                        f->spacing         = flow.spacing();
                        f->angle           = float(Geometry::deg2rad(angle));
                        f->fixed_angle     = true;
                        f->link_max_length = coord_t(scale_(3. * f->spacing));
                        f->overlap         = 0;
                        FillParams fp;
                        fp.density   = 1.f;
                        fp.monotonic = true;
                        Surface s(top ? stTop : stInternalSolid, ex);
                        try { lines = f->fill_surface(&s, fp); } catch (const Slic3r::CriticalException &) { lines.clear(); }
                        if (! lines.empty())
                            break;
                    }
                    for (const Polyline &pl : lines) {
                        jobs.push_back({ pl.points, k, top ? erTopSolidInfill : erSolidInfill, &flow, top ? top_flow_scale : 1., false });
                        if (top) top_fill_lines.push_back(pl);
                    }
                }
            }

            // Gap fill: a neck or a small hole of the footprint leaves strips between the loops and the fill lines too
            // narrow for a line, and the planar top under them is cut (tt_5: 20 mm2 of internal solid showing). Medial-axis
            // lines, metered to the strip width, like a planar gap fill. On every curved layer (0.1.9, thin tops: a rim
            // narrower than two walls gets the loops that fit and a centre line where none does).
            for (int k = P.n - 1; k >= 0; -- k) {
                if (nonplanar_top_band_test_hook != nullptr)
                    nonplanar_top_band_test_hook();
                const bool top = k == P.n - 1;
                const Flow &gflow = top ? top_flow : solid_flow;
                Polygons beads;
                if (top) {
                    beads = offset(top_fill_lines, float(0.5 * top_flow.scaled_width()));
                    append(beads, offset(top_loop_lines, float(0.5 * perim_flow.scaled_width())));
                } else
                    for (const Job &job : jobs)
                        if (job.k == k && job.pts.size() >= 2) {
                            Polyline pl(job.pts);
                            if (job.closed) pl.points.push_back(job.pts.front());
                            append(beads, offset(Polylines{ pl }, float(0.5 * job.flow->scaled_width())));
                        }
                ExPolygons gap_area = diff_ex(top ? p.footprint : p.layer_footprint[k], union_(beads));
                // A deeper layer ends early on the clearance ramp, where the planar part stands higher: its gap fill keeps a
                // bead away from that edge (it ran 0.12 mm under Benchy's planar wall there).
                if (! top)
                    gap_area = diff_ex(gap_area, offset_ex(diff_ex(p.outer, p.handoff[k].empty() ? p.layer_footprint[k] :
                                                                   union_ex(p.layer_footprint[k], p.handoff[k])), float(gflow.scaled_width())));
                const ExPolygons gaps = opening_ex(gap_area, float(scale_(0.05)));
                for (const ExPolygon &gap : gaps) {
                    ThickPolylines tps;
                    gap.medial_axis(scale_(0.1), scale_(1.2), &tps);
                    for (const ThickPolyline &tp : tps) {
                        if (tp.points.size() < 2 || tp.length() < scale_(0.3))
                            continue;
                        double w = 0.;
                        for (coordf_t v : tp.width) w += v;
                        w = tp.width.empty() ? gflow.scaled_width() : w / tp.width.size();
                        const double scale_w = std::clamp(w / gflow.scaled_width(), 0.25, 1.5);
                        jobs.push_back({ tp.points, k, top ? erTopSolidInfill : erSolidInfill, &gflow, (top ? top_flow_scale : 1.) * scale_w, false });
                        jobs.back().gap = true;
                        if (top) top_fill_lines.push_back(Polyline(tp.points));
                    }
                }
            }
        } catch (const Slic3r::CriticalException &ex) {
            BOOST_LOG_TRIVIAL(error) << "Nonplanar layers: the band of the curved top printed at z " << hz << " failed to fill ("
                                     << ex.what() << "); it is dropped and the planar top surface is kept there.";
            band_failed = true;
        }
        if (band_failed) {
            // Nothing of this top has been committed yet: its shells, its cover and its clip all come below.
            ++ stats.skipped_tops;
            continue;
        }

        // Ironing over the top curved layer, on the surface itself, after it (in its layer). Same parameters as
        // planar ironing (Layer::make_ironing): spacing, inset, flow as a fraction of the layer height, angle.
        if (cfg.ironing_type.value != IroningType::NoIroning) {
            const double nozzle  = pc.nozzle_diameter.values.empty() ? 0.4 : pc.nozzle_diameter.values.front();
            const double spacing = std::max(0.02, cfg.ironing_spacing.value);
            const double ih      = P.h * 0.01 * cfg.ironing_flow.value * spacing / nozzle;
            if (ih > EPSILON) {
                const double inset = cfg.ironing_inset.value == 0. ? 0.5 * nozzle : cfg.ironing_inset.value;
                const double angle = (cfg.ironing_angle_fixed.value ? 0. : top_angle) + cfg.ironing_angle.value;
                for (const ExPolygon &ex : offset_ex(p.footprint, -float(scale_(inset)))) {
                    std::unique_ptr<Fill> f(Fill::new_from_type(line_pattern(cfg.ironing_pattern.value)));
                    f->set_bounding_box(object.bounding_box());
                    f->layer_id    = host->id();
                    f->z           = hz;
                    f->spacing     = spacing;
                    f->angle       = float(Geometry::deg2rad(angle));
                    f->fixed_angle = true;
                    f->link_max_length = coord_t(scale_(3. * spacing));
                    FillParams fp;
                    fp.density   = 1.f;
                    fp.monotonic = true;
                    Surface sf(stTop, ex);
                    Polylines lines;
                    try { lines = f->fill_surface(&sf, fp); } catch (const Slic3r::CriticalException &) { lines.clear(); }
                    for (const Polyline &pl : lines) {
                        Job j{ pl.points, P.n - 1, erIroning, &top_flow, 1., false };
                        j.mm3    = nozzle * ih;
                        j.width  = Flow::rounded_rectangle_extrusion_width_from_spacing(float(nozzle), float(ih));
                        j.height = float(ih);
                        jobs.push_back(std::move(j));
                    }
                }
            }
        }

        // Seams (audit P2): a closed loop starts near the seam of the loop printed before it (aligned / nearest / back), or
        // at its rearmost point for the first one and for "rear"; it ends a seam gap short of its start, like a planar loop.
        {
            const SeamPosition sp     = object.config().seam_position.value;
            const double       nozzle = pc.nozzle_diameter.values.empty() ? 0.4 : pc.nozzle_diameter.values.front();
            const double       gap    = std::max(0., cfg.seam_gap.get_abs_value(nozzle));
            bool  have_prev = false;
            Point prev;
            for (Job &job : jobs) {
                if (! job.closed || job.pts.size() < 3)
                    continue;
                size_t best = 0;
                if (sp == spRear || ! have_prev) {
                    for (size_t i = 1; i < job.pts.size(); ++ i)
                        if (job.pts[i].y() > job.pts[best].y() || (job.pts[i].y() == job.pts[best].y() && job.pts[i].x() < job.pts[best].x())) best = i;
                } else if (sp != spRandom) {
                    double bd = std::numeric_limits<double>::max();
                    for (size_t i = 0; i < job.pts.size(); ++ i) {
                        const double d = (job.pts[i] - prev).cast<double>().squaredNorm();
                        if (d < bd) { bd = d; best = i; }
                    }
                }
                std::rotate(job.pts.begin(), job.pts.begin() + best, job.pts.end());
                prev = job.pts.front(); have_prev = true;
                // Open it: the closing point, minus the seam gap.
                Polyline pl(job.pts);
                pl.points.push_back(job.pts.front());
                if (gap > EPSILON && pl.length() > scale_(4. * gap))
                    pl.clip_end(scale_(gap));
                job.pts    = pl.points;
                job.closed = false;
            }
        }

        // Lift every toolpath (in parallel): samples and heights first, then one path per toolpath, metered per segment.
        std::vector<std::vector<std::unique_ptr<ExtrusionPath>>> lifted(jobs.size());
        std::vector<std::vector<char>>              job_keep(jobs.size()); // empty: every sample is printed
        std::vector<double>                         job_top(jobs.size(), -1e30);
        std::vector<std::vector<Vec2d>>             job_xy(jobs.size());
        std::vector<std::vector<double>>            job_z(jobs.size());
        const double tlim_env = std::tan(std::min(89., P.max_angle + 3.) * M_PI / 180.);
        // (after a raise the envelope is a little flatter: a raised run lies on the envelope slope over its whole
        // length, and the G-code rounding of a long chord must not tip it over the limit)
        const double tlim_raise = std::tan(std::min(89., P.max_angle + 2.5) * M_PI / 180.);
        auto upper_envelope = [](const std::vector<Vec2d> &xy, std::vector<double> &z, double tlim) {
            for (size_t i = 1; i < xy.size(); ++ i)
                z[i] = std::max(z[i], z[i - 1] - tlim * (xy[i] - xy[i - 1]).norm());
            for (size_t i = xy.size() - 1; i -- > 0;)
                z[i] = std::max(z[i], z[i + 1] - tlim * (xy[i + 1] - xy[i]).norm());
        };
        tbb::parallel_for(tbb::blocked_range<size_t>(0, jobs.size()), [&](const tbb::blocked_range<size_t> &range) {
            for (size_t j = range.begin(); j < range.end(); ++ j) {
                const Job   &job   = jobs[j];
                const int    k     = job.k;
                const double depth = (P.n - 1 - k) * P.h;
                Points pts = job.pts;
                if (job.closed && ! pts.empty()) pts.push_back(pts.front());
                if (pts.size() < 2)
                    continue;
                std::vector<Vec2d> &xy = job_xy[j];
                for (size_t i = 0; i + 1 < pts.size(); ++ i) {
                    const Vec2d a = unscale(pts[i]), b = unscale(pts[i + 1]);
                    const int   ns = std::max(1, int(std::ceil((b - a).norm() / kResample)));
                    for (int s = 0; s < ns; ++ s) xy.push_back(a + (b - a) * (double(s) / ns));
                }
                xy.push_back(unscale(pts.back()));
                // A point on the very outline can graze the side wall and hit the BOTTOM face (seen on a test part:
                // a loop point at the object's edge read S = 0 and was lifted to z = -0.6). Only a hit within
                // this patch's own surface range counts; a miss or a stray hit takes the nearest good one.
                std::vector<double> &z = job_z[j];
                z.assign(xy.size(), 0.);
                std::vector<char>   good(xy.size(), 0);
                const double        s_lo = p.smin_lift - P.h, s_hi = p.smax + P.h;
                for (size_t i = 0; i < xy.size(); ++ i) {
                    const double s = surface_in(xy[i], s_lo, s_hi, patch_lower[pid] != 0);
                    if (! std::isnan(s) && s >= s_lo && s <= s_hi) { z[i] = s; good[i] = 1; }
                }
                {
                    double last = std::numeric_limits<double>::quiet_NaN();
                    for (size_t i = 0; i < xy.size(); ++ i) {          // forward: carry the last good height
                        if (good[i]) last = z[i]; else z[i] = last;
                    }
                    last = std::numeric_limits<double>::quiet_NaN();
                    for (size_t i = xy.size(); i -- > 0;) {           // backward: fill a bad start
                        if (good[i]) last = z[i]; else if (std::isnan(z[i])) z[i] = last;
                    }
                }
                for (double &v : z)
                    if (std::isnan(v)) v = p.smax;
                // Spikes (audit P0: an outline sample hit a higher part of the same top, 0.33-0.40 mm over 0.1 mm): a sample
                // is checked against the top's own cells around it and against its neighbours on the path, and an outlier
                // is re-interpolated from the good samples either side.
                {
                    const double tmax = std::tan(P.max_angle * M_PI / 180.);
                    std::vector<char> bad(xy.size(), 0);
                    for (size_t i = 0; i < xy.size(); ++ i) {
                        const int ci = int(std::lround((xy[i].x() - G.x0) / G.g)), cj = int(std::lround((xy[i].y() - G.y0) / G.g));
                        double best_d = 1e30, ref = 0.;
                        for (int dj = -2; dj <= 2; ++ dj)
                            for (int di = -2; di <= 2; ++ di) {
                                const int qa = ci + di, qb = cj + dj;
                                if (qa < 0 || qa >= G.nx || qb < 0 || qb >= G.ny) continue;
                                for (int v = G.first(qa, qb); v < G.last(qa, qb); ++ v) {
                                    const Cell &q = G.cells[v];
                                    if (! q.ok || (q.patch != pid && cov_of[size_t(v)] != pid)) continue;
                                    const double d = std::hypot(G.cx(qa) - xy[i].x(), G.cy(qb) - xy[i].y());
                                    if (d < best_d) { best_d = d; ref = q.s; }
                                }
                            }
                        if (best_d < 1e29 && std::abs(z[i] - ref) > 0.5 * P.h + tmax * best_d)
                            bad[i] = 1;
                        // Past the top's edge the surface falls away more steeply than the top; a sample there stays
                        // within the top's slope of its nearest cell (dome: a gap-fill end dipped 0.05 mm under the
                        // neighbouring line).
                        else if (best_d < 1e29 && best_d > 0.5 * G.g)
                            z[i] = std::max(z[i], ref - tmax * best_d);
                    }
                    for (size_t i = 1; i + 1 < xy.size(); ++ i) {
                        if (bad[i]) continue;
                        const double d0 = (xy[i] - xy[i - 1]).norm(), d1 = (xy[i + 1] - xy[i]).norm();
                        const double e0 = z[i] - z[i - 1], e1 = z[i] - z[i + 1];
                        // above (or below) both neighbours by more than the slope allows: a spike
                        if ((e0 > tmax * d0 + 0.1 * P.h && e1 > tmax * d1 + 0.1 * P.h) || (e0 < -(tmax * d0 + 0.1 * P.h) && e1 < -(tmax * d1 + 0.1 * P.h)))
                            bad[i] = 1;
                    }
                    if (std::count(bad.begin(), bad.end(), char(1)) < std::ptrdiff_t(xy.size())) {
                        std::vector<double> s(xy.size(), 0.);
                        for (size_t i = 1; i < xy.size(); ++ i) s[i] = s[i - 1] + (xy[i] - xy[i - 1]).norm();
                        for (size_t i = 0; i < xy.size(); ++ i) {
                            if (! bad[i]) continue;
                            size_t a = i, b = i;
                            while (a > 0 && bad[a]) -- a;
                            while (b + 1 < xy.size() && bad[b]) ++ b;
                            if (bad[a]) z[i] = z[b];
                            else if (bad[b]) z[i] = z[a];
                            else z[i] = z[a] + (z[b] - z[a]) * (s[i] - s[a]) / std::max(1e-9, s[b] - s[a]);
                        }
                    }
                }
                // Steps (audit: Benchy 98 segments at 50-60 deg, +-0.11-0.15 mm over 0.09 mm, where an outline sample reads
                // another part of the surface): no segment may be steeper than the max angle + 3 deg. Only ever raised (the
                // upper envelope), so a step is bridged a little above the surface instead of dipping into material.
                upper_envelope(xy, z, tlim_env);
                for (double &v : z)
                    v -= depth;
                // A closed loop starts (and ends) at its flattest point: a seam on a slope shows as a step
                // where the start and the end of the bead meet at different heights in the preview.
                if (job.closed && xy.size() > 3) {
                    const size_t m = xy.size() - 1; // xy.back() == xy.front()
                    size_t best = 0;
                    double best_s = std::numeric_limits<double>::max();
                    for (size_t i = 0; i < m; ++ i) {
                        const double s = std::abs(z[(i + 1) % m] - z[(i + m - 1) % m]);
                        if (s < best_s - 1e-9) { best_s = s; best = i; }
                    }
                    if (best != 0) {
                        std::vector<Vec2d>  xy2;
                        std::vector<double> z2;
                        for (size_t i = 0; i <= m; ++ i) { xy2.push_back(xy[(best + i) % m]); z2.push_back(z[(best + i) % m]); }
                        xy.swap(xy2);
                        z.swap(z2);
                    }
                }
            }
        });

        // Within one curved layer, on a slope, a line closer to an earlier bead of that layer than the line spacing lies
        // lower than that bead, and the nozzle face presses into it (Benchy roof and bow, 0.05-0.07 mm). A gap-fill line
        // within 0.75 of its width of an earlier bead (lower by more than a small tolerance) is trimmed there in the top
        // curved layer (the gap beside the bead is narrower than a line) and raised to it in the layers under the top. Any
        // other path whose centre runs over an earlier bead (within half its width) is raised to it (never lowered, by at
        // most half a layer), then the upper slope envelope again.
        tbb::parallel_for(tbb::blocked_range<int>(0, P.n), [&](const tbb::blocked_range<int> &range) {
            constexpr double cg = 0.1; // mm, height map cell
            const double     tol = 0.03, max_raise = 0.5 * P.h;
            for (int k = range.begin(); k < range.end(); ++ k) {
                BoundingBoxf bb;
                for (size_t j = 0; j < jobs.size(); ++ j)
                    if (jobs[j].k == k)
                        for (const Vec2d &q : job_xy[j]) bb.merge(q);
                if (! bb.defined)
                    continue;
                const double ox = bb.min.x() - 1., oy = bb.min.y() - 1.;
                const int    nx = int(std::ceil((bb.max.x() - ox + 1.) / cg)) + 1, ny = int(std::ceil((bb.max.y() - oy + 1.) / cg)) + 1;
                std::vector<float> hm(size_t(nx) * size_t(ny), -std::numeric_limits<float>::max());
                for (size_t j = 0; j < jobs.size(); ++ j) {
                    const Job &job = jobs[j];
                    if (job.k != k || job.role == erIroning || job_xy[j].empty())
                        continue;
                    const std::vector<Vec2d> &xy = job_xy[j];
                    std::vector<double>      &z  = job_z[j];
                    // A short piece (a wall fragment of a speck of footprint, under 2 mm) is trimmed like a gap fill.
                    double len = 0.;
                    for (size_t i = 0; i + 1 < xy.size() && len < 2.; ++ i) len += (xy[i + 1] - xy[i]).norm();
                    // Only the top curved layer is trimmed: under it a trimmed gap fill leaves a void that nothing fills
                    // (a whole layer missing along a shingled band seam), while a raised one is just covered by the
                    // next curved layer.
                    const bool   thin = job.gap || len < 2.;
                    const bool   trim = thin && k == P.n - 1;
                    const double r  = (thin ? 0.75 : 0.5) * (job.width > 0.f ? double(job.width) : job.flow->width());
                    const int    rc = int(std::ceil(r / cg));
                    bool raised = false;
                    std::vector<char> keep;
                    for (size_t i = 0; i < xy.size(); ++ i) {
                        const int ci = int((xy[i].x() - ox) / cg), cj = int((xy[i].y() - oy) / cg);
                        float m = -std::numeric_limits<float>::max();
                        for (int dj = -rc; dj <= rc; ++ dj)
                            for (int di = -rc; di <= rc; ++ di) {
                                const int a = ci + di, b = cj + dj;
                                if (a < 0 || b < 0 || a >= nx || b >= ny) continue;
                                if (std::hypot(ox + (a + 0.5) * cg - xy[i].x(), oy + (b + 0.5) * cg - xy[i].y()) > r) continue;
                                m = std::max(m, hm[size_t(a) * ny + b]);
                            }
                        if (m > -1e30f && z[i] < double(m) - tol) {
                            if (trim) {
                                if (keep.empty()) keep.assign(xy.size(), 1);
                                keep[i] = 0;
                            } else {
                                z[i] = std::max(z[i], std::min(double(m) - tol, z[i] + max_raise));
                                raised = true;
                            }
                        }
                    }
                    if (raised)
                        upper_envelope(xy, z, tlim_raise);
                    job_keep[j] = keep;
                    for (size_t i = 0; i < xy.size(); ++ i) {
                        if (! keep.empty() && ! keep[i])
                            continue;
                        const int ci = std::clamp(int((xy[i].x() - ox) / cg), 0, nx - 1), cj = std::clamp(int((xy[i].y() - oy) / cg), 0, ny - 1);
                        float &h = hm[size_t(ci) * ny + cj];
                        h = std::max(h, float(z[i]));
                    }
                }
            }
        });

        tbb::parallel_for(tbb::blocked_range<size_t>(0, jobs.size()), [&](const tbb::blocked_range<size_t> &range) {
            for (size_t j = range.begin(); j < range.end(); ++ j) {
                const Job                 &job    = jobs[j];
                const std::vector<Vec2d>  &xy_all = job_xy[j];
                const std::vector<double> &z_all  = job_z[j];
                const std::vector<char>   &keep   = job_keep[j];
                // The kept runs of samples (a trimmed gap fill can fall apart; pieces under 0.3 mm are dropped).
                for (size_t i0 = 0; i0 < xy_all.size();) {
                    if (! keep.empty() && ! keep[i0]) { ++ i0; continue; }
                    size_t i1 = i0;
                    while (i1 + 1 < xy_all.size() && (keep.empty() || keep[i1 + 1])) ++ i1;
                    const size_t ra = i0, rb = i1;
                    i0 = i1 + 1;
                    if (rb == ra)
                        continue;
                    if (! keep.empty()) {
                        double len = 0.;
                        for (size_t i = ra; i < rb; ++ i) len += (xy_all[i + 1] - xy_all[i]).norm();
                        if (len < 0.3)
                            continue;
                    }
                    const std::vector<Vec2d>  xy(xy_all.begin() + ra, xy_all.begin() + rb + 1);
                    const std::vector<double> z(z_all.begin() + ra, z_all.begin() + rb + 1);
                    // Constant flow (metering the first layer is complicated for very little gain). The planar
                    // part is the model sliced as if its top were T lower, so the gap under the first curved layer is one
                    // layer on average (h/2 .. 3h/2).
                    auto seg_ratio = [](size_t) { return 1.; };
                    // Vertices + per-segment flow; a vertex is dropped only if every sample its chord replaces
                    // stays within kCollinearTol and the flow on both sides agrees within kFlowStep.
                    std::vector<Vec3d> run{ Vec3d(xy[0].x(), xy[0].y(), z[0]) };
                    std::vector<float> flows;
                    std::vector<Vec3d> skipped;
                    for (size_t i = 0; i + 1 < xy.size(); ++ i) {
                        const double r = seg_ratio(i);
                        const Vec3d  q(xy[i + 1].x(), xy[i + 1].y(), z[i + 1]);
                        bool merge = false;
                        if (run.size() >= 2 && skipped.size() < 400 && std::abs(flows.back() - r) <= kFlowStep) {
                            const Vec3d &anchor = run[run.size() - 2];
                            if ((q - anchor).dot(run.back() - anchor) > 0 && (q - anchor).squaredNorm() > 0.) {
                                merge = Linef3::distance_to_infinite_squared(run.back(), anchor, q) < kCollinearTol * kCollinearTol;
                                for (size_t s = 0; merge && s < skipped.size(); ++ s)
                                    merge = Linef3::distance_to_infinite_squared(skipped[s], anchor, q) < kCollinearTol * kCollinearTol;
                            }
                        }
                        if (merge) {
                            skipped.push_back(run.back());
                            run.back() = q;
                        } else {
                            run.push_back(q);
                            flows.push_back(float(r));
                            skipped.clear();
                        }
                    }
                    if (run.size() < 2)
                        continue;
                    const double base_mm3 = job.mm3 > 0. ? job.mm3 : job.flow->mm3_per_mm() * job.flow_scale;
                    auto path = std::make_unique<ExtrusionPath>(job.role, base_mm3, job.width > 0.f ? job.width : float(job.flow->width()),
                                                                job.height > 0.f ? job.height : float(P.h));
                    Polyline3 pl3;
                    double    zt = -1e30;
                    for (const Vec3d &q : run) {
                        pl3.append(Point3(coord_t(scale_(q.x())), coord_t(scale_(q.y())), coord_t(scale_(q.z() - hz))));
                        zt = std::max(zt, q.z());
                    }
                    path->polyline        = std::move(pl3);
                    path->z_contoured     = true;
                    path->nonplanar_shell = true;
                    if (std::any_of(flows.begin(), flows.end(), [](float f) { return std::abs(f - 1.f) > 1e-4f; }))
                        path->np_flow = std::move(flows);
                    lifted[j].push_back(std::move(path));
                    job_top[j] = std::max(job_top[j], zt);
                }
            }
        });

        // One child collection per curved layer k, in print order: the G-code writes each as its own layer.
        auto shell = std::make_unique<ExtrusionEntityCollection>();
        shell->no_sort = true;
        for (int k = 0; k < P.n; ++ k) {
            auto layer_k = new ExtrusionEntityCollection();
            layer_k->no_sort = true;
            shell->entities.push_back(layer_k);
        }
        bool any = false;
        for (size_t j = 0; j < lifted.size(); ++ j)
            for (std::unique_ptr<ExtrusionPath> &path : lifted[j]) {
                stats.max_shell_z = std::max(stats.max_shell_z, job_top[j]);
                static_cast<ExtrusionEntityCollection *>(shell->entities[jobs[j].k])->entities.push_back(path.release());
                ++ stats.shell_paths;
                any = true;
            }
        if (! any)
            continue;
        {
            // What the top curved layer covers: only there is the planar part "under the shell" (no top of its own).
            Polygons cov = offset(top_fill_lines, float(0.5 * top_flow.scaled_width()));
            append(cov, offset(top_loop_lines, float(0.5 * ext_flow.scaled_width())));
            patch_cover[pid] = closing_ex(cov, float(0.5 * top_flow.scaled_spacing()));
            append(out.cover, patch_cover[pid]);
        }
        shells.emplace_back(layerm, std::move(shell));
        shell_pid.push_back(pid);
        ++ stats.patches;
        stats.curved_layers = std::max<size_t>(stats.curved_layers, size_t(P.n));
    }

    const auto t_clip = std::chrono::steady_clock::now();

    // ---- the planar part: the model as if its top were T lower, clipped at SLICE level (before perimeters and
    // infill, so every planar layer gets its normal closed walls, infill and its own top layers). Requirement: "all the
    // printing before that must be the same as if a thickness were simply taken off the top of the model".
    // Inside the footprint, a layer keeps material where the surface is at least (n - kclip) h above its slice
    // plane: that is the model's cross-section (n - kclip) h higher up, which for one top surface with nothing
    // above it is exactly the slice of the model offset down by T (kclip > 0 only on the clearance ramp).
    std::vector<ExPolygons> cut(layers.size());
    // With Z contouring on, the planar part is Z-contoured down onto the band's bottom (S - T) like a top surface, so
    // a layer keeps material wherever its BOTTOM is under S - T (less the thinnest a contoured layer may get), not only
    // where its top is within h/2 of it: the planar part then always reaches the band, never stops a step short of it,
    // and Z contouring lowers what sticks up (ContourZ.cpp, NonplanarZaaLimits::band_bottom).
    const bool   zaa_to_band = P.region != nullptr && P.region->zaa_enabled.value;
    const double zc_shift    = zaa_to_band ? -0.5 * P.h + std::clamp(P.region->zaa_min_z.value, 0., 0.5 * P.h) : 0.;
    // Every top's cut planes, sliced in one go (they mostly coincide: the same layers, the same kclip values).
    std::vector<float> zs_sorted;
    for (const Patch &p : patches) {
        std::vector<int> kv;
        for (int u : p.cells) kv.push_back(G.cells[u].kclip);
        sort_remove_duplicates(kv);
        for (int l = 0; l <= p.host && l < int(layers.size()); ++ l)
            for (int kq : kv) {
                const double zc = layers[l]->slice_z + (P.n - kq) * P.h + zc_shift;
                if (zc > p.smin - 1e-6) zs_sorted.push_back(float(zc));
            }
    }
    sort_remove_duplicates(zs_sorted);
    MeshSlicingParamsEx sp;
    sp.closing_radius = object.config().slice_closing_radius.value;
    const std::vector<ExPolygons> sec = zs_sorted.empty() ? std::vector<ExPolygons>() : slice_mesh_ex(its, zs_sorted, sp);
    for (const Patch &p : patches) {
        const int    pid = int(&p - patches.data());
        std::vector<int> kvals;
        for (int u : p.cells) kvals.push_back(G.cells[u].kclip);
        sort_remove_duplicates(kvals);
        // The cell raster per kclip; kclip 0 takes the whole footprint (grown past the outline) less the others.
        // Grown past the outline only (nothing to cut there): inside the top the clip never reaches past what the top
        // curved layer covers.
        // Nor past what the top curved layer covers: at a narrow tip the loops end short of the footprint's tip, and the
        // planar layers there are not cut.
        ExPolygons fp_plus = union_ex(p.fp0, diff_ex(offset_ex(p.fp0, float(scale_(0.5 * G.g))), p.real));
        {
            // (where a shingled seam hands layers over, the neighbouring band's top layer covers). The cover's outline,
            // not its holes: a hole in the top layer's beads where a deeper curved layer runs left a planar speck that
            // stood 0.2 mm over that deeper layer (tt_5).
            ExPolygons mask;
            for (const ExPolygon &e : patch_cover[pid]) mask.emplace_back(e.contour);
            for (const ExPolygons &h : p.handoff)
                append(mask, h);
            fp_plus = intersection_ex(fp_plus, offset_ex(union_ex(mask), float(scale_(0.5 * G.g))));
        }
        std::vector<ExPolygons> zone(kvals.size());
        ExPolygons ramp;
        for (size_t q = 0; q < kvals.size(); ++ q) {
            if (kvals[q] == 0) continue;
            const int kq = kvals[q];
            zone[q] = intersection_ex(cells_to_expolygons(G, [pid, kq](const Cell &c) { return c.ok && c.patch == pid && c.kclip == kq; }), fp_plus);
            append(ramp, zone[q]);
        }
        for (size_t q = 0; q < kvals.size(); ++ q)
            if (kvals[q] == 0) zone[q] = diff_ex(fp_plus, ramp);
        if (! kvals.empty() && kvals.front() > 0) {
            // No kclip-0 cell (a small top where only the upper curved layers clear, Benchy's helm under the roof): the
            // footprint past the cell raster, out to the outline where the edge loop runs, would belong to no zone and
            // its planar layers would stand over the edge loop. It goes to the zone of the cells beside it (the highest
            // kclip first: never cut deeper than the curved layers that print there). Only under the top layer's beads:
            // a planar wall beside the edge loop is not the top's to cut (Benchy's rim lost a layer there, 0.12 mm).
            ExPolygons rest = intersection_ex(diff_ex(fp_plus, ramp), patch_cover[pid]);
            for (size_t q = kvals.size(); q -- > 0 && ! rest.empty();) {
                const int        kq   = kvals[q];
                const ExPolygons part = intersection_ex(rest, offset_ex(cells_to_expolygons(G, [pid, kq](const Cell &c) { return c.ok && c.patch == pid && c.kclip == kq; }),
                                                                        float(scale_(3. * G.g))));
                if (part.empty()) continue;
                zone[q] = union_ex(zone[q], part);
                rest    = diff_ex(rest, part);
            }
        }
        // Slice planes to cut at.
        std::vector<float>               zs;
        std::vector<std::pair<int, int>> want; // (layer, zone)
        for (int l = 0; l <= p.host && l < int(layers.size()); ++ l)
            for (size_t q = 0; q < kvals.size(); ++ q) {
                const double zc = layers[l]->slice_z + (P.n - kvals[q]) * P.h + zc_shift;
                if (zc <= p.smin - 1e-6 || zone[q].empty()) continue; // the whole footprint is above: nothing to cut
                zs.push_back(float(zc));
                want.emplace_back(l, int(q));
            }
        // A top with a hollow under it (a roof over a cabin, a shelf): below its span's bottom the section T higher can be
        // air while the layer holds other material (the deck), which is not this top's to cut. Columns whose span starts
        // above the layer are left alone there (grown over the footprint's reach past the cells, never into cells that
        // are cut).
        float max_bot = -1e30f;
        for (int u : p.cells) max_bot = std::max(max_bot, G.cells[u].bot);
        const ExPolygons pcells = cells_to_expolygons(G, [pid](const Cell &c) { return c.ok && c.patch == pid; });
        int excl_layer = -1;
        ExPolygons excl;
        for (size_t w = 0; w < want.size(); ++ w) {
            const auto [l, q] = want[w];
            const size_t si = size_t(std::lower_bound(zs_sorted.begin(), zs_sorted.end(), zs[w]) - zs_sorted.begin());
            ExPolygons c = diff_ex(zone[q], si < sec.size() && zs_sorted[si] == zs[w] ? sec[si] : ExPolygons());
            if (double(max_bot) > layers[l]->slice_z - 1e-6 && ! c.empty()) {
                if (excl_layer != l) {
                    excl_layer = l;
                    const double sz = layers[l]->slice_z;
                    const ExPolygons hollow = cells_to_expolygons(G, [pid, sz](const Cell &cc) { return cc.ok && cc.patch == pid && double(cc.bot) > sz - 1e-6; });
                    excl.clear();
                    if (! hollow.empty()) {
                        excl = diff_ex(offset_ex(hollow, float(scale_(1.5 * G.g))), diff_ex(pcells, hollow));
                        append(excl, hollow);
                        excl = union_ex(excl);
                    }
                }
                if (! excl.empty())
                    c = diff_ex(c, excl);
            }
            append(cut[l], std::move(c));
        }
    }
    // Hold-back: a held island leaves its layer (it is printed after its top's shell, see below). Only for a top that got a
    // shell: otherwise nothing would print it.
    {
        std::vector<char> has_shell(patches.size(), 0);
        for (int pid : shell_pid) has_shell[size_t(pid)] = 1;
        held_islands.erase(std::remove_if(held_islands.begin(), held_islands.end(), [&](const HeldIsland &h) { return ! has_shell[size_t(h.pid)]; }),
                           held_islands.end());
        for (const HeldIsland &h : held_islands) {
            append(cut[h.layer], ExPolygons{ h.area });
            // the layer under a held island pauses there: its surface is covered later, so it is not ironed
            if (h.layer > 0) {
                out.held_above.resize(layers.size());
                out.held_above[h.layer - 1].push_back(h.area);
            }
            // the layer over the host stands on the island's last held layer
            const int host = patches[h.pid].host;
            if (h.layer == host && host + 1 < int(layers.size())) {
                out.held_under.resize(layers.size());
                out.held_under[host + 1].push_back(h.area);
            }
        }
    }
    // Right beside a top, a planar stair standing higher than the curved top's edge is trimmed to that edge
    // (+0.04 mm) within the outer half of the edge bead plus its core (~0.4 mm): the nozzle tracing the curved edge
    // would otherwise brush the stair corner (angle test: 0.08 mm). Off the tops, the part is untouched.
    {
        const double nozzle = pc.nozzle_diameter.values.empty() ? 0.4 : pc.nozzle_diameter.values.front();
        const int    R      = std::max(1, int(std::ceil(0.95 * nozzle / G.g)));
        // per column: the lowest top of a patch in it
        const size_t NC = size_t(G.ncols());
        std::vector<float> s_near(NC, 1e30f);
        std::vector<int>   d_near(NC, 1 << 20), p_near(NC, -1);
        std::vector<size_t> frontier;
        for (const Cell &c : G.cells)
            if (c.ok && c.patch >= 0 && c.s < s_near[c.col]) {
                if (d_near[c.col] != 0) frontier.push_back(size_t(c.col));
                s_near[c.col] = c.s; d_near[c.col] = 0; p_near[c.col] = c.patch;
            }
        for (int d = 1; d <= R && ! frontier.empty(); ++ d) {
            std::vector<size_t> next;
            for (size_t i : frontier) {
                const int a = int(i % G.nx), b = int(i / G.nx);
                const int nb[4][2] = { {a + 1, b}, {a - 1, b}, {a, b + 1}, {a, b - 1} };
                for (const auto &q : nb) {
                    if (q[0] < 0 || q[0] >= G.nx || q[1] < 0 || q[1] >= G.ny) continue;
                    const size_t qi = size_t(q[1]) * G.nx + q[0];
                    if (d_near[qi] <= d) { if (d_near[qi] == d && s_near[i] < s_near[qi]) { s_near[qi] = s_near[i]; p_near[qi] = p_near[i]; } continue; }
                    d_near[qi] = d; s_near[qi] = s_near[i]; p_near[qi] = p_near[i]; next.push_back(qi);
                }
            }
            frontier.swap(next);
        }
        // Only within reach of a top curved bead: where no curved bead runs (a thin tip the footprint's opening dropped)
        // the stair is the visible top and stays. A stair less than 0.04 mm over the edge (under the contact tolerance)
        // stays too: Benchy's rim lost a layer standing 0.01 mm over the edge, a 0.25 mm dip.
        const double     stair_tol = 0.04;
        const ExPolygons reach = offset_ex(union_ex(out.cover), float(scale_(0.75 * nozzle)));
        for (int l = 0; l < int(layers.size()); ++ l) {
            const double pz = layers[l]->print_z;
            ExPolygons band = cells_to_expolygons(G, [&](const Cell &c) {
                const size_t i = size_t(c.col);
                // only material printed before that top's shell (its host layer and below)
                return d_near[i] > 0 && d_near[i] <= R && pz > s_near[i] + stair_tol && l <= patches[p_near[i]].host;
            });
            if (! band.empty())
                band = intersection_ex(band, reach);
            if (! band.empty())
                append(cut[l], band);
        }
    }
    out.cut.assign(layers.size(), ExPolygons());
    if (std::getenv("NPTOP_PROBE") != nullptr && ! layers.empty()) {
        const BoundingBox b0 = get_extents(layers.front()->lslices);
        std::fprintf(stderr, "np probe: layer 0 bbox %.2f,%.2f .. %.2f,%.2f\n", unscale<double>(b0.min.x()), unscale<double>(b0.min.y()), unscale<double>(b0.max.x()), unscale<double>(b0.max.y()));
    }
    {
        std::atomic<size_t> nclipped(0);
        tbb::parallel_for(tbb::blocked_range<int>(0, int(layers.size())), [&](const tbb::blocked_range<int> &range) {
            for (int l = range.begin(); l < range.end(); ++ l) {
                if (cut[l].empty()) continue;
                // Hairline cuts and slivers where the footprint edge and the section nearly coincide are dropped:
                // they would only give walls around nothing.
                const float      eps = float(scale_(0.05));
                const ExPolygons c   = opening_ex(union_ex(cut[l]), eps);
                if (c.empty()) continue;
                out.cut[l] = c;
                bool any = false;
                for (LayerRegion *layerm : layers[l]->regions()) {
                    Surfaces kept;
                    for (const Surface &surf : layerm->slices.surfaces)
                        for (ExPolygon &ex : diff_ex(surf.expolygon, c))
                            // a fragment the cut leaves (less than a bead square) is not printable
                            if (std::abs(ex.area()) * SCALING_FACTOR * SCALING_FACTOR >= 0.1 || intersection_ex(offset_ex(ex, eps), c).empty())
                                kept.emplace_back(surf, std::move(ex));
                    if (kept.size() != layerm->slices.surfaces.size() || true) {
                        layerm->slices.surfaces = std::move(kept);
                        any = true;
                    }
                }
                if (any) {
                    layers[l]->make_slices();
                    ++ nclipped;
                }
                if (const char *probe = std::getenv("NPTOP_PROBE")) {
                    double px = 0, py = 0;
                    if (std::sscanf(probe, "%lf,%lf", &px, &py) == 2) {
                        const Point pp = Point::new_scale(px, py);
                        bool in_cut = false, in_kept = false;
                        for (const ExPolygon &ex : c) in_cut |= ex.contains(pp);
                        for (const ExPolygon &ex : layers[l]->lslices) in_kept |= ex.contains(pp);
                        std::fprintf(stderr, "np probe layer %d z %.2f: cut %d kept %d\n", l, layers[l]->print_z, int(in_cut), int(in_kept));
                    }
                }
            }
        });
        stats.clipped_layers = nclipped;
    }

    // Held islands: after the shell of their top, one more child collection per layer (the G-code writes every child as its own
    // layer), flat at the layer's own print_z, on the island's layer below: walls and solid lines.
    for (size_t si = 0; si < shells.size() && ! held_islands.empty(); ++ si) {
        const int    pid    = shell_pid[si];
        LayerRegion *layerm = shells[si].first;
        const double hz     = layerm->layer()->print_z;
        const Flow   ext_flow   = layerm->flow(frExternalPerimeter, P.h);
        const Flow   perim_flow = layerm->flow(frPerimeter, P.h);
        const Flow   solid_flow = layerm->flow(frSolidInfill, P.h);
        const int    n_loops    = std::max(1, layerm->region().config().wall_loops.value);
        std::map<int, ExPolygons> by_layer;
        for (const HeldIsland &h : held_islands)
            if (h.pid == pid) by_layer[h.layer].push_back(h.area);
        for (const auto &[l, areas] : by_layer) {
            const double pz = layers[l]->print_z;
            struct HJob { Polyline pl; ExtrusionRole role; const Flow *flow; };
            std::vector<HJob> hj;
            double d = 0.5 * ext_flow.scaled_width();
            for (int li = 0; li < n_loops; ++ li) {
                if (li == 1) d += 0.5 * (ext_flow.scaled_spacing() + perim_flow.scaled_spacing());
                else if (li > 1) d += perim_flow.scaled_spacing();
                const ExPolygons loops = offset_ex(areas, -float(d));
                if (loops.empty()) break;
                for (const ExPolygon &ex : loops)
                    for (const Polygon &pg : to_polygons(ex))
                        hj.push_back({ pg.split_at_first_point(), li == 0 ? erExternalPerimeter : erPerimeter, li == 0 ? &ext_flow : &perim_flow });
            }
            const Flow &last_flow = n_loops == 1 ? ext_flow : perim_flow;
            for (const ExPolygon &ex : offset_ex(areas, -float(d + 0.5 * last_flow.scaled_width() + 0.35 * solid_flow.scaled_width()))) {
                std::unique_ptr<Fill> fl(Fill::new_from_type(ipMonotonic));
                fl->set_bounding_box(object.bounding_box());
                fl->layer_id        = l;
                fl->z               = pz;
                fl->spacing         = solid_flow.spacing();
                fl->angle           = float(Geometry::deg2rad(45. + 90. * (l % 2)));
                fl->fixed_angle     = true;
                fl->link_max_length = coord_t(scale_(3. * fl->spacing));
                fl->overlap         = 0;
                FillParams fp;
                fp.density   = 1.f;
                fp.monotonic = true;
                Surface sf(stInternalSolid, ex);
                Polylines lines;
                try { lines = fl->fill_surface(&sf, fp); } catch (const Slic3r::CriticalException &) { lines.clear(); }
                for (Polyline &pl : lines)
                    hj.push_back({ std::move(pl), erSolidInfill, &solid_flow });
            }
            auto *coll = new ExtrusionEntityCollection();
            coll->no_sort = true;
            for (const HJob &j : hj) {
                if (j.pl.points.size() < 2)
                    continue;
                auto *path = new ExtrusionPath(j.role, j.flow->mm3_per_mm(), float(j.flow->width()), float(P.h));
                Polyline3 pl3;
                for (const Point &q : j.pl.points)
                    pl3.append(Point3(q.x(), q.y(), coord_t(scale_(pz - hz))));
                path->polyline        = std::move(pl3);
                path->z_contoured     = true;
                path->nonplanar_shell = true;
                coll->entities.push_back(path);
                ++ stats.shell_paths;
            }
            if (coll->entities.empty())
                delete coll;
            else
                shells[si].second->entities.push_back(coll);
        }
    }
    // The layer over a host stands on the held islands too, for its walls' overhang test (LayerRegion uses lslices_with_floor
    // as the layer below when it is set).
    for (Layer *ly : layers) ly->lslices_with_floor.clear();
    if (out.held_under.size() == layers.size())
        for (size_t l = 1; l < layers.size(); ++ l)
            if (! out.held_under[l].empty())
                layers[l]->lslices_with_floor = union_ex(layers[l - 1]->lslices, out.held_under[l]);

    // Shells: the patches of one layer region merge into one shell, layer k of every patch together, so curved
    // layer k is one layer in the G-code. Kept aside; make_nonplanar_top() puts them into the fills after infill.
    for (auto &[layerm, shell] : shells) {
        const size_t li = size_t(layerm->layer()->id());
        size_t ri = 0;
        for (size_t r = 0; r < layerm->layer()->regions().size(); ++ r)
            if (layerm->layer()->regions()[r] == layerm) ri = r;
        auto it = std::find_if(out.shells.begin(), out.shells.end(), [&](const NpCached::Shell &s) { return s.layer == li && s.region == ri; });
        if (it == out.shells.end()) {
            out.shells.push_back({ li, ri, std::move(shell) });
            continue;
        }
        for (size_t k = 0; k < shell->entities.size(); ++ k) {
            // (held islands make shells of one layer region differ in their number of children)
            if (k >= it->shell->entities.size()) {
                auto *add = new ExtrusionEntityCollection();
                add->no_sort = true;
                it->shell->entities.push_back(add);
            }
            auto *src = static_cast<ExtrusionEntityCollection *>(shell->entities[k]);
            auto *dst = static_cast<ExtrusionEntityCollection *>(it->shell->entities[k]);
            dst->entities.insert(dst->entities.end(), src->entities.begin(), src->entities.end());
            src->entities.clear();
        }
    }
    for (NpCached::Shell &sh : out.shells) {
        auto &ents = sh.shell->entities;
        for (auto it = ents.begin(); it != ents.end();) {
            if (static_cast<ExtrusionEntityCollection *>(*it)->entities.empty()) { delete *it; it = ents.erase(it); }
            else ++ it;
        }
    }

    const auto t_end = std::chrono::steady_clock::now();
    auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    stats.ms_analysis = ms(t_begin, t_shell);
    stats.ms_shells   = ms(t_shell, t_clip);
    stats.ms_clip     = ms(t_clip, t_end);
    BOOST_LOG_TRIVIAL(info) << "Nonplanar layers: " << stats.patches << " patch(es), " << stats.curved_layers << " curved layers, "
                            << stats.shell_paths << " shell paths, " << stats.clipped_layers << " clipped layers, max shell z "
                            << stats.max_shell_z << ", planar kept (no curved cover) " << stats.kept_planar_mm2 << " mm2; ms: analysis "
                            << stats.ms_analysis << ", shells " << stats.ms_shells << ", clip " << stats.ms_clip;
    if (std::getenv("NPTOP_DEBUG") != nullptr)
        std::fprintf(stderr, "np stats: patches %zu paths %zu clipped %zu kept_planar %.2f mm2 ms analysis %.0f shells %.0f clip %.0f\n",
                     stats.patches, stats.shell_paths, stats.clipped_layers, stats.kept_planar_mm2, stats.ms_analysis, stats.ms_shells, stats.ms_clip);
    return stats;
}

// The mesh the analysis casts rays on and slices, in the slice frame, and the cut-plane ceilings (as in
// PrintObject::contour_z()).
static void np_object_mesh(const PrintObject &object, TriangleMesh &mesh, std::vector<Layer::ContourZCeiling> &ceilings)
{
    for (const ModelVolume *v : object.model_object()->volumes)
        if (v->is_model_part() && (! object.is_segment() || segment_includes_volume(&*object.segment(), *v))) {
            TriangleMesh vol_mesh(v->mesh());
            // UNLAYERED_MOD -- keep a mirrored volume or instance outward-facing: the analysis reads the top
            // from the face normals (log review 11/02: a mirrored part got no curved top, silently)
            vol_mesh.transform(v->get_matrix(), true);
            mesh.merge(vol_mesh);
        }
    const Transform3d &trafo = object.object_to_slice();
    mesh.transform(trafo, true);
    if (object.is_segment()) {
        const Eigen::Matrix3d a_inv_t = trafo.linear().inverse().transpose();
        for (const CutSegmentClip &clip : object.segment()->clips) {
            const Vec3d  n_s = a_inv_t * clip.normal;
            const double len = n_s.norm();
            if (len > EPSILON)
                ceilings.push_back({ n_s / len, (clip.offset + n_s.dot(trafo.translation())) / len });
        }
    }
}

std::string nonplanar_top_clip_slices(PrintObject &object, size_t *skipped_tops)
{
    if (skipped_tops != nullptr)
        *skipped_tops = 0;
    {
        std::lock_guard<std::mutex> lock(g_np_cache_mutex);
        g_np_cache.erase(&object);
    }
    if (! nonplanar_top_requested(object) || object.layers().size() < 2)
        return std::string();
    TriangleMesh                        tmesh;
    std::vector<Layer::ContourZCeiling> ceilings;
    np_object_mesh(object, tmesh, ceilings);
    // UNLAYERED_MOD -- test the slice FRAME, not the part. object_to_slice() is the frame's rotation
    // (layer_rotation_b/c, a cut segment's build normal, a belt) times the object's own placement
    // object.trafo(), so a part that is merely rotated on the plate -- auto-orient, a rotated import --
    // read as "tilted" and lost its nonplanar layers (2026-11-02) although it is sliced in flat,
    // world-horizontal layers. np_object_mesh() already brings the mesh into the slice frame.
    // A tilted frame works with the angle limits reduced by the tilt (np_build); only one tilted past the limit is skipped.
    if (const double tilt = np_frame_tilt_deg(object); tilt > 1e-6) {
        const PrintRegionConfig *rc = nullptr;
        for (size_t r = 0; r < object.num_printing_regions() && rc == nullptr; ++ r)
            if (object.printing_region(r).config().nonplanar_layers.value)
                rc = &object.printing_region(r).config();
        if (rc != nullptr) {
            const double limit = nonplanar_effective_max_angle(*rc, object.print()->config());
            if (limit - tilt < kFlatSlopeDeg)
                return "this chunk is tilted " + std::to_string(int(std::lround(tilt))) + " degrees, more than its nonplanar angle limit of " +
                       std::to_string(int(std::lround(limit))) + " degrees allows";
        }
    }
    if (tmesh.empty())
        return std::string();
    sla::IndexedMesh imesh(tmesh);
    imesh.ground_level_offset(-tmesh.bounding_box().min.z());
    auto        cached = std::make_shared<NpCached>();
    std::string skip;
    const NonplanarTopStats stats = np_build(object, imesh, tmesh.its, ceilings, *cached, skip);
    if (skipped_tops != nullptr)
        *skipped_tops = stats.skipped_tops;
    if (! skip.empty()) {
        BOOST_LOG_TRIVIAL(info) << "Nonplanar layers skipped for " << object.model_object()->name << ": " << skip;
        if (std::getenv("NPTOP_DEBUG") != nullptr)
            std::fprintf(stderr, "np skipped for %s: %s\n", object.model_object()->name.c_str(), skip.c_str());
        return skip;
    }
    if (cached->shells.empty())
        return std::string();
    for (const Layer *l : object.layers()) cached->layers.push_back(l);
    cached->zaa = np_build_zaa(object, *cached);
    std::lock_guard<std::mutex> lock(g_np_cache_mutex);
    g_np_cache[&object] = std::move(cached);
    return std::string();
}

ExPolygons nonplanar_held_above(const Layer &layer)
{
    std::shared_ptr<NpCached> cached;
    {
        std::lock_guard<std::mutex> lock(g_np_cache_mutex);
        auto it = g_np_cache.find(layer.object());
        if (it != g_np_cache.end()) cached = it->second;
    }
    if (! cached || cached->held_above.empty())
        return ExPolygons();
    const auto it = std::find(cached->layers.begin(), cached->layers.end(), &layer);
    if (it == cached->layers.end())
        return ExPolygons();
    const size_t l = size_t(it - cached->layers.begin());
    return l < cached->held_above.size() ? union_ex(cached->held_above[l]) : ExPolygons();
}

std::shared_ptr<const NonplanarZaaLimits> nonplanar_top_zaa_limits(const PrintObject &object)
{
    std::shared_ptr<NpCached> cached;
    {
        std::lock_guard<std::mutex> lock(g_np_cache_mutex);
        auto it = g_np_cache.find(&object);
        if (it != g_np_cache.end()) cached = it->second;
    }
    const auto layers = object.layers();
    if (! cached || cached->layers.size() != layers.size() || ! std::equal(layers.begin(), layers.end(), cached->layers.begin()))
        return nullptr;
    return cached->zaa;
}

void nonplanar_top_retype_surfaces(PrintObject &object)
{
    std::shared_ptr<NpCached> cached;
    {
        std::lock_guard<std::mutex> lock(g_np_cache_mutex);
        auto it = g_np_cache.find(&object);
        if (it != g_np_cache.end()) cached = it->second;
    }
    const LayerPtrs &layers = object.layers();
    if (! cached || cached->layers.size() != layers.size() || ! std::equal(layers.begin(), layers.end(), cached->layers.begin()) ||
        cached->cut.size() != layers.size()) {
        if (std::getenv("NPTOP_DEBUG") != nullptr)
            std::fprintf(stderr, "np retype: no cache (%d)\n", int(cached != nullptr));
        return;
    }
    if (std::getenv("NPTOP_DEBUG") != nullptr) {
        size_t n = 0; for (const ExPolygons &c : cached->cut) n += ! c.empty();
        std::fprintf(stderr, "np retype: %zu layers with a cut\n", n);
    }
    // "The shell is the top": a planar top that only exists because the layer above was taken away for a shell
    // is not a top surface. It becomes internal: sparse infill runs up to the shell, which sits on it the way a planar
    // top shell sits on infill; no top shells grow down from it and nothing irons it. With little or no infill there
    // would be nothing to carry the first curved layer, so then it is one internal solid layer instead. (Internal
    // solid everywhere was +34 % filament on the coaster: Orca widens solid over sparse by its anchoring margins.)
    // When the curved layers are fewer than the planar top shell's layers, the difference is made up by that many internal
    // solid planar layers directly under the shell (not top surface, not ironed), so the whole top is as thick as a planar
    // top shell. The lowest of them is bridged over sparse infill by the normal bridge-over-infill step.
    const int m = solid_layers_under_shell(object);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, layers.size() > 0 ? layers.size() - 1 : 0), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t l = range.begin(); l < range.end(); ++ l) {
            // The shell's band starts within the next max(1, m) layers up from here.
            ExPolygons above;
            for (int j = 1; j <= std::max(1, m) && l + j < layers.size(); ++ j)
                append(above, cached->cut[l + j]);
            if (above.empty()) continue;
            const ExPolygons under = intersection_ex(offset_ex(union_ex(above), float(scale_(0.1))), cached->cover);
            if (under.empty()) continue;
            for (LayerRegion *layerm : layers[l]->regions()) {
                const SurfaceType st = m > 0 || layerm->region().config().sparse_infill_density.value < 10. ? stInternalSolid : stInternal;
                for (SurfaceCollection *sc : { &layerm->slices, &layerm->fill_surfaces }) {
                    Surfaces out;
                    bool changed = false;
                    for (Surface &s : sc->surfaces) {
                        if (s.surface_type != stTop && (m == 0 || s.surface_type != stInternal)) { out.push_back(std::move(s)); continue; }
                        ExPolygons in = intersection_ex(s.expolygon, under);
                        if (in.empty()) { out.push_back(std::move(s)); continue; }
                        changed = true;
                        for (ExPolygon &ex : diff_ex(s.expolygon, under)) out.emplace_back(s, std::move(ex));
                        for (ExPolygon &ex : in) { out.emplace_back(s, std::move(ex)); out.back().surface_type = st; }
                    }
                    sc->surfaces = std::move(out); // always: every surface was moved into `out`
                }
            }
        }
    });
    // Hold-back: the layer over a host stands on held islands printed with the shell; the clipped layer below has nothing
    // there, so it was classified as a bottom (a bridge over air).
    if (cached->held_under.size() == layers.size())
        for (size_t l = 0; l < layers.size(); ++ l) {
            if (cached->held_under[l].empty()) continue;
            const ExPolygons under = union_ex(cached->held_under[l]);
            for (LayerRegion *layerm : layers[l]->regions())
                for (SurfaceCollection *sc : { &layerm->slices, &layerm->fill_surfaces }) {
                    Surfaces out;
                    for (Surface &s : sc->surfaces) {
                        if (s.surface_type != stBottom && s.surface_type != stBottomBridge) { out.push_back(std::move(s)); continue; }
                        for (ExPolygon &ex : diff_ex(s.expolygon, under)) out.emplace_back(s, std::move(ex));
                        for (ExPolygon &ex : intersection_ex(s.expolygon, under)) { out.emplace_back(s, std::move(ex)); out.back().surface_type = stInternalSolid; }
                    }
                    sc->surfaces = std::move(out);
                }
        }
    // Hold-back, the other end: the layer a held island pauses on has nothing above it until the island resumes, so it
    // was classified as a top. It is covered later, so it is plain sparse infill, as if the island were already above it:
    // the infill grid is fixed per layer, so it runs on unbroken into the island; no top pattern, no solid band, no bridge
    // over it, no ironing (2026-11-02).
    if (cached->held_above.size() == layers.size())
        for (size_t l = 0; l < layers.size(); ++ l) {
            if (cached->held_above[l].empty()) continue;
            const ExPolygons above = union_ex(cached->held_above[l]);
            for (LayerRegion *layerm : layers[l]->regions())
                for (SurfaceCollection *sc : { &layerm->slices, &layerm->fill_surfaces }) {
                    Surfaces out;
                    for (Surface &s : sc->surfaces) {
                        if (s.surface_type != stTop) { out.push_back(std::move(s)); continue; }
                        for (ExPolygon &ex : diff_ex(s.expolygon, above)) out.emplace_back(s, std::move(ex));
                        for (ExPolygon &ex : intersection_ex(s.expolygon, above)) { out.emplace_back(s, std::move(ex)); out.back().surface_type = stInternal; }
                    }
                    sc->surfaces = std::move(out);
                }
        }
}

void nonplanar_top_shell_authority(PrintObject &object)
{
    if (std::getenv("NPTOP_NO_AUTH") != nullptr)
        return;
    std::shared_ptr<NpCached> cached;
    {
        std::lock_guard<std::mutex> lock(g_np_cache_mutex);
        auto it = g_np_cache.find(&object);
        if (it != g_np_cache.end()) cached = it->second;
    }
    const LayerPtrs &layers = object.layers();
    if (! cached || cached->layers.size() != layers.size() || ! std::equal(layers.begin(), layers.end(), cached->layers.begin()) ||
        cached->cut.size() != layers.size())
        return;
    // Orca makes the area under a removed top solid for the top-shell depth (vertical shells, horizontal shells, bridges
    // over infill) as if it were a planar top; under a curved shell that was +34 % filament on the coaster. Within the
    // shadow of a shell (its band in the next top_shell_layers layers up) solid and bridge infill go back to sparse,
    // except where a bottom surface needs its own shells.
    if (const char *probe = std::getenv("NPTOP_PROBE")) {
        double px = 0, py = 0;
        if (std::sscanf(probe, "%lf,%lf", &px, &py) == 2) {
            const Point pp = Point::new_scale(px, py);
            for (const Layer *ly : layers)
                for (const LayerRegion *lr : ly->regions())
                    for (const Surface &s : lr->fill_surfaces.surfaces)
                        if (s.expolygon.contains(pp))
                            std::fprintf(stderr, "np auth-start probe layer %zu: type %d\n", size_t(ly->id()), int(s.surface_type));
        }
    }
    const int solid_under = solid_layers_under_shell(object);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, layers.size()), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t l = range.begin(); l < range.end(); ++ l) {
            for (LayerRegion *layerm : layers[l]->regions()) {
                const PrintRegionConfig &rc = layerm->region().config();
                const int depth  = std::max(1, rc.top_shell_layers.value + 1);
                const int bdepth = std::max(0, rc.bottom_shell_layers.value);
                ExPolygons shadow;
                for (int j = 1; j <= depth && l + j < layers.size(); ++ j)
                    append(shadow, cached->cut[l + j]);
                if (shadow.empty()) continue;
                shadow = intersection_ex(union_ex(shadow), cached->cover);
                // Bottom shells keep their solid layers.
                ExPolygons bottoms;
                for (int j = 0; j <= bdepth && j <= int(l); ++ j)
                    for (const LayerRegion *lr : layers[l - j]->regions())
                        for (const Surface &s : lr->slices.surfaces)
                            if (s.surface_type == stBottom || s.surface_type == stBottomBridge) bottoms.push_back(s.expolygon);
                if (! bottoms.empty())
                    shadow = diff_ex(shadow, offset_ex(union_ex(bottoms), float(scale_(1.))));
                if (shadow.empty()) continue;
                // With little or no infill the layer right under the shell stays solid, to carry it.
                if (const char *probe = std::getenv("NPTOP_PROBE")) {
                    double px = 0, py = 0;
                    if (std::sscanf(probe, "%lf,%lf", &px, &py) == 2) {
                        const Point pp = Point::new_scale(px, py);
                        for (const Surface &s : layerm->fill_surfaces.surfaces)
                            if (s.expolygon.contains(pp))
                                std::fprintf(stderr, "np auth probe layer %zu z %.2f: fill surface type %d\n", l, layers[l]->print_z, int(s.surface_type));
                        bool in_sl = false;
                        for (const ExPolygon &ex : layers[l]->lslices) in_sl |= ex.contains(pp);
                        if (in_sl) std::fprintf(stderr, "np auth probe layer %zu: in lslices\n", l);
                    }
                }
                ExPolygons keep_solid;
                if (rc.sparse_infill_density.value < 10. && l + 1 < layers.size())
                    keep_solid = cached->cut[l + 1];
                // The solid layers under the shell (see nonplanar_top_retype_surfaces) stay solid.
                for (int j = 1; j <= solid_under && l + j < layers.size(); ++ j)
                    append(keep_solid, cached->cut[l + j]);
                if (solid_under > 0 && ! keep_solid.empty())
                    keep_solid = union_ex(keep_solid);
                Surfaces out;
                bool changed = false;
                for (Surface &s : layerm->fill_surfaces.surfaces) {
                    if (s.surface_type != stInternalSolid && s.surface_type != stInternalBridge && s.surface_type != stTop) {
                        out.push_back(std::move(s));
                        continue;
                    }
                    ExPolygons in = intersection_ex(s.expolygon, keep_solid.empty() ? shadow : diff_ex(shadow, keep_solid));
                    if (in.empty()) { out.push_back(std::move(s)); continue; }
                    changed = true;
                    for (ExPolygon &ex : diff_ex(s.expolygon, in)) out.emplace_back(s, std::move(ex));
                    for (ExPolygon &ex : in) {
                        out.emplace_back(s, std::move(ex));
                        out.back().surface_type = stInternal;
                    }
                }
                // (always put them back: the loop above moved every surface into `out`)
                layerm->fill_surfaces.surfaces = std::move(out);
                if (changed) {
                    if (const char *probe = std::getenv("NPTOP_PROBE")) {
                        double px = 0, py = 0;
                        if (std::sscanf(probe, "%lf,%lf", &px, &py) == 2) {
                            const Point pp = Point::new_scale(px, py);
                            for (const Surface &s : layerm->fill_surfaces.surfaces)
                                if (s.expolygon.contains(pp))
                                    std::fprintf(stderr, "np auth probe AFTER layer %zu z %.2f: fill surface type %d\n", l, layers[l]->print_z, int(s.surface_type));
                        }
                    }
                }
            }
        }
    });
}

NonplanarTopStats make_nonplanar_top(PrintObject &object, const sla::IndexedMesh &, const std::vector<Layer::ContourZCeiling> &)
{
    NonplanarTopStats stats;
    const LayerPtrs &layers = object.layers();
    if (const char *probe = std::getenv("NPTOP_PROBE")) {
        double px = 0, py = 0;
        if (std::sscanf(probe, "%lf,%lf", &px, &py) == 2) {
            const Point pp = Point::new_scale(px, py);
            for (const Layer *l : layers) {
                bool fs = false, sl = false;
                for (const LayerRegion *lr : l->regions()) {
                    for (const Surface &s : lr->fill_surfaces.surfaces) fs |= s.expolygon.contains(pp);
                    for (const Surface &s : lr->slices.surfaces) sl |= s.expolygon.contains(pp);
                }
                size_t np_close = 0;
                std::vector<const ExtrusionPath *> paths;
                for (const LayerRegion *lr : l->regions())
                    for (const ExtrusionEntity *e : lr->fills.entities) {
                        std::function<void(const ExtrusionEntity *)> v = [&](const ExtrusionEntity *x) {
                            if (auto *c = dynamic_cast<const ExtrusionEntityCollection *>(x)) { for (auto *y : c->entities) v(y); return; }
                            paths.clear(); collect_paths(x, paths);
                            for (const ExtrusionPath *p : paths)
                                for (const Point3 &q : p->polyline.points)
                                    np_close += (Point(q.x(), q.y()) - pp).cast<double>().norm() < scale_(0.8);
                        };
                        v(e);
                    }
                if (fs || sl || np_close)
                    std::fprintf(stderr, "np fills probe layer %zu z %.2f: fill_surface %d slices %d fill points np_close %zu\n", size_t(l->id()), l->print_z, int(fs), int(sl), np_close);
            }
        }
    }
    // Idempotent: drop shells a previous run left in the fills (ironing can re-run without infill).
    for (Layer *layer : layers)
        for (LayerRegion *layerm : layer->regions()) {
            ExtrusionEntitiesPtr &ents = layerm->fills.entities;
            for (auto it = ents.begin(); it != ents.end();) {
                if (is_shell(*it)) { delete *it; it = ents.erase(it); }
                else ++ it;
            }
        }
    std::shared_ptr<NpCached> cached;
    {
        std::lock_guard<std::mutex> lock(g_np_cache_mutex);
        auto it = g_np_cache.find(&object);
        if (it != g_np_cache.end()) cached = it->second;
    }
    if (! cached || cached->layers.size() != layers.size() || ! std::equal(layers.begin(), layers.end(), cached->layers.begin()))
        return stats;
    for (const NpCached::Shell &sh : cached->shells) {
        if (sh.layer >= layers.size() || sh.region >= layers[sh.layer]->regions().size()) continue;
        layers[sh.layer]->regions()[sh.region]->fills.entities.push_back(sh.shell->clone());
        ++ stats.patches;
        std::vector<const ExtrusionPath *> paths;
        for (const ExtrusionEntity *k : sh.shell->entities)
            for (const ExtrusionEntity *e : static_cast<const ExtrusionEntityCollection *>(k)->entities) { paths.clear(); collect_paths(e, paths); stats.shell_paths += paths.size(); }
    }
    return stats;
}

} // namespace Slic3r
