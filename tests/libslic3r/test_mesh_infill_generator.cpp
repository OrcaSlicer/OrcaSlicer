#include <catch2/catch_all.hpp>

#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Fill/FillMeshBacked.hpp"

#include <functional>
#include "libslic3r/Fill/Implicits.hpp"
#include "libslic3r/Fill/MeshGenerator.hpp"
#include "libslic3r/Surface.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

TEST_CASE("Gyroid implicit field", "[MeshInfill]") {
    const GyroidParams gp { 10.f, 1.f };

    SECTION("zero at origin") {
        REQUIRE_THAT(gyroid(Vec3f(0.f, 0.f, 0.f), gp), WithinAbs(0.0, 1e-6));
    }

    SECTION("known value at quarter period") {
        // a = pi/2, b = c = 0: sin(a)cos(b) + sin(b)cos(c) + sin(c)cos(a) = 1
        REQUIRE_THAT(gyroid(Vec3f(gp.period / 4.f, 0.f, 0.f), gp), WithinAbs(1.0, 1e-5));
    }

    SECTION("periodic in every axis") {
        const Vec3f p(1.234f, -2.345f, 3.456f);
        const float f0 = gyroid(p, gp);
        REQUIRE_THAT(gyroid(p + Vec3f(gp.period, 0.f, 0.f), gp), WithinAbs(f0, 1e-4));
        REQUIRE_THAT(gyroid(p + Vec3f(0.f, gp.period, 0.f), gp), WithinAbs(f0, 1e-4));
        // Z period is period / omega; with omega = 1 it equals period.
        REQUIRE_THAT(gyroid(p + Vec3f(0.f, 0.f, gp.period), gp), WithinAbs(f0, 1e-4));
    }

    SECTION("Schwarz-D zero at origin-diagonal points and periodic") {
        // f = sin*sin*sin - cos*cos*cos; at the origin f = -1, at the cell
        // center (period/4 in all axes) f = +1, zero along the way.
        REQUIRE_THAT(schwarz_d(Vec3f(0.f, 0.f, 0.f), 10.f), WithinAbs(-1.0, 1e-5));
        REQUIRE_THAT(schwarz_d(Vec3f(2.5f, 2.5f, 2.5f), 10.f), WithinAbs(1.0, 1e-5));
        const Vec3f p(1.1f, -0.7f, 2.3f);
        REQUIRE_THAT(schwarz_d(p + Vec3f(10.f, 0.f, 0.f), 10.f), WithinAbs(schwarz_d(p, 10.f), 1e-4));
    }

    SECTION("Fischer-Koch S periodic and matches its freq form") {
        const Vec3f p(0.9f, 1.7f, -2.2f);
        REQUIRE_THAT(fischer_koch_s(p + Vec3f(0.f, 10.f, 0.f), 10.f), WithinAbs(fischer_koch_s(p, 10.f), 1e-4));
        REQUIRE_THAT(fischer_koch_s_freq(p, float(2.0 * PI) / 10.f), WithinAbs(fischer_koch_s(p, 10.f), 1e-6));
    }

    SECTION("omega factor clamped to [1, 2]") {
        REQUIRE_THAT(compute_omega_factor(1.0, 0.45, 0.2), WithinAbs(1.0, 1e-9));   // dense -> lower clamp
        REQUIRE_THAT(compute_omega_factor(0.01, 0.45, 0.2), WithinAbs(2.0, 1e-9));  // sparse -> upper clamp
        const double mid = compute_omega_factor(0.3, 0.45, 0.2);
        REQUIRE(mid > 1.0);
        REQUIRE(mid < 2.0);
    }
}

// make_implicit_shell expects f in approximate world-unit signed distance;
// rescale the dimensionless gyroid by its gradient magnitude 2π/period.
static float gyroid_sdf(const Vec3f &p, const GyroidParams &gp)
{
    return gyroid(p, gp) * gp.period / float(2.0 * M_PI);
}

