#include <catch2/catch_all.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/AABBTreeIndirect.hpp"
#include "libslic3r/SkeletonBuild.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <cmath>
#include <vector>

using namespace Slic3r;

// Cut planes are placed at skeleton edge MIDPOINTS, so a skeleton edge lying in air is a cut
// plane centred on nothing. smooth_skeleton_graph() takes optional V/F to constrain exactly that,
// and its own header records the cost of leaving them out: 7.1% of skeleton length outside the
// material before smoothing, 14.2% after. This pins the containment of the collapse-to-spine
// path, which is the path a user is pushed onto in practice -- the cell mask is a uint64, so a
// branched part hits the 64-plane wall and "Collapse to spine" is what gets it under.
namespace {

// A washer: OD 50, ID 30, H 8. Chosen because it is the corpus shape where the failure is
// unmissable -- the skeleton must follow the material RING, and unconstrained Laplacian smoothing
// chords each arc inward until the graph sits in the central hole. A solid convex part cannot
// show this at all (measured 0% either way on a solid T), which is why it is the wrong fixture.
indexed_triangle_set make_washer(double r_outer = 22.0, double r_inner = 18.0, double h = 8.0,
                                 int n = 72)
{
    indexed_triangle_set its;
    auto &V = its.vertices;
    auto &F = its.indices;
    V.reserve(4 * n);

    // 0..n-1 outer bottom, n..2n-1 outer top, 2n..3n-1 inner bottom, 3n..4n-1 inner top
    for (int pass = 0; pass < 2; ++pass) {
        const double r = pass == 0 ? r_outer : r_inner;
        for (int lvl = 0; lvl < 2; ++lvl) {
            const double z = lvl == 0 ? 0.0 : h;
            for (int i = 0; i < n; ++i) {
                const double a = 2.0 * M_PI * double(i) / double(n);
                V.emplace_back(Vec3f(float(r * std::cos(a)), float(r * std::sin(a)), float(z)));
            }
        }
    }
    const int OB = 0, OT = n, IB = 2 * n, IT = 3 * n;

    // Winding matters: CGAL needs a closed, consistently outward-oriented manifold or
    // skeletonization and the genus check both bail. Verified numerically for these four
    // patterns -- every directed edge appears exactly once with an opposite twin, the divergence
    // -theorem volume is +99.84% of pi*(R^2-r^2)*h (the rest is the polygonal circle), and
    // chi = 288-864+576 = 0, so genus 1 as a washer should be. Getting the two caps backwards
    // gives chi = 128 and a third of the volume, which reads as a plausible mesh right up until
    // nothing works.
    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        // outer wall, normals +r
        F.emplace_back(OB + i, OB + j, OT + j);
        F.emplace_back(OB + i, OT + j, OT + i);
        // inner wall, normals -r, i.e. out of the material toward the bore axis
        F.emplace_back(IB + i, IT + i, IT + j);
        F.emplace_back(IB + i, IT + j, IB + j);
        // top annulus, normals +z
        F.emplace_back(OT + i, OT + j, IT + j);
        F.emplace_back(OT + i, IT + j, IT + i);
        // bottom annulus, normals -z
        F.emplace_back(OB + i, IB + i, IB + j);
        F.emplace_back(OB + i, IB + j, OB + j);
    }
    return its;
}

// Inside/outside by parity of ray crossings, which needs no CGAL and no signed distance. The
// direction is deliberately not axis-aligned: a +X ray from a point on the washer's y=0 plane
// grazes coincident edges and the parity becomes a coin flip.
struct Containment
{
    const indexed_triangle_set                        &its;
    AABBTreeIndirect::Tree3f                           tree;

    explicit Containment(const indexed_triangle_set &m)
        : its(m), tree(AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(m.vertices, m.indices))
    {}

    bool inside(const Vec3d &p) const
    {
        // Vec3d origin/direction against float vertices, as every working call site does
        // (test_aabbindirect.cpp, SeamPlacer.cpp) -- the template deduces VectorType from these
        // two and does not accept Vec3f here.
        const Vec3d dir(0.5773502692, 0.5773502692, 0.5773502692);
        std::vector<igl::Hit<float>> hits;
        AABBTreeIndirect::intersect_ray_all_hits(its.vertices, its.indices, tree, p, dir, hits);
        return (hits.size() % 2) == 1;
    }

