// libslic3r leaves nanosvg's definitions to each executable, and every executable that slices links
// this library.
#define NANOSVG_IMPLEMENTATION
#include "nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg/nanosvgrast.h"
