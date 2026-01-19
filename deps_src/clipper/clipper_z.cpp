// Hackish wrapper around the ClipperLib library to compile the Clipper library with the Z support.

// Enable the Z coordinate support.
#define CLIPPERLIB_USE_XYZ

// and let it compile
#include "clipper.cpp"