    // Fraction of total edge length lying outside the solid, sampled uniformly per edge.
    // SkeletonGraph keeps undirected adjacency rather than an edge list, so each pair is
    // visited once by taking only u < v.
    double outside_fraction(const SkeletonGraph &G, int samples = 25) const
    {
        double total = 0., outside = 0.;
        for (int u = 0; u < G.num_nodes(); ++u)
            for (int v : G.adj[u]) {
                if (v <= u)
                    continue;
                const Vec3d a = G.nodes[u], b = G.nodes[v];
                const double len = (b - a).norm();
                if (len <= 0.)
                    continue;
                int in_count = 0;
                for (int s = 0; s < samples; ++s)
                    if (this->inside(a + (b - a) * (double(s) / double(samples - 1))))
                        ++in_count;
                total   += len;
                outside += len * (1.0 - double(in_count) / double(samples));
            }
        return total > 0. ? outside / total : 0.;
    }
};

// min/max distance from the Z axis, for diagnosing WHERE a skeleton sits relative to the walls.
std::pair<double,double> radius_range(const SkeletonGraph &G)
{
    double lo = 1e30, hi = -1e30;
    for (const auto &n : G.nodes) {
        const double r = std::hypot(n.x(), n.y());
        lo = std::min(lo, r);
        hi = std::max(hi, r);
    }
    return {lo, hi};
}

SkeletonParams washer_params(bool collapse_to_spine)
{
    SkeletonParams p;                        // CLI/GUI defaults
    p.collapse_to_spine = collapse_to_spine;
    // The washer's bore aspect scores 0.27 against the 1.2 threshold, so Threshold already keeps
    // the loop; pinned explicitly so a future default change cannot quietly turn this into a
    // bore-tracing test, where "outside the material" is the CORRECT answer.
    p.tunnel_fill = TunnelFill::Off;
    return p;
}

} // namespace

SCENARIO("skeleton stays inside the material", "[skeleton]")
{
    const indexed_triangle_set washer = make_washer();
    const Containment          cont(washer);

    // A containment metric that cannot return false would make every assertion below pass
    // vacuously, so pin it on points whose answer is not in question.
    GIVEN("the containment probe itself") {
        THEN("it separates material from hole and from outside") {
            CHECK(cont.inside(Vec3d(20.0, 0.0, 4.0)));      // mid-wall, r=20 between 18 and 22
            CHECK(cont.inside(Vec3d(0.0, -20.0, 1.0)));     // same ring, another quadrant
            CHECK_FALSE(cont.inside(Vec3d(0.0, 0.0, 4.0)));  // dead centre: the hole
            CHECK_FALSE(cont.inside(Vec3d(20.0, 0.0, 40.0))); // above the part
            CHECK_FALSE(cont.inside(Vec3d(60.0, 0.0, 4.0)));  // outside the OD
        }
    }

    GIVEN("a washer, whose skeleton must follow the material ring") {
        WHEN("the skeleton is built without collapsing to a spine") {
            const SkeletonResult r = build_skeleton(washer, washer_params(false));
            THEN("essentially all of it is inside the solid") {
                REQUIRE(r.graph.num_nodes() > 2);
                const double frac = cont.outside_fraction(r.graph);
                const auto rr = radius_range(r.graph);
                UNSCOPED_INFO("nodes=" << r.graph.num_nodes() << " outside_fraction=" << frac << " radius " << rr.first << ".." << rr.second);
                CHECK(frac < 0.05);
            }
        }

        WHEN("the skeleton is collapsed to a spine") {
            const SkeletonResult r = build_skeleton(washer, washer_params(true));
            THEN("it must not escape into the hole") {
                REQUIRE(r.graph.num_nodes() > 2);
                // The bug this pins: smooth_skeleton_graph protects CYCLIC components with
                // Taubin (no net shrink), keyed on |E| >= |V| in the component. Collapsing a
                // ring yields a PATH, which drops out of that branch into plain Laplacian, and
                // the arc then chords inward through the bore wall. The containment guard does
                // not stop it, because outer_loop_at consults only depth-0 loops -- a node in
                // the bore still reads as "inside", on purpose, so traced bore axes pass.
                //
                // Measured on this fixture: spine radius 17.4532..19.7522 and 53.4% of its
                // length outside the solid before the fix; 19.6435..19.7522 and 0.0% after, i.e.
                // sitting on the material mid-line where it belongs. The corpus OD50/ID30
                // washer went to r 13.08 with 82.9% outside.
                //
                // This bound is the canary. If it trips, find out what started smoothing a
                // cyclic source again rather than widening it.
                const double frac = cont.outside_fraction(r.graph);
                const auto rr = radius_range(r.graph);
                UNSCOPED_INFO("nodes=" << r.graph.num_nodes() << " outside_fraction=" << frac << " radius " << rr.first << ".." << rr.second);
                CHECK(frac < 0.10);
            }
        }
    }
}
