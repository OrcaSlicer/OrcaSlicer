// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2016 Daniele Panozzo <daniele.panozzo@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_PNG_WRITE_PNG_H
#define IGL_PNG_WRITE_PNG_H
#include "../igl_inline.h"
#include <Eigen/Core>
#include <string>

namespace igl
{
  namespace png
  {
    // Writes an image to a png file
    //
    // Input:
    //  R,G,B,A texture channels
    // Output:
    //  png_file  path to .png file
    // Returns true on success, false on failure
    //
    IGL_INLINE bool writePNG
    (
      const Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic>& R,
      const Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic>& G,
      const Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic>& B,
      const Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic>& A,
      const std::string png_file
    );
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "writePNG.cpp"
#endif

#endif
