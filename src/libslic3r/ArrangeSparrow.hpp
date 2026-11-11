#ifndef SLIC3R_ARRANGE_SPARROW_HPP
#define SLIC3R_ARRANGE_SPARROW_HPP

#include "Arrange.hpp"

namespace Slic3r {

class BoundingBox;

namespace arrangement {

/// Arrange with the sparrow (Rust) packing backend instead of libnest2d.
/// Only rectangular beds are supported, which is every Bambu printer.
/// Writes translation/rotation/bed_idx back into \p arrangables exactly like
/// arrange() does. Returns false without modifying inputs when unsupported
/// material constraints, invalid input, an FFI error, or an incomplete result
/// require a stock arrange retry. Cancellation never retries an incomplete result.
bool arrange_sparrow(ArrangePolygons &      arrangables,
                     const ArrangePolygons &excludes,
                     const BoundingBox &    bed,
                     const ArrangeParams &  params);

}} // namespace Slic3r::arrangement

#endif // SLIC3R_ARRANGE_SPARROW_HPP