TEST_CASE("make_implicit_surface meshes the signed level set", "[MeshInfill]") {
    const GyroidParams gp { 10.f, 1.f };
    auto field = [&gp](const Vec3f &p) { return gyroid_sdf(p, gp); };
    const BoundingBoxf3 aabb(Vec3d(-10., -10., -10.), Vec3d(10., 10., 10.));
    const double voxel = 0.5; // period/20 — no band to resolve in surface mode

    TriangleMesh surf = make_implicit_surface(field, aabb, voxel, gp.period / 4.);
    REQUIRE_FALSE(surf.empty());

    SECTION("watertight") {
        REQUIRE(surf.stats().open_edges == 0);
    }

    SECTION("slices to labyrinth area polygons, roughly half the box") {
        std::vector<ExPolygons> slices = slice_mesh_ex(surf.its, { 0.f, 4.f });
        const double box_area = scale_(20.) * scale_(20.);
        for (size_t i = 0; i < slices.size(); ++i) {
            INFO("slice " << i);
            REQUIRE_FALSE(slices[i].empty());
            double area = 0.;
            for (const ExPolygon &ep : slices[i])
                area += ep.area();
            // One labyrinth of the gyroid fills ~50% of space.
            REQUIRE(area > 0.3 * box_area);
            REQUIRE(area < 0.7 * box_area);
        }
    }

    SECTION("streaming surface slices match monolithic") {
        std::vector<float> zs { -6.f, -2.f, 0.f, 2.f, 6.f };
        std::vector<ExPolygons> mono = slice_mesh_ex(surf.its, zs);
        std::vector<ExPolygons> streamed = make_implicit_shell_slices(
            field, aabb, /*thickness=*/0., voxel, gp.period / 4., /*adaptivity=*/0.,
            zs, /*closing_radius=*/0.f, []() {}, /*slab_height=*/6., nullptr, nullptr,
            ImplicitMeshMode::Surface);
        REQUIRE(streamed.size() == mono.size());
        for (size_t i = 0; i < zs.size(); ++i) {
            INFO("z = " << zs[i]);
            double area_mono = 0., area_streamed = 0.;
            for (const ExPolygon &ep : mono[i])     area_mono     += ep.area();
            for (const ExPolygon &ep : streamed[i]) area_streamed += ep.area();
            REQUIRE_THAT(area_streamed, Catch::Matchers::WithinRel(area_mono, 0.01));
        }
    }
}

TEST_CASE("make_mesh_infill_preview_mesh generates for all TPMS patterns", "[MeshInfill]") {
    const BoundingBoxf3 aabb(Vec3d(-8., -8., 0.), Vec3d(8., 8., 12.));
    for (InfillPattern pattern : { ipGyroid, ipTpmsD, ipTpmsFK }) {
        DYNAMIC_SECTION("pattern " << int(pattern)) {
            MeshInfillField spec = make_mesh_infill_field(pattern, 0.15, 0.45, 1, 0.2);
            REQUIRE(spec.period > 0.f);
            REQUIRE(spec.field);
            TriangleMesh preview = make_mesh_infill_preview_mesh(spec, aabb, ImplicitMeshMode::Surface, 1, 0.45);
            REQUIRE_FALSE(preview.empty());
            REQUIRE(preview.stats().open_edges == 0);
        }
    }
    SECTION("unsupported pattern yields empty") {
        MeshInfillField spec = make_mesh_infill_field(ipRectilinear, 0.15, 0.45, 1, 0.2);
        REQUIRE(spec.period <= 0.f);
        REQUIRE(make_mesh_infill_preview_mesh(spec, aabb, ImplicitMeshMode::Surface, 1, 0.45).empty());
    }
}

