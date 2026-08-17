// Provides the single-header nanosvg implementation for the whole libslic3r
// library. libslic3r's SVG/emboss code (NSVGUtils.cpp, Format/svg.cpp) calls
// into nanosvg, but the implementation is only emitted when
// NANOSVG_IMPLEMENTATION is defined before the header is included. Keeping it
// in one dedicated TU inside libslic3r makes every consumer (GUI, tests,
// dev-utils) link without having to define the macro themselves.
//
// This file is excluded from the unity build (see CMakeLists.txt) so it is
// always its own translation unit. That guarantees the nanosvg public
// functions (which are external symbols) are emitted exactly once across the
// whole library. If this file were part of a unity batch, a sibling file could
// include nanosvg.h first and either suppress this implementation (via the
// include guard) or, if the macro were defined target-wide, cause duplicate
// symbols at link time.
#define NANOSVG_IMPLEMENTATION
#include "nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg/nanosvgrast.h"
