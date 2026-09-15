#include "TextureBakeRelocate.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

namespace Slic3r {
namespace TextureBake {

namespace {

// Any two unit vectors orthogonal to n. Which two does not matter - the gradient is expressed in this
// basis and converted straight back, so the result is basis independent.
void tangent_basis(const Vec3f &n, Vec3f &t1, Vec3f &t2)
{
    const Vec3f a = (std::abs(n.x()) < 0.9f) ? Vec3f(1.f, 0.f, 0.f) : Vec3f(0.f, 1.f, 0.f);
    t1 = n.cross(a).normalized();
    t2 = n.cross(t1).normalized();
}

} // namespace

RelocateResult relocate_to_contours(const TriSoup &geometry, const HeightSampleFn &sample,
                                    const RelocateSettings &settings, const std::vector<uint8_t> &locked)
{
    RelocateResult result;
    result.geometry     = geometry;
    const size_t count  = geometry.pos.size();
    const size_t tri_ct = count / 3;
    if (count == 0 || !sample || settings.iterations <= 0)
        return result;

    // Weld, so every copy of a position moves together and the mesh cannot come apart.
    QuantizedPointMap  weld(WELD_GRID_GEOMETRY, std::min(count, size_t(1) << 22));
    std::vector<int>   vid(count);
    std::vector<Vec3f> pos;
    for (size_t i = 0; i < count; ++i) {
        vid[i] = weld.get_or_set(geometry.pos[i], int(pos.size()));
        if (weld.inserted())
            pos.push_back(geometry.pos[i]);
    }
    const size_t nv = pos.size();

    // Incident corners per position, CSR style, plus the mean incident edge length that sets the scale
    // for both the finite difference and the move limit.
    std::vector<uint32_t> start(nv + 1, 0);
    for (size_t i = 0; i < count; ++i)
        ++start[size_t(vid[i]) + 1];
    for (size_t v = 0; v < nv; ++v)
        start[v + 1] += start[v];
    std::vector<uint32_t> inc(count), cursor(nv, 0);
    for (size_t i = 0; i < count; ++i)
        inc[start[size_t(vid[i])] + cursor[size_t(vid[i])]++] = uint32_t(i);

    std::vector<uint8_t> frozen(nv, 0);
    if (!locked.empty())
        for (size_t t = 0; t < tri_ct && t < locked.size(); ++t)
            if (locked[t])
                for (int k = 0; k < 3; ++k)
                    frozen[size_t(vid[t * 3 + size_t(k)])] = 1;

    std::vector<float> edge_len(nv, 0.f), normal_len(nv, 0.f);
    std::vector<Vec3f> nrm(nv, Vec3f::Zero());
    const auto rebuild_frames = [&]() {
        std::fill(nrm.begin(), nrm.end(), Vec3f::Zero());
        std::fill(edge_len.begin(), edge_len.end(), 0.f);
        std::vector<uint32_t> deg(nv, 0);
        for (size_t t = 0; t < tri_ct; ++t) {
            const int a = vid[t * 3], b = vid[t * 3 + 1], c = vid[t * 3 + 2];
            const Vec3f fn = (pos[size_t(b)] - pos[size_t(a)]).cross(pos[size_t(c)] - pos[size_t(a)]);
            for (int k = 0; k < 3; ++k) {
                const int u = vid[t * 3 + size_t(k)], w = vid[t * 3 + size_t((k + 1) % 3)];
                nrm[size_t(u)] += fn;
                edge_len[size_t(u)] += (pos[size_t(w)] - pos[size_t(u)]).norm();
                ++deg[size_t(u)];
            }
        }
        for (size_t v = 0; v < nv; ++v) {
            const float l = nrm[v].norm();
            nrm[v]      = (l > 0.f) ? Vec3f(nrm[v] / l) : Vec3f(0.f, 0.f, 1.f);
            edge_len[v] = deg[v] > 0 ? edge_len[v] / float(deg[v]) : 0.f;
        }
    };
    rebuild_frames();

    // The level to snap onto, taken from the height actually present on this patch rather than assumed.
    // A texture that never reaches full black or white would otherwise be measured against a range it
    // does not occupy.
    double h_lo = std::numeric_limits<double>::max(), h_hi = -h_lo;
    {
        std::vector<float> h0(nv, 0.f);
        tbb::parallel_for(tbb::blocked_range<size_t>(0, nv), [&](const tbb::blocked_range<size_t> &r) {
            for (size_t v = r.begin(); v < r.end(); ++v)
                h0[v] = sample(pos[v], nrm[v], nrm[v]);
        });
        for (const float h : h0) {
            h_lo = std::min(h_lo, double(h));
            h_hi = std::max(h_hi, double(h));
        }
    }
    const double h_range = h_hi - h_lo;
    if (!(h_range > 0.0))
        return result; // a flat height field has no contour to snap to
    const double target   = h_lo + h_range * settings.contour_level;
    // A gradient is worth acting on when the height changes by this much across one edge length.
    const double min_grad = h_range * settings.min_gradient_fraction;

    std::vector<uint8_t> ever_moved(nv, 0);

    for (int iter = 0; iter < settings.iterations; ++iter) {
        std::vector<Vec3f> proposal(nv);
        std::vector<uint8_t> want(nv, 0);

        tbb::parallel_for(tbb::blocked_range<size_t>(0, nv), [&](const tbb::blocked_range<size_t> &r) {
            for (size_t v = r.begin(); v < r.end(); ++v) {
                if (frozen[v] || edge_len[v] <= 0.f)
                    continue;
                const Vec3f n = nrm[v];
                Vec3f       t1, t2;
                tangent_basis(n, t1, t2);
                const float eps = edge_len[v] * float(settings.gradient_step_fraction);
                if (eps <= 0.f)
                    continue;

                // Central differences in the tangent plane. Sampling the field itself, not the mesh,
                // so the gradient is the image's, at whatever resolution the mesh happens to have.
                const double h  = double(sample(pos[v], n, n));
                const double gx = (double(sample(pos[v] + t1 * eps, n, n)) -
                                   double(sample(pos[v] - t1 * eps, n, n))) / (2.0 * double(eps));
                const double gy = (double(sample(pos[v] + t2 * eps, n, n)) -
                                   double(sample(pos[v] - t2 * eps, n, n))) / (2.0 * double(eps));
                const double g2 = gx * gx + gy * gy;
                if (g2 <= 0.0)
                    continue;
                // Scale-free test: how much the height changes across one edge, versus the patch range.
                if (std::sqrt(g2) * double(edge_len[v]) < min_grad)
                    continue;

                // Newton step onto the level set h = target, expressed back in 3D.
                const double s   = -(h - target) / g2;
                Vec3f        d   = t1 * float(s * gx) + t2 * float(s * gy);
                const float  cap = edge_len[v] * float(settings.max_move_fraction);
                const float  len = d.norm();
                if (len <= 0.f)
                    continue;
                if (len > cap)
                    d *= cap / len;
                proposal[v] = pos[v] + d;
                want[v]     = 1;
            }
        });

        // Apply one at a time: a move is only valid against the neighbourhood as it stands, and two
        // adjacent vertices moving together can invert a triangle neither would have on its own.
        size_t applied = 0;
        for (size_t v = 0; v < nv; ++v) {
            if (!want[v])
                continue;
            const Vec3f old = pos[v];
            pos[v]          = proposal[v];
            bool ok = true;
            for (uint32_t k = start[v]; k < start[v + 1] && ok; ++k) {
                const size_t t = size_t(inc[k]) / 3;
                const Vec3f &a = pos[size_t(vid[t * 3])];
                const Vec3f  n2 = (pos[size_t(vid[t * 3 + 1])] - a).cross(pos[size_t(vid[t * 3 + 2])] - a);
                // Compared against the frame this vertex carried before the move: a triangle that
                // flips or collapses means the move crossed a neighbour.
                if (n2.squaredNorm() <= 0.f || n2.normalized().dot(nrm[v]) < 0.f)
                    ok = false;
            }
            if (ok) {
                ++applied;
                ever_moved[v] = 1;
            } else {
                pos[v] = old;
                ++result.rejected;
            }
        }
        if (applied == 0)
            break;
        rebuild_frames();
    }

    for (size_t v = 0; v < nv; ++v)
        if (ever_moved[v])
            ++result.moved;

    // Write the relocated positions back to every copy, and rebuild the per-face normals.
    for (size_t i = 0; i < count; ++i)
        result.geometry.pos[i] = pos[size_t(vid[i])];
    for (size_t t = 0; t < tri_ct; ++t) {
        Vec3f       n   = (result.geometry.pos[t * 3 + 1] - result.geometry.pos[t * 3])
                        .cross(result.geometry.pos[t * 3 + 2] - result.geometry.pos[t * 3]);
        const float len = n.norm();
        n = (len > 0.f) ? Vec3f(n / len) : Vec3f(0.f, 0.f, 1.f);
        result.geometry.nrm[t * 3] = result.geometry.nrm[t * 3 + 1] = result.geometry.nrm[t * 3 + 2] = n;
    }
    return result;
}

} // namespace TextureBake
} // namespace Slic3r
