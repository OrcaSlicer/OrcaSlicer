// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_AMBIENT_OCCLUSION_H
#define IGL_AMBIENT_OCCLUSION_H
#include "igl_inline.h"
#include "AABB.h"
#include <Eigen/Core>
#include <functional>
namespace igl
{
  /// Compute ambient occlusion per given point using ray-mesh intersection
  /// function handle.
  ///
  /// @param[in]  shoot_ray  function handle that outputs hits of a given ray against a
  ///               mesh (embedded in function handles as captured variable/data)
  /// @param[in]  P  #P by 3 list of origin points
  /// @param[in]  N  #P by 3 list of origin normals
  /// @param[in] num_samples  number of samples to use (e.g., 1000)
  /// @param[out]  S  #P list of ambient occlusion values between 1 (fully occluded) and
  ///      0 (not occluded)
  ///
  template <
    typename DerivedP,
    typename DerivedN,
    typename DerivedS >
  IGL_INLINE void ambient_occlusion(
    const std::function<
      bool(
        const Eigen::Matrix<typename DerivedP::Scalar,3,1>&,
        const Eigen::Matrix<typename DerivedP::Scalar,3,1>&)
        > & shoot_ray,
    const Eigen::MatrixBase<DerivedP> & P,
    const Eigen::MatrixBase<DerivedN> & N,
    const int num_samples,
    Eigen::PlainObjectBase<DerivedS> & S);
  /// Compute ambient occlusion per given point for mesh (V,F) with precomputed
  /// AABB tree.
  ///
  //  @param[in] AABB  axis-aligned bounding box hierarchy around (V,F)
  /// @param[in] V  #V by 3 list of mesh vertex positions
  /// @param[in] F  #F by 3 list of mesh face indices into V
  /// @param[in]  P  #P by 3 list of origin points
  /// @param[in]  N  #P by 3 list of origin normals
  /// @param[in] num_samples  number of samples to use (e.g., 1000)
  /// @param[out]  S  #P list of ambient occlusion values between 1 (fully occluded) and
  ///      0 (not occluded)
  ///
  template <
    typename DerivedV,
    int DIM,
    typename DerivedF,
    typename DerivedP,
    typename DerivedN,
    typename DerivedS >
  IGL_INLINE void ambient_occlusion(
    const igl::AABB<DerivedV,DIM> & aabb,
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    const Eigen::MatrixBase<DerivedP> & P,
    const Eigen::MatrixBase<DerivedN> & N,
    const int num_samples,
    Eigen::PlainObjectBase<DerivedS> & S);
  /// Compute ambient occlusion per given point for mesh (V,F)
  ///
  /// @param[in] V  #V by 3 list of mesh vertex positions
  /// @param[in] F  #F by 3 list of mesh face indices into V
  /// @param[in]  P  #P by 3 list of origin points
  /// @param[in]  N  #P by 3 list of origin normals
  /// @param[in] num_samples  number of samples to use (e.g., 1000)
  /// @param[out]  S  #P list of ambient occlusion values between 1 (fully occluded) and
  ///      0 (not occluded)
  template <
    typename DerivedV,
    typename DerivedF,
    typename DerivedP,
    typename DerivedN,
    typename DerivedS >
  IGL_INLINE void ambient_occlusion(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    const Eigen::MatrixBase<DerivedP> & P,
    const Eigen::MatrixBase<DerivedN> & N,
    const int num_samples,
    Eigen::PlainObjectBase<DerivedS> & S);

};
#ifndef IGL_STATIC_LIBRARY
#  include "ambient_occlusion.cpp"
#endif

#endif
