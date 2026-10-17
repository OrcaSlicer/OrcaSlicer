// The one definition of nanosvg and its rasterizer for every target that
// links libslic3r. Kept in its own file because on Windows nanosvg.h's
// implementation includes windows.h, whose global GDI names (Polyline,
// Polygon) are ambiguous with Slic3r's under `using namespace Slic3r`.
// Keep nanosvg out of the precompiled headers, or its include guard hides
// the implementation.
#define NANOSVG_IMPLEMENTATION
#include "nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg/nanosvgrast.h"
