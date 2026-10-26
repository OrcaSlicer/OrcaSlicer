#include "DistanceTransform.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <limits>
#include <vector>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include "Point.hpp"
#include "libslic3r.h"

namespace Slic3r {

void squared_distance_transform_line(const float *f, float *d, int n, int *v, double *s)
{
    constexpr float  InfF = std::numeric_limits<float>::infinity();
    constexpr double InfD = std::numeric_limits<double>::infinity();
    int k = -1;
    for (int q = 0; q < n; ++q) {
        if (f[q] == InfF)
            continue;
        double x = -InfD;
        while (k >= 0) {
            x = (f[q] + double(q) * q - f[v[k]] - double(v[k]) * v[k]) / (2. * (q - v[k]));
            if (x > s[k])
                break;
            --k;
        }
        if (k < 0)
            x = -InfD;
        v[++k]   = q;
        s[k]     = x;
        s[k + 1] = InfD;
    }
    if (k < 0) {
        std::fill(d, d + n, InfF);
        return;
    }
    for (int q = 0, j = 0; q < n; ++q) {
        while (s[j + 1] < q)
            ++j;
        d[q] = float(sqr(double(q - v[j])) + f[v[j]]);
    }
}

void squared_distance_transform_axis(std::vector<float> &grid, const Vec3i32 &size, int axis, const std::function<void()> &throw_if_canceled)
{
    const int    n      = size[axis];
    const int    a1     = (axis + 1) % 3;
    const int    a2     = (axis + 2) % 3;
    const size_t stride = axis == 0 ? 1 : axis == 1 ? size_t(size.x()) : size_t(size.x()) * size.y();
    tbb::parallel_for(tbb::blocked_range<size_t>(0, size_t(size[a1]) * size[a2]), [&](const tbb::blocked_range<size_t> &range) {
        std::vector<float>  f(n), d(n);
        std::vector<int>    v(n);
        std::vector<double> s(n + 1);
        for (size_t line = range.begin(); line < range.end(); ++line) {
            Vec3i32 idx;
            idx[axis]          = 0;
            idx[a1]            = int(line % size[a1]);
            idx[a2]            = int(line / size[a1]);
            const size_t first = (size_t(idx.z()) * size.y() + idx.y()) * size.x() + idx.x();
            for (int i = 0; i < n; ++i)
                f[i] = grid[first + i * stride];
            squared_distance_transform_line(f.data(), d.data(), n, v.data(), s.data());
            for (int i = 0; i < n; ++i)
                grid[first + i * stride] = d[i];
        }
        throw_if_canceled();
    });
}

} // namespace Slic3r
