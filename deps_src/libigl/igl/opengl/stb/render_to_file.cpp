// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "render_to_file.h"
#include "../../stb/write_image.h"
#include <stb_image_write.h>

#include "../gl.h"

IGL_INLINE bool igl::opengl::stb::render_to_file(
  const std::string filename,
  const int width,
  const int height,
  const bool alpha)
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
  const bool ret = igl::stb::write_image(filename,width,height,data);
  delete [] data;
  return ret;
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
#endif
