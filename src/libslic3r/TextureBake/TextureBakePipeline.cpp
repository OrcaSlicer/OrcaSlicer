#include "TextureBakePipeline.hpp"

#include <algorithm>
#include <cmath>

namespace Slic3r {
namespace TextureBake {

void clamp_below_bottom(TriSoup &geometry, float bottom_z)
{
    for (size_t t = 0; t + 2 < geometry.pos.size(); t += 3) {
        bool dirty = false;
        for (int k = 0; k < 3; ++k)
            if (geometry.pos[t + size_t(k)].z() < bottom_z) {
                geometry.pos[t + size_t(k)].z() = bottom_z;
                dirty = true;
            }
        if (!dirty)
            continue;
        Vec3f       n   = (geometry.pos[t + 1] - geometry.pos[t]).cross(geometry.pos[t + 2] - geometry.pos[t]);
        const float len = n.norm();
        n = (len > 0.f) ? Vec3f(n / len) : Vec3f(0.f, 0.f, 1.f);
        geometry.nrm[t] = geometry.nrm[t + 1] = geometry.nrm[t + 2] = n;
    }
}

size_t snap_bottom_to_flat(TriSoup &geometry, float bottom_z, double tol)
{
    const size_t vert_count = geometry.pos.size();
    const size_t tri_count  = vert_count / 3;
    if (tri_count == 0 || tol <= 0.0)
        return 0;

    // Weld at the finest grid: by this point copies of one position are bit-identical, because every
    // earlier stage moved them by the same vector.
    QuantizedPointMap weld(WELD_GRID_DECIMATION, std::min(vert_count, size_t(1) << 22));
    std::vector<int>  vid(vert_count);
    int               unique = 0;
    for (size_t i = 0; i < vert_count; ++i) {
        vid[i] = weld.get_or_set(geometry.pos[i], unique);
        if (weld.inserted())
            ++unique;
    }
    // Incident corners per position, CSR style.
    std::vector<uint32_t> start(size_t(unique) + 1, 0);
    for (size_t i = 0; i < vert_count; ++i)
        ++start[size_t(vid[i]) + 1];
    for (size_t id = 0; id < size_t(unique); ++id)
        start[id + 1] += start[id];
    std::vector<uint32_t> inc(vert_count), cursor(size_t(unique), 0);
    for (size_t i = 0; i < vert_count; ++i)
        inc[start[size_t(vid[i])] + cursor[size_t(vid[i])]++] = uint32_t(i);

    const double         fold_cos = std::cos(75.0 * M_PI / 180.0);
    std::vector<uint8_t> dirty_tri(tri_count, 0);

    for (size_t id = 0; id < size_t(unique); ++id) {
        const float z = geometry.pos[inc[start[id]]].z();
        if (z == bottom_z || std::abs(double(z) - double(bottom_z)) > tol)
            continue;

        // Simulate the move: every incident triangle must keep positive area and must not fold.
        bool ok = true;
        for (uint32_t k = start[id]; k < start[id + 1] && ok; ++k) {
            const size_t t = size_t(inc[k]) / 3;
            Vec3f        p[3];
            for (int v = 0; v < 3; ++v) {
                p[v] = geometry.pos[t * 3 + size_t(v)];
                if (vid[t * 3 + size_t(v)] == int(id))
                    p[v].z() = bottom_z;
            }
            const Vec3d on = (geometry.pos[t * 3 + 1] - geometry.pos[t * 3])
                                 .cross(geometry.pos[t * 3 + 2] - geometry.pos[t * 3]).cast<double>();
            const Vec3d nn = (p[1] - p[0]).cross(p[2] - p[0]).cast<double>();
            const double o2 = on.squaredNorm(), n2 = nn.squaredNorm();
            if (n2 < 1e-20) { ok = false; break; }  // would collapse to zero area
            if (o2 < 1e-20) continue;               // already degenerate, cannot judge a rotation
            const double dot = on.dot(nn);
            if (dot < 0.0 || dot * dot < fold_cos * fold_cos * o2 * n2)
                ok = false;
        }
        if (!ok)
            continue;

        for (uint32_t k = start[id]; k < start[id + 1]; ++k) {
            geometry.pos[inc[k]].z() = bottom_z;
            dirty_tri[size_t(inc[k]) / 3] = 1;
        }
    }

    size_t dirty = 0;
    for (size_t t = 0; t < tri_count; ++t) {
        if (!dirty_tri[t])
            continue;
        ++dirty;
        Vec3f       n   = (geometry.pos[t * 3 + 1] - geometry.pos[t * 3])
                        .cross(geometry.pos[t * 3 + 2] - geometry.pos[t * 3]);
        const float len = n.norm();
        n = (len > 0.f) ? Vec3f(n / len) : Vec3f(0.f, 0.f, 1.f);
        geometry.nrm[t * 3] = geometry.nrm[t * 3 + 1] = geometry.nrm[t * 3 + 2] = n;
    }
    return dirty;
}

PipelineResult run_pipeline(const TriSoup &input, const HeightSampleFn &sample,
                            const PipelineSettings &settings, const DisplaceBounds &bounds,
                            PipelineMode mode, const std::vector<uint8_t> &face_excluded,
                            const PipelineProgressFn &on_progress)
{
    PipelineResult result;
    const auto     report = [&](const char *stage, double f) {
        return !on_progress || on_progress(stage, f);
    };

    if (input.empty() || !sample) {
        result.geometry = input;
        return result;
    }

    // 1. Refine to the target edge length.
    SubdivideResult sub = subdivide(
        input, settings.refine_length, face_excluded, /* fast */ false, settings.safety_cap,
        [&](double f, size_t, double) { return report("subdivide", f); });
    result.safety_cap_hit = sub.safety_cap_hit;
    if (!report("subdivide", 1.0)) {
        result.canceled = true;
        return result;
    }

    // 2. Dissolve the slivers refinement inherited, then recover the edges that lengthened.
    if (settings.regularize) {
        RegularizeOptions ropts = settings.regularize_opts;
        ropts.preserve_excluded = settings.preserve_untextured;
        RegularizeResult reg = regularize_mesh(sub.geometry, sub.face_parent_id,
                                               settings.refine_length, ropts);
        result.collapse_count = reg.collapse_count;
        if (!report("regularize", 1.0)) {
            result.canceled = true;
            return result;
        }
        if (reg.collapse_count > 0) {
            // Excluded faces are carried on the soup itself, so the flag is re-derived rather than
            // indexed across the collapse.
            std::vector<uint8_t> excl;
            if (!reg.geometry.exclude_weight.empty()) {
                excl.assign(reg.geometry.triangle_count(), 0);
                for (size_t t = 0; t < excl.size(); ++t)
                    excl[t] = reg.geometry.exclude_weight[t * 3] > 0.99f ? 1 : 0;
            }
            sub = subdivide(reg.geometry, settings.refine_length * settings.regularize_second_pass_mul,
                            excl, false, settings.safety_cap,
                            [&](double f, size_t, double) { return report("re-subdivide", f); });
            result.safety_cap_hit = result.safety_cap_hit || sub.safety_cap_hit;
            // The second pass renumbers faces, so the parent map has to be composed through it.
            std::vector<int> composed(sub.face_parent_id.size());
            for (size_t i = 0; i < composed.size(); ++i) {
                const int mid = sub.face_parent_id[i];
                composed[i] = (mid >= 0 && size_t(mid) < reg.face_parent_id.size())
                                  ? reg.face_parent_id[size_t(mid)] : -1;
            }
            sub.face_parent_id = std::move(composed);
        } else {
            sub.geometry       = std::move(reg.geometry);
            sub.face_parent_id = std::move(reg.face_parent_id);
        }
    }

    // 3. Displace.
    TriSoup displaced = apply_displacement(sub.geometry, sample, settings.displace, bounds,
                                           [&](double f) { return report("displace", f); });
    if (!report("displace", 1.0)) {
        result.canceled = true;
        return result;
    }

    // 4. Decimate - export only. A bake needs the face-parent map, which a collapse destroys.
    std::vector<int> parent = std::move(sub.face_parent_id);
    if (mode == PipelineMode::Export) {
        const bool needs_decimation = displaced.triangle_count() > settings.max_triangles;
        if (needs_decimation || settings.harvest_flat) {
            std::vector<uint8_t> locked;
            if (settings.preserve_untextured && !displaced.exclude_weight.empty()) {
                locked.assign(displaced.triangle_count(), 0);
                for (size_t t = 0; t < locked.size(); ++t)
                    locked[t] = displaced.exclude_weight[t * 3] > 0.99f ? 1 : 0;
            }
            DecimateResult dec = decimate(displaced, settings.max_triangles, settings.harvest_flat,
                                          settings.harvest_tol, locked,
                                          [&](double f) { return report("decimate", f); });
            result.locked_over_budget = dec.locked_over_budget;
            displaced                 = std::move(dec.geometry);
            parent.clear(); // no longer meaningful
        }
        if (!report("decimate", 1.0)) {
            result.canceled = true;
            return result;
        }
    }

    // 5. Flatten the bed-contact surface.
    if (settings.displace.bottom_angle_limit > 0.f)
        clamp_below_bottom(displaced, bounds.min.z());
    if (settings.bottom_snap_tol > 0.0)
        snap_bottom_to_flat(displaced, bounds.min.z(), settings.bottom_snap_tol);

    // 6. Close the T-junctions decimation left behind. Only meaningful when it ran.
    if (mode == PipelineMode::Export && parent.empty())
        displaced = resolve_t_junctions(displaced);

    result.geometry       = std::move(displaced);
    result.face_parent_id = std::move(parent);
    return result;
}

} // namespace TextureBake
} // namespace Slic3r