TEST_CASE("FillMeshBacked density gate", "[MeshInfill]") {
    // Pre-sliced data: one square band outline at z = 1.0, prepared at 15%.
    MeshInfillSlices slices;
    slices.prepared_density = 0.15f;
    ExPolygon band(Polygon(Points{ Point::new_scale(1., 1.), Point::new_scale(9., 1.),
                                   Point::new_scale(9., 9.), Point::new_scale(1., 9.) }));
    slices.slices_by_print_z[MeshInfillSlices::key_for_print_z(1.0)] = ExPolygons{ band };

    Surface surf(stInternal, ExPolygon(Polygon(Points{ Point::new_scale(0., 0.), Point::new_scale(10., 0.),
                                                       Point::new_scale(10., 10.), Point::new_scale(0., 10.) })));
    auto configure = [](Fill &f) { f.z = 1.0; f.spacing = 0.45; f.angle = 0.f; f.layer_id = 5; };
    FillParams params;
    params.density = 0.15f;

    FillMeshBackedGyroid fmb;
    configure(fmb);
    fmb.mesh_slices = &slices;

    SECTION("matching density consumes the mesh slices") {
        Polylines out = fmb.fill_surface(&surf, params);
        REQUIRE(out.size() == 1);
        // The band's closed 8x8 square trace, ~32mm long.
        REQUIRE_THAT(unscale<double>(coord_t(out.front().length())), Catch::Matchers::WithinRel(32., 0.01));
    }

    SECTION("density mismatch falls back to analytic FillGyroid") {
        params.density = 0.30f;
        Polylines fallback_out = fmb.fill_surface(&surf, params);
        FillGyroid analytic;
        configure(analytic);
        Polylines analytic_out = analytic.fill_surface(&surf, params);
        REQUIRE(fallback_out.size() == analytic_out.size());
        double len_a = 0., len_b = 0.;
        for (const Polyline &pl : fallback_out) len_a += pl.length();
        for (const Polyline &pl : analytic_out) len_b += pl.length();
        REQUIRE_THAT(len_a, Catch::Matchers::WithinRel(len_b, 1e-9));
    }

    SECTION("pattern mismatch falls back (mesh prepared for another TPMS pattern)") {
        slices.pattern = ipTpmsD;
        Polylines fallback_out = fmb.fill_surface(&surf, params);
        FillGyroid analytic;
        configure(analytic);
        Polylines analytic_out = analytic.fill_surface(&surf, params);
        REQUIRE(fallback_out.size() == analytic_out.size());
        slices.pattern = ipGyroid;
    }

    SECTION("SolidBand mode fills band cross-sections with Arachne walls") {
        // 1mm-wide square annulus band, to be filled with 2 walls at ~0.45mm.
        ExPolygon annulus(Polygon(Points{ Point::new_scale(1., 1.), Point::new_scale(9., 1.),
                                          Point::new_scale(9., 9.), Point::new_scale(1., 9.) }));
        annulus.holes.emplace_back(Polygon(Points{ Point::new_scale(2., 2.), Point::new_scale(2., 8.),
                                                   Point::new_scale(8., 8.), Point::new_scale(8., 2.) }));
        slices.mode       = ImplicitMeshMode::SolidBand;
        slices.wall_count = 2;
        slices.slices_by_print_z[MeshInfillSlices::key_for_print_z(1.0)] = ExPolygons{ annulus };

        PrintConfig       print_config;
        PrintObjectConfig print_object_config;
        fmb.print_config        = &print_config;
        fmb.print_object_config = &print_object_config;
        FillParams p2 = params;
        p2.flow         = Flow(0.45f, 0.2f, 0.4f);
        p2.layer_height = 0.2f;

        ExtrusionEntitiesPtr out;
        fmb.fill_surface_extrusion(&surf, p2, out);
        REQUIRE_FALSE(out.empty());
        double total_len = 0.;
        std::function<void(const ExtrusionEntity *)> add_length = [&](const ExtrusionEntity *ee) {
            if (auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(ee))
                for (const ExtrusionEntity *child : collection->entities)
                    add_length(child);
            else
                total_len += unscaled<double>(ee->length());
        };
        for (const ExtrusionEntity *ee : out)
            add_length(ee);
        // Two concentric walls around an 8x8/6x6 annulus: roughly the two
        // ring perimeters plus transitions (~32 + ~27 + joins); generous corridor.
        REQUIRE(total_len > 40.);
        REQUIRE(total_len < 95.);
        for (ExtrusionEntity *ee : out)
            delete ee;
        // restore for other sections
        slices.mode = ImplicitMeshMode::Surface;
        slices.wall_count = 1;
    }

    SECTION("non-internal surface falls back regardless of density") {
        Surface solid(stInternalSolid, surf.expolygon);
        Polylines fallback_out = fmb.fill_surface(&solid, params);
        FillGyroid analytic;
        configure(analytic);
        Polylines analytic_out = analytic.fill_surface(&solid, params);
        REQUIRE(fallback_out.size() == analytic_out.size());
    }
}

TEST_CASE("make_implicit_shell rejects degenerate input", "[MeshInfill]") {
    const GyroidParams gp { 10.f, 1.f };
    auto field = [&gp](const Vec3f &p) { return gyroid_sdf(p, gp); };
    const BoundingBoxf3 aabb(Vec3d(-10., -10., -10.), Vec3d(10., 10., 10.));

    SECTION("undefined aabb")       { REQUIRE(make_implicit_shell(field, BoundingBoxf3(), 0.5, 0.25, 2.5).empty()); }
    SECTION("non-positive thickness") { REQUIRE(make_implicit_shell(field, aabb, 0.0, 0.25, 2.5).empty()); }
    SECTION("non-positive voxel")   { REQUIRE(make_implicit_shell(field, aabb, 0.5, 0.0, 2.5).empty()); }
    SECTION("non-positive stride")  { REQUIRE(make_implicit_shell(field, aabb, 0.5, 0.25, 0.0).empty()); }
    SECTION("sub-voxel aabb") {
        const BoundingBoxf3 tiny(Vec3d(0., 0., 0.), Vec3d(0.1, 0.1, 0.1));
        REQUIRE(make_implicit_shell(field, tiny, 0.5, 0.25, 2.5).empty());
    }
    SECTION("field with no zero crossing in range") {
        auto far_field = [](const Vec3f &) { return 100.f; };
        REQUIRE(make_implicit_shell(far_field, aabb, 0.5, 0.25, 2.5).empty());
    }
}

