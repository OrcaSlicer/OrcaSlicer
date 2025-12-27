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
  // Compute ambient occlusion per given point
  //
  // Inputs:
  //    shoot_ray  function handle that outputs hits of a given ray against a
  //      mesh (embedded in function handles as captured variable/data)
  //    P  #P by 3 list of origin points
  //    N  #P by 3 list of origin normals
  // Outputs:
  //    S  #P list of ambient occlusion values between 1 (fully occluded) and
  //      0 (not occluded)
  //
  template <
    typename DerivedP,
    typename DerivedN,
    typename DerivedS >
  IGL_INLINE void ambient_occlusion(
    const std::function<
      bool(
        const Eigen::Vector3f&,
        const Eigen::Vector3f&)
        > & shoot_ray,
    const Eigen::PlainObjectBase<DerivedP> & P,
    const Eigen::PlainObjectBase<DerivedN> & N,
    const int num_samples,
    Eigen::PlainObjectBase<DerivedS> & S);
  // Inputs:
  //   AABB  axis-aligned bounding box hierarchy around (V,F)
  template <
    typename DerivedV,
    int DIM,
    typename DerivedF,
    typename DerivedP,
    typename DerivedN,
    typename DerivedS >
  IGL_INLINE void ambient_occlusion(
    const igl::AABB<DerivedV,DIM> & aabb,
    const Eigen::PlainObjectBase<DerivedV> & V,
    const Eigen::PlainObjectBase<DerivedF> & F,
    const Eigen::PlainObjectBase<DerivedP> & P,
    const Eigen::PlainObjectBase<DerivedN> & N,
    const int num_samples,
    Eigen::PlainObjectBase<DerivedS> & S);
  // Inputs:
  //    V  #V by 3 list of mesh vertex positions
  //    F  #F by 3 list of mesh face indices into V
  template <
    typename DerivedV,
    typename DerivedF,
    typename DerivedP,
    typename DerivedN,
    typename DerivedS >
  IGL_INLINE void ambient_occlusion(
    const Eigen::PlainObjectBase<DerivedV> & V,
    const Eigen::PlainObjectBase<DerivedF> & F,
    const Eigen::PlainObjectBase<DerivedP> & P,
    const Eigen::PlainObjectBase<DerivedN> & N,
    const int num_samples,
    Eigen::PlainObjectBase<DerivedS> & S);

};
#ifndef IGL_STATIC_LIBRARY
#  include "ambient_occlusion.cpp"
#endif

#endif
