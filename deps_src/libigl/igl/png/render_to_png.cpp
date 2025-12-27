// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "render_to_png.h"
#include <igl_stb_image.h>

#include "../opengl/gl.h"

IGL_INLINE bool igl::png::render_to_png(
  const std::string png_file,
  const int width,
  const int height,
  const bool alpha,
  const bool fast)
{
  unsigned char * data = new unsigned char[4*width*height];
  glReadPixels(
    0,
    0,
    width,
    height,
    GL_RGBA,
    GL_UNSIGNED_BYTE,
    data);
  //img->flip();
  if(!alpha)
  {
    for(int i = 0;i<width;i++)
    for(int j = 0;j<height;j++)
    {
      data[4*(i+j*width)+3] = 255;
    }
  }
  bool ret = igl::stbi_write_png(png_file.c_str(), width, height, 4, data, 4*width*sizeof(unsigned char));
  delete [] data;
  return ret;
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
#endif
