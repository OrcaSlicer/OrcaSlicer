// Hackish wrapper around the ClipperLib library to compile the Clipper2 library with the Z support.

#ifndef clipper2_z_hpp
#ifdef CLIPPER_H
#error "You should include clipper2_z.hpp before clipper.h"
#endif

#define clipper2_z_hpp

// Enable the Z coordinate support.
#define USINGZ

#include "clipper.h"

#undef CLIPPER_H
#undef USINGZ
#endif // clipper2_z_hpp
