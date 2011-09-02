// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_OPENGL2_DRAW_MESH_H
#define IGL_OPENGL2_DRAW_MESH_H
#include "../igl_inline.h"
#include "gl.h"
#include <Eigen/Dense>


namespace igl
{
  namespace opengl2
  {

    // Draw ../opengl/OpenGL_ commands needed to display a mesh with normals
    //
    // Inputs:
    //   V  #V by 3 eigen Matrix of mesh vertex 3D positions
    //   F  #F by 3|4 eigen Matrix of face (triangle/quad) indices
    //   N  #V|#F by 3 eigen Matrix of 3D normals
    IGL_INLINE void draw_mesh(
      const Eigen::MatrixXd & V,
      const Eigen::MatrixXi & F,
      const Eigen::MatrixXd & N);
    
    // Draw ../opengl/OpenGL_ commands needed to display a mesh with normals and per-vertex
    // colors
    //
    // Inputs:
    //   V  #V by 3 eigen Matrix of mesh vertex 3D positions
    //   F  #F by 3|4 eigen Matrix of face (triangle/quad) indices
    //   N  #V|#F by 3 eigen Matrix of 3D normals
    //   C  #V|#F|1 by 3 eigen Matrix of RGB colors
    IGL_INLINE void draw_mesh(
      const Eigen::MatrixXd & V,
      const Eigen::MatrixXi & F,
      const Eigen::MatrixXd & N,
      const Eigen::MatrixXd & C);
    // Inputs:
    //   V  #V by 3 eigen Matrix of mesh vertex 3D positions
    //   F  #F by 3|4 eigen Matrix of face (triangle/quad) indices
    //   N  #V|#F by 3 eigen Matrix of 3D normals
    //   C  #V|#F|1 by 3 eigen Matrix of RGB colors
    //   TC  #V|#F|1 by 3 eigen Matrix of Texture Coordinates
    IGL_INLINE void draw_mesh(
      const Eigen::MatrixXd & V,
      const Eigen::MatrixXi & F,
      const Eigen::MatrixXd & N,
      const Eigen::MatrixXd & C,
      const Eigen::MatrixXd & TC);
    
    // Draw ../opengl/OpenGL_ commands needed to display a mesh with normals, per-vertex
    // colors and LBS weights
    //
    // Inputs:
    //   V  #V by 3 eigen Matrix of mesh vertex 3D positions
    //   F  #F by 3|4 eigen Matrix of face (triangle/quad) indices
    //   N  #V by 3 eigen Matrix of mesh vertex 3D normals
    //   C  #V by 3 eigen Matrix of mesh vertex RGB colors
    //   TC  #V by 3 eigen Matrix of mesh vertex UC coorindates between 0 and 1
    //   W  #V by #H eigen Matrix of per mesh vertex, per handle weights
    //   W_index  Specifies the index of the "weight" vertex attribute: see
    //     glBindAttribLocation, if W_index is 0 then weights are ignored
    //   WI  #V by #H eigen Matrix of per mesh vertex, per handle weight ids
    //   WI_index  Specifies the index of the "weight" vertex attribute: see
    //     glBindAttribLocation, if WI_index is 0 then weight indices are ignored
    IGL_INLINE void draw_mesh(
      const Eigen::MatrixXd & V,
      const Eigen::MatrixXi & F,
      const Eigen::MatrixXd & N,
      const Eigen::MatrixXd & C,
      const Eigen::MatrixXd & TC,
      const Eigen::MatrixXd & W,
      const GLuint W_index,
      const Eigen::MatrixXi & WI,
      const GLuint WI_index);
    
    // Draw ../opengl/OpenGL_ commands needed to display a mesh with normals, per-vertex
    // colors and LBS weights
    //
    // Inputs:
    //   V  #V by 3 eigen Matrix of mesh vertex 3D positions
    //   F  #F by 3|4 eigen Matrix of face (triangle/quad) indices
    //   N  #V by 3 eigen Matrix of mesh vertex 3D normals
    //   NF  #F by 3 eigen Matrix of face (triangle/quad) normal indices, <0
    //     means no normal
    //   C  #V by 3 eigen Matrix of mesh vertex RGB colors
    //   TC  #V by 3 eigen Matrix of mesh vertex UC coorindates between 0 and 1
    //   TF  #F by 3 eigen Matrix of face (triangle/quad) texture indices, <0
    //     means no texture
    //   W  #V by #H eigen Matrix of per mesh vertex, per handle weights
    //   W_index  Specifies the index of the "weight" vertex attribute: see
    //     glBindAttribLocation, if W_index is 0 then weights are ignored
    //   WI  #V by #H eigen Matrix of per mesh vertex, per handle weight ids
    //   WI_index  Specifies the index of the "weight" vertex attribute: see
    //     glBindAttribLocation, if WI_index is 0 then weight indices are ignored
    IGL_INLINE void draw_mesh(
      const Eigen::MatrixXd & V,
      const Eigen::MatrixXi & F,
      const Eigen::MatrixXd & N,
      const Eigen::MatrixXi & NF,
      const Eigen::MatrixXd & C,
      const Eigen::MatrixXd & TC,
      const Eigen::MatrixXi & TF,
      const Eigen::MatrixXd & W,
      const GLuint W_index,
      const Eigen::MatrixXi & WI,
      const GLuint WI_index);

  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "draw_mesh.cpp"
#endif

#endif
