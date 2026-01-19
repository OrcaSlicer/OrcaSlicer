// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "render_to_png_async.h"
#include "../opengl/gl.h"
#include <igl_stb_image.h>

static IGL_INLINE bool render_to_png_async_helper(
  unsigned char * img, int width, int height,
  const std::string png_file,
  const bool alpha,
  const bool fast)
{
  //img->flip();
  if(!alpha)
  {
    for(int i = 0;i<width;i++)
    for(int j = 0;j<height;j++)
    {
      img[4*(i+j*width)+3] = 255;
    }
  }

  bool ret = igl::stbi_write_png(png_file.c_str(), width, height, 4, img, width*sizeof(unsigned char));
  delete [] img;
  return ret;
}

IGL_INLINE std::thread igl::png::render_to_png_async(
  const std::string png_file,
  const int width,
  const int height,
  const bool alpha,
  const bool fast)
{
  // Part that should serial
  unsigned char * data = new unsigned char[width*height];
  glReadPixels(
    0,
    0,
    width,
    height,
    GL_RGBA,
    GL_UNSIGNED_BYTE,
    data);
  // Part that should be asynchronous
  std::thread t(render_to_png_async_helper,data,width,height,png_file,alpha,fast);
  t.detach();
  return t;
}
