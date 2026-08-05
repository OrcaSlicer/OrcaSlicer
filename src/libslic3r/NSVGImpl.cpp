// Provides the single-header nanosvg implementation for the whole libslic3r
// library. libslic3r's SVG/emboss code (NSVGUtils.cpp, Format/svg.cpp) calls
// into nanosvg, but the implementation is only emitted when
// NANOSVG_IMPLEMENTATION is defined before the header is included. Keeping it
// in one dedicated TU inside libslic3r makes every consumer (GUI, tests,
// dev-utils) link without having to define the macro themselves.
//
// This file must stay out of the unity build: in a merged translation unit a
// sibling file may include nanosvg.h first (via EmbossShape.hpp / NSVGUtils.hpp),
// whose include guard would then suppress the implementation.
#define NANOSVG_IMPLEMENTATION
#include "nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg/nanosvgrast.h"
