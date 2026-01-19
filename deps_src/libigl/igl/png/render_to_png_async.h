// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_PNG_RENDER_TO_PNG_ASYNC_H
#define IGL_PNG_RENDER_TO_PNG_ASYNC_H
#include <igl/igl_inline.h>
#include <thread>
//#include <boost/thread/thread.hpp>

#include <string>
namespace igl
{
  namespace png
  {
    // History:
    //  added multithreaded parameter and support, Alec Sept 3, 2012
    //
    // Render current open GL image to .png file
    // Inputs:
    //   png_file  path to output .png file
    //   width  width of scene and resulting image
    //   height height of scene and resulting image
    //   alpha  whether to include alpha channel
    //   fast  sacrifice compression ratio for speed
    // Returns true only if no errors occurred
    //
    // See also: igl/render_to_tga which is faster but writes .tga files
    IGL_INLINE std::thread render_to_png_async(
      const std::string png_file,
      const int width,
      const int height,
      const bool alpha = true,
      const bool fast = false);
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "render_to_png_async.cpp"
#endif

#endif
