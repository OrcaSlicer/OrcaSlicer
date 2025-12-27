// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_OPENGL_RENDER_TO_TGA_H
#define IGL_OPENGL_RENDER_TO_TGA_H
#include "../../igl_inline.h"
#include <string>

namespace igl
{
  namespace opengl
  {
    // Render current open GL image to .tga file
    // Inputs:
    //   tga_file  path to output .tga file
    //   width  width of scene and resulting image
    //   height height of scene and resulting image
    ///  alpha  whether to include alpha channel
    // Returns true only if no errors occurred
    //
    // See also: png/render_to_png which is slower but writes .png files
    IGL_INLINE bool render_to_tga(
      const std::string tga_file,
      const int width,
      const int height,
      const bool alpha);
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "render_to_tga.cpp"
#endif

#endif
