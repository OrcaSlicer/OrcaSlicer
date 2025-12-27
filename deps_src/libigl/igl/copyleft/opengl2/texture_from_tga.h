// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_OPENGL_TEXTURE_FROM_TGA_H
#define IGL_OPENGL_TEXTURE_FROM_TGA_H
#include "../../igl_inline.h"
#include "../../opengl2/gl.h"
#include <string>

namespace igl
{
  namespace opengl
  {
    // Read an image from a .tga file and use it as a texture
    //
    // Input:
    //  tga_file  path to .tga file
    // Output:
    //  id  of generated openGL texture
    // Returns true on success, false on failure
    IGL_INLINE bool texture_from_tga(const std::string tga_file, GLuint & id);
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "texture_from_tga.cpp"
#endif

#endif
