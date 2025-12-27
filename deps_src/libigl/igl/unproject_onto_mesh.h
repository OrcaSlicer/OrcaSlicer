// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_UNPROJECT_ONTO_MESH
#define IGL_UNPROJECT_ONTO_MESH
#include "igl_inline.h"
#include "Hit.h"
#include <Eigen/Core>
#include <functional>

namespace igl
{
  // Unproject a screen location (using current opengl viewport, projection, and
  // model view) to a 3D position _onto_ a given mesh, if the ray through the
  // given screen location (x,y) _hits_ the mesh.
  //
  // Inputs:
  //    pos        screen space coordinates
  //    model      model matrix
  //    proj       projection matrix
  //    viewport   vieweport vector
  //    V   #V by 3 list of mesh vertex positions
  //    F   #F by 3 list of mesh triangle indices into V
  // Outputs:
  //    fid  id of the first face hit
  //    bc  barycentric coordinates of hit
  // Returns true if there's a hit
  template < typename DerivedV, typename DerivedF, typename Derivedbc>
  IGL_INLINE bool unproject_onto_mesh(
    const Eigen::Vector2f& pos,
    const Eigen::Matrix4f& model,
    const Eigen::Matrix4f& proj,
    const Eigen::Vector4f& viewport,
    const Eigen::PlainObjectBase<DerivedV> & V,
    const Eigen::PlainObjectBase<DerivedF> & F,
    int & fid,
    Eigen::PlainObjectBase<Derivedbc> & bc);
  //
  // Inputs:
  //    pos        screen space coordinates
  //    model      model matrix
  //    proj       projection matrix
  //    viewport   vieweport vector
  //    shoot_ray  function handle that outputs hits of a given ray against a
  //      mesh (embedded in function handles as captured variable/data)
  // Outputs:
  //    fid  id of the first face hit
  //    bc  barycentric coordinates of hit
  // Returns true if there's a hit
  template <typename Derivedbc>
  IGL_INLINE bool unproject_onto_mesh(
    const Eigen::Vector2f& pos,
    const Eigen::Matrix4f& model,
    const Eigen::Matrix4f& proj,
    const Eigen::Vector4f& viewport,
    const std::function<
      bool(
        const Eigen::Vector3f&,
        const Eigen::Vector3f&,
        igl::Hit  &)
        > & shoot_ray,
    int & fid,
    Eigen::PlainObjectBase<Derivedbc> & bc);
}
#ifndef IGL_STATIC_LIBRARY
#  include "unproject_onto_mesh.cpp"
#endif
#endif


