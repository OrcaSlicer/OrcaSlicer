// Hackish wrapper around the ClipperLib library to compile the Clipper library using Slic3r::Point.

#include "clipper.hpp"

// Don't include <clipper/clipper.hpp> for the second time.
#define clipper_hpp

// Override ClipperLib namespace to Slic3r::ClipperLib
#define CLIPPERLIB_NAMESPACE_PREFIX	Slic3r
// Override Slic3r::ClipperLib::IntPoint to Slic3r::Point
#define CLIPPERLIB_INTPOINT_TYPE    Slic3r::Point

#include <clipper/clipper.cpp>
