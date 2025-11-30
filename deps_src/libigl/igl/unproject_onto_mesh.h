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
  /// Unproject a screen location (using current opengl viewport, projection, and
  /// model view) to a 3D position _onto_ a given mesh, if the ray through the
  /// given screen location (x,y) _hits_ the mesh.
  ///
  /// @param[in] pos        screen space coordinates
  /// @param[in] model      model matrix
  /// @param[in] proj       projection matrix
  /// @param[in] viewport   vieweport vector
  /// @param[in] V   #V by 3 list of mesh vertex positions
  /// @param[in] F   #F by 3 list of mesh triangle indices into V
  /// @param[out] fid  id of the first face hit
  /// @param[out] bc  barycentric coordinates of hit
  /// @return true if there's a hit
  ///
  /// #### Example:
  ///
  /// \code{cpp}
  /// igl::opengl::glfw::Viewer vr;
  /// ...
  /// igl::unproject_onto_mesh(
  ///   pos,vr.core().view,vr.core().proj,vr.core().viewport,V,F,fid,bc);
  /// \endcode
  ///
  template < typename DerivedV, typename DerivedF, typename Derivedbc>
  IGL_INLINE bool unproject_onto_mesh(
    const Eigen::Vector2f& pos,
    const Eigen::Matrix4f& model,
    const Eigen::Matrix4f& proj,
    const Eigen::Vector4f& viewport,
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    int & fid,
    Eigen::PlainObjectBase<Derivedbc> & bc);
  /// \overload
  /// @param[in] shoot_ray  function handle that outputs hits of a given ray against a
  ///      mesh (embedded in function handles as captured variable/data)
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
        igl::Hit<float>  &)
        > & shoot_ray,
    int & fid,
    Eigen::PlainObjectBase<Derivedbc> & bc);
}
#ifndef IGL_STATIC_LIBRARY
#  include "unproject_onto_mesh.cpp"
#endif
#endif


