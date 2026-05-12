// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_OPENGL_CREATE_VECTOR_VBO_H
#define IGL_OPENGL_CREATE_VECTOR_VBO_H
#include "../igl_inline.h"
#include "gl.h"
#include <Eigen/Core>

namespace igl
{
  namespace opengl
  {
    /// Create a VBO (Vertex Buffer Object) for a list of vectors:
    /// GL_ARRAY_BUFFER for the vectors (V)
    ///
    /// @tparam T  should be a eigen matrix primitive type like int or double
    /// @param[in] V  m by n eigen Matrix of type T values
    /// @param[out] V_vbo_id  buffer id for vectors
    ///
    template <typename T>
    IGL_INLINE void create_vector_vbo(
      const Eigen::Matrix<T,Eigen::Dynamic,Eigen::Dynamic> & V,
      GLuint & V_vbo_id);
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "create_vector_vbo.cpp"
#endif

#endif
