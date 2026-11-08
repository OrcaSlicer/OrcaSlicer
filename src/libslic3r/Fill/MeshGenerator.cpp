#include "MeshGenerator.hpp"

#include "../OpenVDBUtils.hpp"
#include "../TriangleMeshSlicer.hpp"
#include "FillGyroid.hpp"
#include "FillTpmsD.hpp"
#include "Implicits.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>

#include <boost/log/trivial.hpp>

namespace Slic3r {

bool mesh_backed_tpms_enabled()
{
    static const bool enabled = [] {
        const char *v = std::getenv("UNLAYERED_MESH_INFILL");
        return v != nullptr && *v != '\0' && std::strcmp(v, "0") != 0;
    }();
    return enabled;
}

// Shared walker/mesher for both representations:
//  - SolidBand: phi = |f| - thickness, the closed thin shell around f = 0.
//  - Surface:   phi = f (signed). The zero level set is the boundary between
//    the field's two labyrinths; after the narrow-band walk the inactive
//    interior must be sign-filled (signedFloodFill) or volumeToMesh would see
//    a phantom surface at the inner edge of the band.
static TriangleMesh make_implicit_mesh_impl(
    const std::function<float(const Vec3f&)>& f,
    const BoundingBoxf3& aabb,
    double thickness,
    double voxel_size,
    double seed_stride,
    double adaptivity,
    bool signed_field)
{
    if (!aabb.defined || voxel_size <= 0.0 || seed_stride <= 0.0)
        return {};
    if (!signed_field && thickness <= 0.0)
        return {};

    const Vec3d size_world = aabb.size();
    if (size_world.x() <= voxel_size || size_world.y() <= voxel_size || size_world.z() <= voxel_size)
        return {};

    openvdb::initialize();

    const float voxel             = float(voxel_size);
    const float thick             = float(thickness);
    // OpenVDB's conventional level-set narrow band; values outside ±band are
    // clamped to the background and the grid stays sparse there.
    constexpr float kNarrowBandV  = 3.0f;
    const float narrow_band_world = kNarrowBandV * voxel;

    auto transform = openvdb::math::Transform::createLinearTransform(voxel);
    auto grid      = openvdb::FloatGrid::create(/*background=*/ narrow_band_world);
    grid->setTransform(transform);
    grid->setGridClass(openvdb::GRID_LEVEL_SET);

    const openvdb::Coord ijk_lo(
        int(std::floor(aabb.min.x() / voxel_size)) - 1,
        int(std::floor(aabb.min.y() / voxel_size)) - 1,
        int(std::floor(aabb.min.z() / voxel_size)) - 1);
    const openvdb::Coord ijk_hi(
        int(std::ceil (aabb.max.x() / voxel_size)) + 1,
        int(std::ceil (aabb.max.y() / voxel_size)) + 1,
        int(std::ceil (aabb.max.z() / voxel_size)) + 1);

    // phi clamped to ±narrow_band keeps the grid sparse far from the surface
    // without changing where marching cubes finds the phi = 0 crossing.
    auto eval_phi = [&](const openvdb::Coord& c) -> float {
        const Vec3f p(float(c.x()) * voxel, float(c.y()) * voxel, float(c.z()) * voxel);
        const float fp  = f(p);
        const float phi = signed_field ? fp : std::fabs(fp) - thick;
        return std::clamp(phi, -narrow_band_world, narrow_band_world);
    };

    auto accessor = grid->getAccessor();
    std::deque<openvdb::Coord> queue;

    auto in_bounds = [&](const openvdb::Coord& c) {
        return c.x() >= ijk_lo.x() && c.x() <= ijk_hi.x()
            && c.y() >= ijk_lo.y() && c.y() <= ijk_hi.y()
            && c.z() >= ijk_lo.z() && c.z() <= ijk_hi.z();
    };

    if (signed_field) {
        // Surface mode: the field's "interior" is an entire labyrinth (half
        // the volume), so a narrow-band walk cannot represent the signs of
        // the inactive interior without a watertight flood fill — fragile
        // where the surface exits the walk bounds. Instead, evaluate the box
        // densely with clamped signed values: the caller streams in Z slabs,
        // so a slab is small (a few MB of voxels) and the plateaus at ±band
        // produce no spurious zero crossings. The mesh closes at the box
        // faces (interior negative voxels meet the positive background just
        // outside), giving the caps the per-region clip later removes.
        for (int i = ijk_lo.x(); i <= ijk_hi.x(); ++i)
            for (int j = ijk_lo.y(); j <= ijk_hi.y(); ++j)
                for (int k = ijk_lo.z(); k <= ijk_hi.z(); ++k) {
                    const openvdb::Coord c(i, j, k);
                    accessor.setValue(c, eval_phi(c));
                }
    } else {
        // Band mode: surface-walker BFS — admit while phi is below the cap,
        // which keeps the (thin) interior and the exterior fringe,
        // bracketing every phi = 0 crossing. Cost scales with surface area.
        auto try_visit = [&](const openvdb::Coord& c) {
            if (!in_bounds(c)) return;
            if (accessor.isValueOn(c)) return;
            const float phi = eval_phi(c);
            if (phi < narrow_band_world) {
                accessor.setValue(c, phi);
                queue.push_back(c);
            }
        };

        // Seed grid in voxel coordinates. Stride is rounded up so we never
        // overshoot the caller's world-space spacing constraint.
        const int seed_stride_voxels = std::max(1, int(std::ceil(seed_stride / voxel_size)));
        for (int i = ijk_lo.x(); i <= ijk_hi.x(); i += seed_stride_voxels)
            for (int j = ijk_lo.y(); j <= ijk_hi.y(); j += seed_stride_voxels)
                for (int k = ijk_lo.z(); k <= ijk_hi.z(); k += seed_stride_voxels)
                    try_visit(openvdb::Coord(i, j, k));

        static const std::array<openvdb::Coord, 6> kNeighbours = {
            openvdb::Coord( 1, 0, 0), openvdb::Coord(-1, 0, 0),
            openvdb::Coord( 0, 1, 0), openvdb::Coord( 0,-1, 0),
            openvdb::Coord( 0, 0, 1), openvdb::Coord( 0, 0,-1),
        };
        while (!queue.empty()) {
            const openvdb::Coord c = queue.front();
            queue.pop_front();
            for (const auto& d : kNeighbours)
                try_visit(c + d);
        }
    }

    if (grid->activeVoxelCount() == 0)
        return {};

    indexed_triangle_set its = grid_to_mesh(*grid, 0.0, std::clamp(adaptivity, 0.0, 1.0), true);
    if (its.indices.empty())
        return {};

    return TriangleMesh(std::move(its));
}

TriangleMesh make_implicit_shell(
    const std::function<float(const Vec3f&)>& f,
    const BoundingBoxf3& aabb,
    double thickness,
    double voxel_size,
    double seed_stride,
    double adaptivity)
{
    return make_implicit_mesh_impl(f, aabb, thickness, voxel_size, seed_stride, adaptivity, /*signed_field=*/false);
}

TriangleMesh make_implicit_surface(
    const std::function<float(const Vec3f&)>& f,
    const BoundingBoxf3& aabb,
    double voxel_size,
    double seed_stride,
    double adaptivity)
{
    return make_implicit_mesh_impl(f, aabb, 0., voxel_size, seed_stride, adaptivity, /*signed_field=*/true);
}

MeshInfillField make_mesh_infill_field(
    InfillPattern pattern, double density_fraction, double spacing, int multiline, double layer_height)
{
    MeshInfillField out;
    switch (pattern) {
    case ipTpmsD: {
        // FillTpmsD: scaleFactor = spacing / density_adjusted, field period
        // = 2*pi * scaleFactor (see make_waves derivation in FillTpmsD.cpp).
        const double density_adjusted = std::max(0.001, density_fraction * FillTpmsD::DensityAdjust / multiline);
        out.period   = float(2.0 * M_PI * spacing / density_adjusted);
        out.inv_grad = out.period / float(2.0 * M_PI);
        const float period = out.period, inv_grad = out.inv_grad;
        out.field = [period, inv_grad](const Vec3f &p) { return schwarz_d(p, period) * inv_grad; };
        break;
    }
    case ipTpmsFK: {
        // FillTpmsFK: vari_T = 4.18 * spacing * multiline / min(0.9, density).
        const float density_factor = std::min(0.9f, float(density_fraction));
        out.period   = 4.18f * float(spacing) * float(multiline) / std::max(0.001f, density_factor);
        // FK's cos(2.) terms double the field gradient.
        out.inv_grad = out.period / float(4.0 * M_PI);
        const float period = out.period, inv_grad = out.inv_grad;
        out.field = [period, inv_grad](const Vec3f &p) { return fischer_koch_s(p, period) * inv_grad; };
        break;
    }
    case ipGyroid: {
        const double density_adjusted = std::max(0.001, density_fraction * FillGyroid::DensityAdjust / multiline);
        const float  density_factor   = std::max(0.001f, float(density_fraction * FillGyroid::DensityAdjust / multiline));
        out.period   = float(2.0 * M_PI) * float(spacing) / density_factor;
        out.omega    = compute_omega_factor(density_adjusted, spacing * multiline, layer_height);
        out.inv_grad = out.period / float(2.0 * M_PI);
        const GyroidParams gp { out.period, float(out.omega) };
        const float inv_grad = out.inv_grad;
        out.field = [gp, inv_grad](const Vec3f &p) { return gyroid(p, gp) * inv_grad; };
        break;
    }
    default:
        break; // period stays 0: unsupported
    }
    return out;
}

TriangleMesh make_mesh_infill_preview_mesh(
    const MeshInfillField &field_spec,
    const BoundingBoxf3   &aabb,
    ImplicitMeshMode       mode,
    int                    wall_count,
    double                 spacing)
{
    if (field_spec.period <= 0.f || ! field_spec.field)
        return {};
    const double period      = double(field_spec.period);
    const double thickness   = mode == ImplicitMeshMode::SolidBand ? 0.5 * spacing * std::max(1, wall_count) : 0.;
    const double voxel_size  = mode == ImplicitMeshMode::SolidBand
        ? std::clamp(std::min(period / 20., thickness / 1.5), 0.05, 1.0)
        : std::clamp(period / 20., 0.05, 1.0);
    const double adaptivity  = mode == ImplicitMeshMode::Surface ? 0. : 0.5;
    BoundingBoxf3 padded = aabb;
    padded.offset(thickness + 2. * voxel_size);
    return mode == ImplicitMeshMode::Surface
        ? make_implicit_surface(field_spec.field, padded, voxel_size, period / 4., adaptivity)
        : make_implicit_shell(field_spec.field, padded, thickness, voxel_size, period / 4., adaptivity);
}

std::vector<ExPolygons> make_implicit_shell_slices(
    const std::function<float(const Vec3f&)>& f,
    const BoundingBoxf3& aabb,
    double thickness,
    double voxel_size,
    double seed_stride,
    double adaptivity,
    const std::vector<float>& zs,
    float closing_radius,
    const std::function<void()>& throw_on_cancel,
    double slab_height,
    TriangleMesh* merged_out,
    size_t* total_facets,
    ImplicitMeshMode mode)
{
    std::vector<ExPolygons> out(zs.size());
    if (zs.empty() || !aabb.defined || slab_height <= 0.)
        return out;
    // zs from PrintObject::layers() are ascending; the slab partition below
    // relies on it.
    assert(std::is_sorted(zs.begin(), zs.end()));

    // The slab mesh must extend past the outermost sliced z by more than the
    // band's own reach, so the end-caps (where the shell is cut off by the
    // slab AABB) are never inside the sliced range.
    const double z_margin = thickness + 3. * voxel_size;
    if (total_facets != nullptr)
        *total_facets = 0;

    size_t begin = 0;
    while (begin < zs.size()) {
        const double slab_z_lo = zs[begin];
        size_t end = begin;
        while (end + 1 < zs.size() && zs[end + 1] <= slab_z_lo + slab_height)
            ++end;

        BoundingBoxf3 slab_aabb = aabb;
        slab_aabb.min.z() = std::max(aabb.min.z(), double(zs[begin]) - z_margin);
        slab_aabb.max.z() = std::min(aabb.max.z(), double(zs[end])   + z_margin);

        TriangleMesh slab = make_implicit_mesh_impl(f, slab_aabb, thickness, voxel_size, seed_stride, adaptivity,
                                                    /*signed_field=*/mode == ImplicitMeshMode::Surface);
        throw_on_cancel();
        if (! slab.empty()) {
            const std::vector<float> slab_zs(zs.begin() + begin, zs.begin() + end + 1);
            std::vector<ExPolygons> slices = slice_mesh_ex(slab.its, slab_zs, closing_radius, throw_on_cancel);
            for (size_t i = 0; i < slices.size(); ++i)
                out[begin + i] = std::move(slices[i]);
            if (total_facets != nullptr)
                *total_facets += slab.facets_count();
            BOOST_LOG_TRIVIAL(debug) << "make_implicit_shell_slices: slab z=[" << zs[begin] << ", " << zs[end]
                                     << "] " << slab.facets_count() << " facets, " << slab_zs.size() << " layers";
            if (merged_out != nullptr)
                merged_out->merge(slab);
        }
        begin = end + 1;
    }
    return out;
}

} // namespace Slic3r
