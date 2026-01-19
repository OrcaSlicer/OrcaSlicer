// Hackish wrapper around the ClipperLib library to compile the Clipper library with the Z support.

#ifndef clipper_z_hpp
#ifdef clipper_hpp
#error "You should include clipper_z.hpp before clipper.hpp"
#endif

#define clipper_z_hpp

// Enable the Z coordinate support.
#define CLIPPERLIB_USE_XYZ

#include "clipper.hpp"

#undef clipper_hpp
#undef CLIPPERLIB_USE_XYZ

#endif // clipper_z_hpp