TEST_CASE("make_implicit_shell meshes a gyroid band", "[MeshInfill]") {
    const GyroidParams gp { 10.f, 1.f };
    auto field = [&gp](const Vec3f &p) { return gyroid_sdf(p, gp); };
    const BoundingBoxf3 aabb(Vec3d(-10., -10., -10.), Vec3d(10., 10., 10.));
    const double thickness = 0.5, voxel = 0.25;

    TriangleMesh shell = make_implicit_shell(field, aabb, thickness, voxel, gp.period / 4.);
    REQUIRE_FALSE(shell.empty());

    SECTION("watertight") {
        REQUIRE(shell.stats().open_edges == 0);
    }

    SECTION("stays inside the padded aabb") {
        const BoundingBoxf3 mesh_bb = shell.bounding_box();
        const double pad = 2. * voxel;
        REQUIRE(mesh_bb.min.x() >= aabb.min.x() - pad);
        REQUIRE(mesh_bb.min.y() >= aabb.min.y() - pad);
        REQUIRE(mesh_bb.min.z() >= aabb.min.z() - pad);
        REQUIRE(mesh_bb.max.x() <= aabb.max.x() + pad);
        REQUIRE(mesh_bb.max.y() <= aabb.max.y() + pad);
        REQUIRE(mesh_bb.max.z() <= aabb.max.z() + pad);
    }

    SECTION("slices to non-empty closed polygons on interior layers") {
        const std::vector<float> zs { -5.f, 0.f, 5.f };
        std::vector<ExPolygons> slices = slice_mesh_ex(shell.its, zs, float(thickness / 2.));
        REQUIRE(slices.size() == zs.size());
        for (size_t i = 0; i < slices.size(); ++i) {
            INFO("slice at z = " << zs[i]);
            REQUIRE_FALSE(slices[i].empty());
            for (const ExPolygon &ep : slices[i])
                REQUIRE(ep.contour.area() > 0);
        }
    }

    SECTION("no geometry outside the aabb in z") {
        std::vector<ExPolygons> outside = slice_mesh_ex(shell.its, { 15.f }, float(thickness / 2.));
        REQUIRE(outside.front().empty());
    }

    SECTION("streaming slab output matches monolithic slicing") {
        std::vector<float> zs;
        for (float z = -8.f; z <= 8.f; z += 1.f)
            zs.push_back(z);
        std::vector<ExPolygons> mono = slice_mesh_ex(shell.its, zs, float(thickness / 2.));
        // 5 mm slabs over a 20 mm span -> 4 slabs; margin logic must keep the
        // slab end-caps outside every sliced z.
        std::vector<ExPolygons> streamed = make_implicit_shell_slices(
            field, aabb, thickness, voxel, gp.period / 4., /*adaptivity=*/0.,
            zs, float(thickness / 2.), []() {}, /*slab_height=*/5.);
        REQUIRE(streamed.size() == mono.size());
        for (size_t i = 0; i < zs.size(); ++i) {
            INFO("z = " << zs[i]);
            REQUIRE(streamed[i].size() == mono[i].size());
            double area_mono = 0., area_streamed = 0.;
            for (const ExPolygon &ep : mono[i])     area_mono     += ep.area();
            for (const ExPolygon &ep : streamed[i]) area_streamed += ep.area();
            REQUIRE_THAT(area_streamed, Catch::Matchers::WithinRel(area_mono, 0.01));
        }
    }

    SECTION("seed stride coarser than the period still finds the surface") {
        // The BFS walker expands from any seed that lands in the band, so even
        // a sparse seed lattice must find the (connected) gyroid surface.
        TriangleMesh sparse_seeded = make_implicit_shell(field, aabb, thickness, voxel, 2. * gp.period);
        REQUIRE_FALSE(sparse_seeded.empty());
        // Same surface -> comparable size (within 25%; marching output may differ slightly).
        const double ratio = double(sparse_seeded.facets_count()) / double(shell.facets_count());
        REQUIRE(ratio > 0.75);
        REQUIRE(ratio < 1.25);
    }
}
