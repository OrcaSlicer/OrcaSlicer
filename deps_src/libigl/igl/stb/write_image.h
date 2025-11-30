// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2016 Daniele Panozzo <daniele.panozzo@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_STB_WRITE_IMAGE_H
#define IGL_STB_WRITE_IMAGE_H
#include "../igl_inline.h"
#include <Eigen/Core>
#include <string>

namespace igl
{
  namespace stb
  {
    /// Writes an image to a file
    /// 
    /// Supported file formats (based on STB):
    ///  
    ///    JPEG 
    ///    PNG 
    ///    TGA 
    ///    BMP 
    /// 
    /// @param[in] R  red channel
    /// @param[in] G  green channel
    /// @param[in] B  blue channel
    /// @param[in] A  alpha channel
    /// @param[in] image_file  path to image file
    /// @param[in] quality  (only for jpg file) jpeg quality
    /// @returns true on success, false on errors
    ///
    IGL_INLINE bool write_image
    (
      const std::string image_file,
      const Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic>& R,
      const Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic>& G,
      const Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic>& B,
      const Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic>& A,
      const int quality=90
    );
    /// Raw image writer.
    ///
    /// @param[in] image_file  path to image file
    /// @param[in] width  image width
    /// @param[in] height  image height
    /// @param[in] rgba_data  raw image data
    /// @param[in] quality  (only for jpg file) jpeg quality
    /// @returns true on success, false on errors
    ///
    IGL_INLINE bool write_image(
      const std::string image_file,
      const int width,
      const int height,
      const unsigned char * rgba_data,
      const int quality = 90);

  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "write_image.cpp"
#endif

#endif
