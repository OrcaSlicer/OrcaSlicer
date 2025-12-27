// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "create_index_vbo.h"

// http://www.songho.ca/opengl/gl_vbo.html#create
IGL_INLINE void igl::opengl::create_index_vbo(
  const Eigen::MatrixXi & F,
  GLuint & F_vbo_id)
{
  // Generate Buffers
  glGenBuffers(1,&F_vbo_id);
  // Bind Buffers
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,F_vbo_id);
  // Copy data to buffers
  // We expect a matrix with each vertex position on a row, we then want to
  // pass this data to OpenGL reading across rows (row-major)
  if(F.Options & Eigen::RowMajor)
  {
    glBufferData(
      GL_ELEMENT_ARRAY_BUFFER,
      sizeof(int)*F.size(),
      F.data(),
      GL_STATIC_DRAW);
  }else
  {
    // Create temporary copy of transpose
    Eigen::MatrixXi FT = F.transpose();
    // If its column major then we need to temporarily store a transpose
    glBufferData(
      GL_ELEMENT_ARRAY_BUFFER,
      sizeof(int)*F.size(),
      FT.data(),
      GL_STATIC_DRAW);
  }
  // bind with 0, so, switch back to normal pointer operation
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
#endif
