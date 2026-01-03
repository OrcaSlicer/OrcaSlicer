// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_UNPROJECT_IN_MESH
#define IGL_UNPROJECT_IN_MESH
#include "igl_inline.h"
#include <Eigen/Core>

#include <vector>
#include "Hit.h"

namespace igl
{
  /// Unproject a screen location (using current opengl viewport, projection, and
  /// model view) to a 3D position _inside_ a given mesh. If the ray through the
  /// given screen location (x,y) _hits_ the mesh more than twice then the 3D
  /// midpoint between the first two hits is return. If it hits once, then that
  /// point is return. If it does not hit the mesh then obj is not set.
  ///
  /// @param[in] pos        screen space coordinates
  /// @param[in] model      model matrix
  /// @param[in] proj       projection matrix
  /// @param[in] viewport   vieweport vector
  /// @param[in] V   #V by 3 list of mesh vertex positions
  /// @param[in] F   #F by 3 list of mesh triangle indices into V
  /// @param[out] obj        3d unprojected mouse point in mesh
  /// @param[out] hits       vector of hits
  /// @return number of hits
  ///
  template < typename DerivedV, typename DerivedF, typename Derivedobj>
    IGL_INLINE int unproject_in_mesh(
        const Eigen::Vector2f& pos,
        const Eigen::Matrix4f& model,
        const Eigen::Matrix4f& proj,
        const Eigen::Vector4f& viewport,
        const Eigen::MatrixBase<DerivedV> & V,
        const Eigen::MatrixBase<DerivedF> & F,
        Eigen::PlainObjectBase<Derivedobj> & obj,
        std::vector<igl::Hit<float> > & hits);
  /// \overload
  template < typename DerivedV, typename DerivedF, typename Derivedobj>
    IGL_INLINE int unproject_in_mesh(
        const Eigen::Vector2f& pos,
        const Eigen::Matrix4f& model,
        const Eigen::Matrix4f& proj,
        const Eigen::Vector4f& viewport,
        const Eigen::MatrixBase<DerivedV> & V,
        const Eigen::MatrixBase<DerivedF> & F,
        Eigen::PlainObjectBase<Derivedobj> & obj);
  /// \overload
  ///
  /// @param[in] shoot_ray  function handle that outputs first hit of a given ray
  ///      against a mesh (embedded in function handles as captured
  ///      variable/data)
  template < typename Derivedobj>
    IGL_INLINE int unproject_in_mesh(
        const Eigen::Vector2f& pos,
        const Eigen::Matrix4f& model,
        const Eigen::Matrix4f& proj,
        const Eigen::Vector4f& viewport,
        const std::function<
          void(
            const Eigen::Vector3f&,
            const Eigen::Vector3f&,
            std::vector<igl::Hit<float>> &)
            > & shoot_ray,
        Eigen::PlainObjectBase<Derivedobj> & obj,
        std::vector<igl::Hit<float> > & hits);
}
#ifndef IGL_STATIC_LIBRARY
#  include "unproject_in_mesh.cpp"
#endif
#endif

