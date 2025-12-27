// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_TETGEN_READ_INTO_TETGENIO_H
#define IGL_COPYLEFT_TETGEN_READ_INTO_TETGENIO_H
#include "../../igl_inline.h"

#include <string>
#ifndef TETLIBRARY
#define TETLIBRARY 
#endif
#include "tetgen.h" // Defined tetgenio, REAL

namespace igl
{
  namespace copyleft
  {
    namespace tetgen
    {
      // Read a mesh or point set into tetgenio (input object for calling
      // tetgen).  Many file formats are already supported by tetgen:
      //   .off
      //   .ply
      //   .node
      //   .ply
      //   .medit
      //   .vtk
      //   etc.
      // Notably it does not support .obj which is loaded by hand here (also
      // demonstrating how to load points/faces programmatically)
      //
      // If the file extension is not recognized the filename is assumed to be
      // the basename of a collection describe a tetmesh, (of which at least
      // the .node file must exist):
      //   [filename].node
      //   [filename].ele
      //   [filename].face
      //   [filename].edge
      //   [filename].vol
      //
      // Inputs:
      //   path  path to file or basename to files
      // Outputs:
      //   in  tetgenio input object
      // Returns true on success, false on error
      IGL_INLINE bool read_into_tetgenio(
        const std::string & path, 
        tetgenio & in);
    }
  }
}


#ifndef IGL_STATIC_LIBRARY
#  include "read_into_tetgenio.cpp"
#endif

#endif
