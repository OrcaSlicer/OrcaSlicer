#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>
#include <type_traits>

namespace Slic3r { namespace Gpu {

// Emit only strict-interior intersections, in the CPU contour/edge/scanline
// order. Vertex, vertical-edge and tangency decisions stay in the product's
// existing CPU path. If false is returned, discard every emitted candidate.
// The disabled path deliberately does not inspect contours or allocate memory.
template<class Region, class Coord, class Emit>
bool visit_rectilinear_candidates(bool enabled, const Region& region, size_t n_vlines,
                                  Coord x0, Coord spacing, Emit&& emit)
{
    static_assert(std::is_integral<Coord>::value && std::is_signed<Coord>::value,
                  "scanline coordinates must be signed integers");
    constexpr size_t maximum_requests = 256 * 1024;
    constexpr Coord hi = std::numeric_limits<Coord>::max();
    constexpr Coord lo = std::numeric_limits<Coord>::min();
    if (!enabled || n_vlines == 0 || n_vlines > size_t(std::numeric_limits<int>::max()) || spacing <= 0)
        return false;
    // Prove all scanline positions representable before generating any work.
    if (n_vlines - 1 > size_t(hi / spacing))
        return false;
    const Coord last_offset = Coord(n_vlines - 1) * spacing;
    if (x0 > hi - last_offset)
        return false;

    size_t count = 0;
    for (size_t c = 0; c < region.n_contours; ++c) {
        const auto& contour = region.contour(c).points;
        if (contour.size() < 2)
            continue;
        for (size_t s = 0; s < contour.size(); ++s) {
            const auto& a = contour[(s == 0 ? contour.size() : s) - 1];
            const auto& b = contour[s];
            const Coord left = std::min(a.x(), b.x());
            const Coord right = std::max(a.x(), b.x());
            if (right < x0 || left > x0 + last_offset)
                continue;
            // Avoid the CPU routine's overflowing range arithmetic on inputs
            // outside the optional accelerator's representable domain.
            if ((x0 > 0 && left < lo + x0) || (x0 < 0 && right > hi + x0))
                return false;
            const Coord left_delta = left - x0;
            const Coord right_delta = right - x0;
            Coord first = left_delta / spacing;
            if (left_delta % spacing > 0)
                ++first;
            Coord last = right_delta / spacing;
            if (right_delta % spacing < 0)
                --last;
            first = std::max(Coord(0), first);
            last = std::min(Coord(n_vlines - 1), last);
            for (Coord i = first; i <= last; ++i) {
                const Coord x = x0 + i * spacing;
                if (a.x() == x || b.x() == x)
                    continue;
                if (count == maximum_requests)
                    return false;
                emit(a, b, x);
                ++count;
            }
        }
    }
    return true;
}

}} // namespace Slic3r::Gpu
