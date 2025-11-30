// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_OPENGL_STB_RENDER_TO_PNG_ASYNC_H
#define IGL_OPENGL_STB_RENDER_TO_PNG_ASYNC_H
#include "../../igl_inline.h"
#include <thread>
//#include <boost/thread/thread.hpp>

#include <string>
namespace igl
{
  namespace opengl
  {
    namespace stb
    {
      // History:
      //  added multithreaded parameter and support, Alec Sept 3, 2012
      //
      /// Render current open GL image to file using a separate thread
      ///
      /// @param[in] filename  path to output image file
      /// @param[in] width  width of scene and resulting image
      /// @param[in] height height of scene and resulting image
      /// @param[in] alpha  whether to include alpha channel
      /// @return true only if no errors occurred
      ///
      /// \see render_to_file
      IGL_INLINE std::thread render_to_file_async(
        const std::string filename,
        const int width,
        const int height,
        const bool alpha = true);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "render_to_file_async.cpp"
#endif

#endif
