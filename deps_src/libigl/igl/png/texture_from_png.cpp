// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "texture_from_png.h"

#include "../opengl/report_gl_error.h"
#include <igl_stb_image.h>

IGL_INLINE bool igl::png::texture_from_png(const std::string png_file, const bool flip, GLuint & id)
{
  int width,height,n;
  unsigned char *data = igl::stbi_load(png_file.c_str(), &width, &height, &n, 4);
  if(data == NULL) {
    return false;
  }

  // Why do I need to flip?
  /*if(flip)
  {
    yimg.flip();
  }*/

  glGenTextures(1, &id);
  glBindTexture(GL_TEXTURE_2D, id);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexImage2D(
    GL_TEXTURE_2D, 0, GL_RGB,
    width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
  glBindTexture(GL_TEXTURE_2D, 0);

  igl::stbi_image_free(data);

  return true;
}

IGL_INLINE bool igl::png::texture_from_png(const std::string png_file, GLuint & id)
{
  return texture_from_png(png_file,false,id);
}


IGL_INLINE bool igl::png::texture_from_png(
  const std::string png_file,
  Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic>& R,
  Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic>& G,
  Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic>& B,
  Eigen::Matrix<unsigned char,Eigen::Dynamic,Eigen::Dynamic>& A
)
{
  int width,height,n;
  unsigned char *data = igl::stbi_load(png_file.c_str(), &width, &height, &n, 4);
  if(data == NULL) {
    return false;
  }

  R.resize(height,width);
  G.resize(height,width);
  B.resize(height,width);
  A.resize(height,width);

  for (unsigned j=0; j<height; ++j) {
    for (unsigned i=0; i<width; ++i) {
      // used to flip with libPNG, but I'm not sure if
      // simply j*width + i wouldn't be better
      // stb_image uses horizontal scanline an starts top-left corner
      R(i,j) = data[4*( (width-1-i) + width * (height-1-j) )];
      G(i,j) = data[4*( (width-1-i) + width * (height-1-j) ) + 1];
      B(i,j) = data[4*( (width-1-i) + width * (height-1-j) ) + 2];
      //A(i,j) = data[4*( (width-1-i) + width * (height-1-j) ) + 3];
    }
  }

  igl::stbi_image_free(data);

  return true;
}
