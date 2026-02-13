#include "libslic3r/libslic3r.h"
#define NANOSVG_IMPLEMENTATION
#include "nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg/nanosvgrast.h"

// Without the above, you get some errors like these when linking

// usr/bin/ld: src/libslic3r/RelWithDebInfo/liblibslic3r.a(svg.cpp.o): in function `Slic3r::get_svg_profile(char const*, std::vector<Slic3r::Element_Info, std::allocator<Slic3r::Element_Info> >&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&)':
// /opt/prg/OrcaSlicer/src/libslic3r/Format/svg.cpp:128:(.text+0x114e): undefined reference to `nsvgParseFromFile'
// /usr/bin/ld: /opt/prg/OrcaSlicer/src/libslic3r/Format/svg.cpp:299:(.text+0x34d6): undefined reference to `nsvgDelete'
