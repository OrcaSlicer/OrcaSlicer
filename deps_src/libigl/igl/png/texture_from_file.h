// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_PNG_TEXTURE_FROM_FILE_H
#define IGL_PNG_TEXTURE_FROM_FILE_H
#include "../igl_inline.h"
#include "../opengl/gl.h"

#include <string>

namespace igl
{
  namespace png
  {
    // Read an image from an image file and use it as a texture. Officially,
    // only <del>.tga and</del> .png are supported. Any filetype read by
    // ImageMagick's `convert` will work via an unsafe system call.
    //
    // Input:
    //  filename  path to image file
    // Output:
    //  id  of generated openGL texture
    // Returns true on success, false on failure
    IGL_INLINE bool texture_from_file(const std::string filename, GLuint & id);
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "texture_from_file.cpp"
#endif

#endif



