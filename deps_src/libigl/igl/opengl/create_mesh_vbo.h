// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_OPENGL_CREATE_MESH_VBO_H
#define IGL_OPENGL_CREATE_MESH_VBO_H
#include "../igl_inline.h"
#include "gl.h"
#include <Eigen/Core>

namespace igl
{
  namespace opengl
  {
  
    /// Create a VBO (Vertex Buffer Object) for a mesh. Actually two VBOs: one 
    /// GL_ARRAY_BUFFER for the vertex positions (V) and one
    /// GL_ELEMENT_ARRAY_BUFFER for the triangle indices (F)
    ///
    /// @param[in] V  #V by 3 eigen Matrix of mesh vertex 3D positions
    /// @param[in] F  #F by 3 eigen Matrix of face (triangle) indices
    /// @param[out] V_vbo_id  buffer id for vertex positions
    /// @param[out] F_vbo_id  buffer id for face indices
    ///
    /// \note when using glDrawElements VBOs for V and F using MatrixXd and
    /// MatrixXi will have types GL_DOUBLE and GL_UNSIGNED_INT respectively
    ///
    IGL_INLINE void create_mesh_vbo(
      const Eigen::MatrixXd & V,
      const Eigen::MatrixXi & F,
      GLuint & V_vbo_id,
      GLuint & F_vbo_id);
    /// \overload
    ///
    /// @param[in] N  #V by 3 eigen Matrix of mesh vertex 3D normals
    /// @param[out] N_vbo_id  buffer id for vertex positions
    IGL_INLINE void create_mesh_vbo(
      const Eigen::MatrixXd & V,
      const Eigen::MatrixXi & F,
      const Eigen::MatrixXd & N,
      GLuint & V_vbo_id,
      GLuint & F_vbo_id,
      GLuint & N_vbo_id);
  }

}

#ifndef IGL_STATIC_LIBRARY
#  include "create_mesh_vbo.cpp"
#endif

#endif
