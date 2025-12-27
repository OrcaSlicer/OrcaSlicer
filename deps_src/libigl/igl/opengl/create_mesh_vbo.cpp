// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "create_mesh_vbo.h"

#include "create_vector_vbo.h"
#include "create_index_vbo.h"

// http://www.songho.ca/opengl/gl_vbo.html#create
IGL_INLINE void igl::opengl::create_mesh_vbo(
  const Eigen::MatrixXd & V,
  const Eigen::MatrixXi & F,
  GLuint & V_vbo_id,
  GLuint & F_vbo_id)
{
  // Create VBO for vertex position vectors
  create_vector_vbo(V,V_vbo_id);
  // Create VBO for face index lists
  create_index_vbo(F,F_vbo_id);
}

// http://www.songho.ca/opengl/gl_vbo.html#create
IGL_INLINE void igl::opengl::create_mesh_vbo(
  const Eigen::MatrixXd & V,
  const Eigen::MatrixXi & F,
  const Eigen::MatrixXd & N,
  GLuint & V_vbo_id,
  GLuint & F_vbo_id,
  GLuint & N_vbo_id)
{
  // Create VBOs for faces and vertices
  create_mesh_vbo(V,F,V_vbo_id,F_vbo_id);
  // Create VBO for normal vectors
  create_vector_vbo(N,N_vbo_id);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
#endif
