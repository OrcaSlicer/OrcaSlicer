#include "TextureBakePipeline.hpp"

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include "TextureBakeDebug.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

#include <boost/log/trivial.hpp>

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
                            const PipelineProgressFn &on_progress, BakeStageRecorder *debug,
                            const ColorSampleFn &color_sample)
{
    PipelineResult result;
    const auto     report = [&](const char *stage, double f) {
        return !on_progress || on_progress(stage, f);
    };
    // Per-stage wall time. The stages differ in cost by orders of magnitude depending on the model, so
    // without this it is guesswork which one to attack.
    auto       clock_now = [] { return std::chrono::steady_clock::now(); };
    auto       t_stage   = clock_now();
    // One call site for both the log line and the debug capture, so a stage cannot appear in one and
    // be missing from the other. The capture happens after the elapsed time is read: welding the soup
    // and scanning its edges costs more than some of the stages do, and must not land inside the
    // measurement it is reporting.
    const auto lap = [&](const char *stage, const TriSoup &geometry, const std::string &detail = {}) {
        const double ms = std::chrono::duration<double, std::milli>(clock_now() - t_stage).count();
        BOOST_LOG_TRIVIAL(info) << "TextureBake " << stage << ": " << ms << " ms, "
                                << geometry.triangle_count() << " tris";
        if (debug != nullptr)
            debug->capture(stage, geometry, ms, detail);
        t_stage = clock_now();
    };

    if (input.empty() || !sample) {
        result.geometry = input;
        return result;
    }
    if (debug != nullptr)
        debug->capture("input", input, 0.0, "as handed to the pipeline");
    t_stage = clock_now(); // the capture above is not part of the first stage

    // 1. Refine to the target edge length.
    SubdivideResult sub = subdivide(
        input, settings.refine_length, face_excluded, /* fast */ false, settings.safety_cap,
        [&](double f, size_t, double) { return report("subdivide", f); });
    result.safety_cap_hit = sub.safety_cap_hit;
    lap("subdivide", sub.geometry);
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
        lap("regularize", reg.geometry, std::to_string(reg.collapse_count) + " collapses");
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
            lap("re-subdivide", sub.geometry);
        } else {
            sub.geometry       = std::move(reg.geometry);
            sub.face_parent_id = std::move(reg.face_parent_id);
        }
    }

    // 2b. Paint finer than the input triangles. The caller includes a source triangle when any part of
    // it is painted; now that the faces are small, ask once more per face and switch the unpainted
    // ones off. They are pinned like the excluded region from here on (their own corners at weight
    // 1, and the displacement's boundary sealing pins the stroke's rim on the painted side), but they
    // are refined pieces of painted triangles, not original geometry, so `soft_excluded` keeps them
    // out of the decimation lock below. Every stage between here and the decimation rewrites faces in
    // place, so the per-face flag stays valid by index.
    std::vector<uint8_t> soft_excluded;
    if (settings.painted) {
        const size_t         nf     = sub.geometry.triangle_count();
        const bool           have_w = !sub.geometry.exclude_weight.empty();
        std::vector<uint8_t> unpainted(nf, 0);
        tbb::parallel_for(tbb::blocked_range<size_t>(0, nf), [&](const tbb::blocked_range<size_t> &r) {
            for (size_t t = r.begin(); t < r.end(); ++t) {
                if (have_w && sub.geometry.exclude_weight[t * 3] > 0.99f)
                    continue; // excluded from the start, never asked
                const Vec3f &a = sub.geometry.pos[t * 3], &b = sub.geometry.pos[t * 3 + 1], &c = sub.geometry.pos[t * 3 + 2];
                if (!settings.painted((a + b + c) / 3.f))
                    unpainted[t] = 1;
            }
        });
        size_t switched = 0;
        for (size_t t = 0; t < nf; ++t)
            switched += unpainted[t];
        if (switched > 0) {
            if (sub.geometry.exclude_weight.empty())
                sub.geometry.exclude_weight.assign(sub.geometry.pos.size(), 0.f);
            for (size_t t = 0; t < nf; ++t)
                if (unpainted[t])
                    sub.geometry.exclude_weight[t * 3] = sub.geometry.exclude_weight[t * 3 + 1] =
                        sub.geometry.exclude_weight[t * 3 + 2] = 1.f;
            soft_excluded = std::move(unpainted);
        }
        lap("paint", sub.geometry, std::to_string(switched) + " faces switched off");
        if (!report("paint", 1.0)) {
            result.canceled = true;
            return result;
        }
    }

    // 3. Align the mesh to the height field's edges, then displace.
    if (settings.relocate) {
        std::vector<uint8_t> locked;
        if (settings.preserve_untextured && !sub.geometry.exclude_weight.empty()) {
            locked.assign(sub.geometry.triangle_count(), 0);
            for (size_t t = 0; t < locked.size(); ++t)
                locked[t] = sub.geometry.exclude_weight[t * 3] > 0.99f ? 1 : 0;
        }
        RelocateResult rel = relocate_to_contours(sub.geometry, sample, settings.relocate_opts, locked);
        BOOST_LOG_TRIVIAL(info) << "TextureBake relocate: moved=" << rel.moved
                                << " rejected=" << rel.rejected;
        sub.geometry = std::move(rel.geometry);
        lap("relocate", sub.geometry,
            "moved " + std::to_string(rel.moved) + ", rejected " + std::to_string(rel.rejected));
    }

    // 3b. Diagonals along the height field's steps, so they displace into straight walls.
    if (settings.flip_edges) {
        std::vector<uint8_t> locked;
        if (settings.preserve_untextured && !sub.geometry.exclude_weight.empty()) {
            locked.assign(sub.geometry.triangle_count(), 0);
            for (size_t t = 0; t < locked.size(); ++t)
                locked[t] = sub.geometry.exclude_weight[t * 3] > 0.99f ? 1 : 0;
        }
        FlipResult fl = flip_edges_to_height(sub.geometry, sub.face_parent_id, sample, settings.flip_opts, locked);
        sub.geometry       = std::move(fl.geometry);
        sub.face_parent_id = std::move(fl.face_parent_id);
        lap("align edges", sub.geometry, std::to_string(fl.flipped) + " flips");
        if (!report("align edges", 1.0)) {
            result.canceled = true;
            return result;
        }
    }

    TriSoup displaced = apply_displacement(sub.geometry, sample, settings.displace, bounds,
                                           [&](double f) { return report("displace", f); });
    lap("displace", displaced);
    if (!report("displace", 1.0)) {
        result.canceled = true;
        return result;
    }

    // 4. Decimate - export only. A bake needs the face-parent map, which a collapse destroys.
    std::vector<int>   parent                   = std::move(sub.face_parent_id);
    const size_t       displaced_before_decimate = displaced.triangle_count();
    if (mode == PipelineMode::Export) {
        std::vector<uint8_t> locked;
        size_t               preserved = 0;
        {
            if (settings.preserve_untextured && !displaced.exclude_weight.empty()) {
                locked.assign(displaced.triangle_count(), 0);
                // The corner average, as the displacement stage judges it: after the flip stage's
                // per-vertex merge an included face touching the excluded region carries one corner
                // at weight 1, and must stay free to collapse and to take colour.
                for (size_t t = 0; t < locked.size(); ++t)
                    locked[t] = (displaced.exclude_weight[t * 3] + displaced.exclude_weight[t * 3 + 1] +
                                 displaced.exclude_weight[t * 3 + 2]) / 3.f > 0.99f ? 1 : 0;
                // Faces the paint test switched off carry weight 1 too, but are refined pieces of
                // painted triangles rather than original geometry: locking them would keep a partly
                // painted source triangle at full refinement. Face indices survived relocate, flip
                // and displace unchanged, so the flag still lines up.
                for (size_t t = 0; t < locked.size() && t < soft_excluded.size(); ++t)
                    if (soft_excluded[t])
                        locked[t] = 0;
                preserved = size_t(std::count(locked.begin(), locked.end(), uint8_t(1)));
            }
        }
        // The budget is what this bake may spend on what it refines. Geometry it only preserves - the
        // relief of an earlier bake, which this one does not paint - is counted on top of it: charged
        // against the same budget, a second bake over a fresh area had to evict the first one's
        // triangles to fit, so every bake after the first came out coarser than the one before.
        const size_t target = settings.max_triangles + preserved;
        const bool   over_budget = displaced.triangle_count() > target;
        // Only when the mesh is actually over budget: the decimation pass also welds the soup and is
        // followed by the T-junction repair, and putting an under-budget bake through both changed the
        // sliced result by a fifth even with the collapse tolerance at zero, i.e. with nothing
        // collapsed. Harvesting flat faces on a mesh that already fits needs that path to leave the
        // geometry alone first.
        if (over_budget) {
            // Colour per face on the fine mesh, so colour boundaries become creases the collapse
            // respects. Excluded (unpainted) faces take no colour.
            std::vector<int> face_color;
            if (color_sample) {
                const size_t nf = displaced.triangle_count();
                face_color.assign(nf, -1);
                const bool have_w = !displaced.exclude_weight.empty();
                tbb::parallel_for(tbb::blocked_range<size_t>(0, nf), [&](const tbb::blocked_range<size_t> &r) {
                    for (size_t t = r.begin(); t < r.end(); ++t) {
                        if (have_w && (displaced.exclude_weight[t * 3] + displaced.exclude_weight[t * 3 + 1] +
                                       displaced.exclude_weight[t * 3 + 2]) / 3.f > 0.99f)
                            continue;
                        const Vec3f &a = displaced.pos[t * 3], &b = displaced.pos[t * 3 + 1], &c = displaced.pos[t * 3 + 2];
                        face_color[t] = color_sample((a + b + c) / 3.f, displaced.nrm[t * 3]);
                    }
                });
            }
            const size_t before = displaced.triangle_count();
            DecimateResult dec = decimate(displaced, target, settings.harvest_flat,
                                          settings.harvest_tol, locked,
                                          [&](double f) { return report("decimate", f); }, face_color);
            result.locked_over_budget = dec.locked_over_budget;
            displaced                 = std::move(dec.geometry);
            lap("decimate", displaced, "over budget, simplified");
            BOOST_LOG_TRIVIAL(info) << "TextureBake decimate: " << before << " -> " << displaced.triangle_count()
                                    << " (budget " << target << ")";
            parent.clear(); // no longer meaningful
        }
        result.triangles_refined = displaced_before_decimate;
        result.triangles_budget  = target;
        result.budget_limited    = over_budget;
        if (!report("decimate", 1.0)) {
            result.canceled = true;
            return result;
        }
    }

    // 5. Flatten the bed-contact surface.
    {
        const bool clamped = settings.clamp_below_plate || settings.displace.bottom_angle_limit > 0.f;
        if (clamped)
            clamp_below_bottom(displaced, bounds.min.z());
        size_t snapped = 0;
        if (settings.bottom_snap_tol > 0.0)
            snapped = snap_bottom_to_flat(displaced, bounds.min.z(), settings.bottom_snap_tol);
        if (clamped || settings.bottom_snap_tol > 0.0)
            lap("bottom clamp + snap", displaced, std::to_string(snapped) + " triangles snapped flat");
    }

    // 6. Close the T-junctions decimation left behind. Only meaningful when it ran.
    if (mode == PipelineMode::Export && parent.empty()) {
        displaced = resolve_t_junctions(displaced);
        lap("repair", displaced);
    }

    result.geometry       = std::move(displaced);
    result.face_parent_id = std::move(parent);
    return result;
}

} // namespace TextureBake
} // namespace Slic3r
