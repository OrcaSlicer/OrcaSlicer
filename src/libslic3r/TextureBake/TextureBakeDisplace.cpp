#include "TextureBakeDisplace.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Slic3r {
namespace TextureBake {

TriSoup apply_displacement(const TriSoup &geometry, const HeightSampleFn &sample,
                           const DisplaceSettings &settings, const DisplaceBounds &bounds,
                           const DisplaceProgressFn &on_progress)
{
    TriSoup      out;
    const size_t count = geometry.pos.size();
    if (count == 0 || !sample)
        return geometry;

    out.pos.resize(count);
    out.nrm.resize(count);

    // Everything below is keyed by this id, which is what makes one vector per position expressible.
    const bool need_id_positions = settings.boundary_falloff > 0.f;
    QuantizedPointMap dedup(WELD_GRID_GEOMETRY, std::min(count, size_t(1) << 22));
    std::vector<int>  vertex_id(count);
    std::vector<Vec3f> id_pos;
    int                next_id = 0;
    for (size_t i = 0; i < count; ++i) {
        const int id = dedup.get_or_set(geometry.pos[i], next_id);
        if (dedup.inserted()) {
            ++next_id;
            if (need_id_positions)
                id_pos.push_back(geometry.pos[i]);
        }
        vertex_id[i] = id;
    }
    const size_t unique_count = size_t(next_id);

    // Pass 1: area-weighted smooth normals per position, plus what masking and falloff need.
    std::vector<Vec3d>   smooth_nrm(unique_count, Vec3d::Zero());
    std::vector<double>  masked_area(unique_count, 0.0), total_area(unique_count, 0.0);
    const bool           have_weights = !geometry.exclude_weight.empty();
    std::vector<uint8_t> user_excluded_face(have_weights ? count / 3 : 0, 0);
    std::vector<uint8_t> excluded_pos(have_weights ? unique_count : 0, 0);

    for (size_t t = 0; t + 2 < count; t += 3) {
        const Vec3d a = geometry.pos[t].cast<double>();
        const Vec3d face_n = (geometry.pos[t + 1].cast<double>() - a).cross(geometry.pos[t + 2].cast<double>() - a);
        const double face_area = face_n.norm(); // twice the triangle area, so weighting is natural
        const double nz = face_area > 1e-12 ? face_n.z() / face_area : 0.0;
        const double face_angle = std::acos(std::min(1.0, std::abs(nz))) * (180.0 / M_PI);
        const bool   angle_masked =
            nz < 0.0 ? (settings.bottom_angle_limit > 0.f && face_angle <= settings.bottom_angle_limit)
                     : (settings.top_angle_limit > 0.f && face_angle <= settings.top_angle_limit);

        // Thresholded high, not at a half: merging by maximum leaves a face bordering an excluded one
        // with two corners at 1.0, averaging about 0.67, which a half threshold would misread.
        bool user_excluded = false;
        if (have_weights) {
            const float avg = (geometry.exclude_weight[t] + geometry.exclude_weight[t + 1] +
                               geometry.exclude_weight[t + 2]) / 3.f;
            user_excluded = avg > 0.99f;
            if (user_excluded)
                user_excluded_face[t / 3] = 1;
        }

        for (int v = 0; v < 3; ++v) {
            const size_t vid = size_t(vertex_id[t + size_t(v)]);
            if (user_excluded && have_weights)
                excluded_pos[vid] = 1;
            // Subdivision split vertices at sharp edges, so these are smooth across soft edges and
            // sharp across hard ones - no faceting on round surfaces, no rounding of corners.
            smooth_nrm[vid] += geometry.nrm[t + size_t(v)].cast<double>() * face_area;
            if (angle_masked)
                masked_area[vid] += face_area;
            total_area[vid] += face_area;
        }
    }

    // The pre-normalisation magnitude over the total area says how much the neighbouring faces agree:
    // near 1 they do, near 0 they cancelled, meaning a knife edge with no usable surface direction.
    std::vector<double> reliability(unique_count, 0.0);
    for (size_t id = 0; id < unique_count; ++id) {
        const double len = smooth_nrm[id].norm();
        reliability[id]  = (len > 0.0 && total_area[id] > 0.0) ? len / total_area[id] : 0.0;
        smooth_nrm[id]   = (len > 0.0) ? Vec3d(smooth_nrm[id] / len) : Vec3d(0.0, 0.0, 1.0);
    }

    // Pass 1.5: the smoothed blend normal - see the header for why it is separate.
    std::vector<Vec3d> blend_nrm = smooth_nrm;
    if (settings.blend_normal_smoothing > 0 && unique_count > 0) {
        // CSR adjacency over the welded graph, deliberately a multigraph: duplicates weight a pair by
        // how often it shares an edge, so a well-connected surface couples more strongly.
        std::vector<uint32_t> degree(unique_count, 0);
        const auto            add_degree = [&](int a, int b) {
            if (a != b) { ++degree[size_t(a)]; ++degree[size_t(b)]; }
        };
        for (size_t t = 0; t + 2 < count; t += 3) {
            const int a = vertex_id[t], b = vertex_id[t + 1], c = vertex_id[t + 2];
            add_degree(a, b); add_degree(b, c); add_degree(c, a);
        }
        std::vector<uint32_t> csr_start(unique_count + 1, 0);
        for (size_t id = 0; id < unique_count; ++id)
            csr_start[id + 1] = csr_start[id] + degree[id];
        std::vector<uint32_t> neighbors(csr_start[unique_count]);
        std::vector<uint32_t> cursor(unique_count, 0);
        const auto            add_edge = [&](int a, int b) {
            if (a == b)
                return;
            neighbors[csr_start[size_t(a)] + cursor[size_t(a)]++] = uint32_t(b);
            neighbors[csr_start[size_t(b)] + cursor[size_t(b)]++] = uint32_t(a);
        };
        for (size_t t = 0; t + 2 < count; t += 3) {
            const int a = vertex_id[t], b = vertex_id[t + 1], c = vertex_id[t + 2];
            add_edge(a, b); add_edge(b, c); add_edge(c, a);
        }

        std::vector<Vec3d> cur = smooth_nrm, nxt(unique_count, Vec3d::Zero());
        for (int iter = 0; iter < settings.blend_normal_smoothing; ++iter) {
            for (size_t id = 0; id < unique_count; ++id) {
                const uint32_t s = csr_start[id], e = csr_start[id + 1];
                if (e == s) {
                    nxt[id] = cur[id];
                    continue;
                }
                Vec3d sum = Vec3d::Zero();
                for (uint32_t k = s; k < e; ++k)
                    sum += cur[neighbors[k]];
                sum /= double(e - s);
                const double len = sum.norm();
                // Cancelling neighbours mean a knife edge; keep what we had.
                nxt[id] = (len > 1e-12) ? Vec3d(sum / len) : cur[id];
            }
            cur.swap(nxt);
        }
        blend_nrm = std::move(cur);
    }

    // A boundary position borders both masked and unmasked faces, or sits on the exclusion seam.
    // Every other position gets its distance to the nearest one, ramped to 1 at the falloff distance.
    std::vector<double> falloff;
    if (settings.boundary_falloff > 0.f && unique_count > 0) {
        std::vector<Vec3f> boundary;
        for (size_t id = 0; id < unique_count; ++id) {
            const double frac = total_area[id] > 0.0 ? masked_area[id] / total_area[id] : 0.0;
            const bool   on_excl = !excluded_pos.empty() && excluded_pos[id] != 0;
            if (on_excl || (frac > 0.0 && frac < 1.0))
                boundary.push_back(id_pos[id]);
        }
        falloff.assign(unique_count, 1.0);
        if (!boundary.empty()) {
            // A uniform grid: the query is nearest-point only, so a tree costs more than it saves.
            Vec3f lo = boundary.front(), hi = boundary.front();
            for (const Vec3f &p : boundary) {
                lo = lo.cwiseMin(p);
                hi = hi.cwiseMax(p);
            }
            const Vec3f  span = (hi - lo).cwiseMax(Vec3f(1e-6f, 1e-6f, 1e-6f));
            const int    res  = std::clamp(int(std::ceil(std::cbrt(double(boundary.size())) * 2.0)), 4, 128);
            const Vec3f  cell = span / float(res);
            const float  cell_min = cell.minCoeff();
            const auto cell_of = [&](const Vec3f &p) {
                Vec3i32 c;
                for (int k = 0; k < 3; ++k)
                    c[k] = std::clamp(int((p[k] - lo[k]) / span[k] * float(res)), 0, res - 1);
                return c;
            };
            const auto cell_index = [&](int x, int y, int z) {
                return size_t(z) * size_t(res) * size_t(res) + size_t(y) * size_t(res) + size_t(x);
            };
            std::vector<std::vector<int>> grid(size_t(res) * size_t(res) * size_t(res));
            for (size_t i = 0; i < boundary.size(); ++i) {
                const Vec3i32 c = cell_of(boundary[i]);
                grid[cell_index(c.x(), c.y(), c.z())].push_back(int(i));
            }

            const double radius = double(settings.boundary_falloff);
            for (size_t id = 0; id < unique_count; ++id) {
                const Vec3f &p = id_pos[id];
                const Vec3i32 c = cell_of(p);
                double       best = std::numeric_limits<double>::max();
                // Anything in shell r is at least (r - 1) cells away, so once the best found is within
                // that bound nothing closer can be hiding further out.
                for (int r = 0; r < res; ++r) {
                    for (int dz = -r; dz <= r; ++dz)
                        for (int dy = -r; dy <= r; ++dy)
                            for (int dx = -r; dx <= r; ++dx) {
                                // The shell only; its interior was covered by a smaller r.
                                if (r > 0 && std::abs(dx) != r && std::abs(dy) != r && std::abs(dz) != r)
                                    continue;
                                const int qx = c.x() + dx, qy = c.y() + dy, qz = c.z() + dz;
                                if (qx < 0 || qy < 0 || qz < 0 || qx >= res || qy >= res || qz >= res)
                                    continue;
                                for (const int bi : grid[cell_index(qx, qy, qz)])
                                    best = std::min(best, double((boundary[size_t(bi)] - p).norm()));
                            }
                    if (best <= double(r) * double(cell_min))
                        break;
                }
                falloff[id] = (best == std::numeric_limits<double>::max() || radius <= 0.0)
                                  ? 1.0
                                  : std::clamp(best / radius, 0.0, 1.0);
            }
        }
    }

    // Pass 2: one sample per unique position.
    std::vector<double>  grey(unique_count, 0.0);
    std::vector<uint8_t> grey_set(unique_count, 0);
    for (size_t i = 0; i < count; ++i) {
        const size_t vid = size_t(vertex_id[i]);
        if (grey_set[vid])
            continue;
        grey_set[vid] = 1;
        grey[vid] = double(sample(geometry.pos[i], smooth_nrm[vid].cast<float>(),
                                  blend_nrm[vid].cast<float>()));
    }

    // Pass 3: move every copy of a position by the identical vector.
    for (size_t i = 0; i < count; ++i) {
        const Vec3f &p   = geometry.pos[i];
        const size_t vid = size_t(vertex_id[i]);

        // Only angle masking uses the per-position blend, so an excluded face never dims its
        // neighbours through a shared vertex.
        const bool face_excluded = !user_excluded_face.empty() && user_excluded_face[i / 3] != 0;
        // Pinned where an included face shares a position with an excluded one, sealing the boundary.
        const bool sealed_boundary =
            !face_excluded && !excluded_pos.empty() && excluded_pos[vid] != 0;
        const double masked_frac = total_area[vid] > 0.0 ? masked_area[vid] / total_area[vid] : 0.0;
        const double centered    = settings.symmetric ? (grey[vid] - 0.5) : grey[vid];
        const double ramp        = falloff.empty() ? 1.0 : falloff[vid];
        const double disp = (face_excluded || sealed_boundary)
                                ? 0.0
                                : ramp * (1.0 - masked_frac) * centered * double(settings.amplitude);

        Vec3d moved = p.cast<double>() + smooth_nrm[vid] * disp;

        // Stop a partly masked vertex poking through the surface it borders.
        if (masked_frac > 0.0) {
            if (settings.bottom_angle_limit > 0.f && moved.z() < double(p.z())) moved.z() = double(p.z());
            if (settings.top_angle_limit > 0.f && moved.z() > double(p.z()))    moved.z() = double(p.z());
        }
        if (settings.no_downward_z && moved.z() < double(p.z()))
            moved.z() = double(p.z());
        // A vertex starting on the bottom plane stays there: otherwise a downward-facing face pulls
        // *up* where the sample is below mid-grey, leaving bed-contact vertices at differing heights.
        if (settings.no_downward_z && double(p.z()) <= double(bounds.min.z()) + 1e-5)
            moved.z() = double(p.z());

        out.pos[i] = moved.cast<float>();

        if (on_progress && (i % 5000) == 0 && !on_progress(double(i) / double(count)))
            return geometry; // cancelled: hand back the input untouched
    }

    // Per-face, not averaged across shared positions: averaging can flip an excluded face's normal
    // when its neighbours moved outward.
    for (size_t t = 0; t + 2 < count; t += 3) {
        const Vec3f n = (out.pos[t + 1] - out.pos[t]).cross(out.pos[t + 2] - out.pos[t]).normalized();
        out.nrm[t] = out.nrm[t + 1] = out.nrm[t + 2] = n;
    }
    out.exclude_weight = geometry.exclude_weight;
    return out;
}

} // namespace TextureBake
} // namespace Slic3r
