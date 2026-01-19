// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2016 Daniele Panozzo <daniele.panozzo@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_PNG_READ_PNG_H
#define IGL_PNG_READ_PNG_H
#include "../igl_inline.h"
#include <Eigen/Core>
#include <string>

namespace igl
{
  namespace png
  {
    // Read an image from a .png file into 4 memory buffers
    //
    // Input:
    //  png_file  path to .png file
    // Output:
    //  R,G,B,A texture channels
    // Returns true on success, false on failure
    //
    IGL_INLINE bool readPNG(const std::string png_file,
    Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic>& R,
    Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic>& G,
    Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic>& B,
    Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic>& A
    );
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "readPNG.cpp"
#endif

#endif
