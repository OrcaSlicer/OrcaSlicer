#pragma once

#include "PreciseSeam.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Polyline.hpp"
#include <cstddef>
#include <vector>

namespace Slic3r::PreciseSeam::detail {

// Binding intermediates retain source edge identity until segment assembly.
struct ClippedEdgeInterval {
    size_t edge;
    double begin;
    double end;
    Point first;
    Point last;
};

struct FragmentBindingFailure {
    size_t pair_index = 0;
    const char *reason = "empty fragment";
};

// Failure rolls back this fragment only; earlier bindings remain intact.
bool append_projected_fragment(const Polyline &fragment, const Polygon &perimeter,
                               std::vector<ClippedEdgeInterval> &intervals,
                               FragmentBindingFailure &failure);

// Exact path, then projection path, without the fallback; intervals are unchanged on failure.
// Exposed so tests can show that a fragment needs the fallback in append_fragment().
bool bind_fragment(const Polyline &fragment, const Polygon &perimeter,
                   std::vector<ClippedEdgeInterval> &intervals, FragmentBindingFailure &failure);

// Binds one fragment. After a failure it tries the rare-case repair and the contact rule, both logged
// as recoveries. Returns false when the fragment is discarded.
bool append_fragment(const Polyline &fragment, const Polygon &perimeter,
                     std::vector<ClippedEdgeInterval> &intervals,
                     const ExtractionContext &context, size_t fragment_index);

} // namespace Slic3r::PreciseSeam::detail
