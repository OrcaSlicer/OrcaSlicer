// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "render_to_file_async.h"
#include "../../stb/write_image.h"
#include "../gl.h"
#include <stb_image_write.h>

static IGL_INLINE bool render_to_file_async_helper(
  unsigned char * img, int width, int height,
  const std::string filename,
  const bool alpha)
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

  const bool ret = igl::stb::write_image(filename,width,height,img);
  delete [] img;
  return ret;
}

IGL_INLINE std::thread igl::opengl::stb::render_to_file_async(
  const std::string filename,
  const int width,
  const int height,
  const bool alpha)
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
  std::thread t(render_to_file_async_helper,data,width,height,filename,alpha);
  t.detach();
  return t;
}
