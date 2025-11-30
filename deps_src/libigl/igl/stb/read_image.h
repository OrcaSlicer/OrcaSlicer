// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2016 Daniele Panozzo <daniele.panozzo@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_STB_READ_IMAGE_H
#define IGL_STB_READ_IMAGE_H
#include "../igl_inline.h"
#include <Eigen/Core>
#include <string>

namespace igl
{
  namespace stb
  {
    // Read an image from a file into 4 memory buffers
    // 
    // Supported file formats (based on STB):
    //  
    //    JPEG baseline & progressive (12 bpc/arithmetic not supported, same as stock IJG lib)
    //    PNG 1/2/4/8/16-bit-per-channel
    //    TGA (not sure what subset, if a subset)
    //    BMP non-1bpp, non-RLE
    //    PSD (composited view only, no extra channels, 8/16 bit-per-channel)

    //    GIF (*comp always reports as 4-channel)
    //    HDR (radiance rgbE format)
    //    PIC (Softimage PIC)
    //    PNM (PPM and PGM binary only)
    //
    // Input:
    //  image_file  path to .png file
    // Output:
    //  R,G,B,A texture channels
    // Returns true on success, false on failure
    //
    IGL_INLINE bool read_image(const std::string image_file,
    Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic>& R,
    Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic>& G,
    Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic>& B,
    Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic>& A
    );
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "read_image.cpp"
#endif

#endif
