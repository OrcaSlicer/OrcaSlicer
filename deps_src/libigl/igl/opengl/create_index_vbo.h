// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_OPENGL_CREATE_INDEX_VBO_H
#define IGL_OPENGL_CREATE_INDEX_VBO_H
#include "../igl_inline.h"
#include "gl.h"
#include <Eigen/Core>

namespace igl
{
  namespace opengl
  {
    /// Create a VBO (Vertex Buffer Object) for a list of indices:
    /// GL_ELEMENT_ARRAY_BUFFER_ARB for the triangle indices (F)
    ///
    /// @param[in] F  #F by 3 eigen Matrix of face (triangle) indices
    /// @param[out] F_vbo_id  buffer id for face indices
    ///
    IGL_INLINE void create_index_vbo(
      const Eigen::MatrixXi & F,
      GLuint & F_vbo_id);
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "create_index_vbo.cpp"
#endif

#endif
