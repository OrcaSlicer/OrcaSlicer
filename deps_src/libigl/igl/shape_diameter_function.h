// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2017 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_SHAPE_DIAMETER_FUNCTION_H
#define IGL_SHAPE_DIAMETER_FUNCTION_H
#include "igl_inline.h"
#include "AABB.h"
#include <Eigen/Core>
#include <functional>
namespace igl
{
  // Compute shape diamater function per given point. In the parlence of the
  // paper "Consistent Mesh Partitioning and Skeletonisation using the Shape
  // Diameter Function" [Shapiro et al. 2008], this implementation uses a 180°
  // cone and a _uniform_ average (_not_ a average weighted by inverse angles).
  //
  // Inputs:
  //    shoot_ray  function handle that outputs hits of a given ray against a
  //      mesh (embedded in function handles as captured variable/data)
  //    P  #P by 3 list of origin points
  //    N  #P by 3 list of origin normals
  // Outputs:
  //    S  #P list of shape diamater function values between bounding box
  //    diagonal (perfect sphere) and 0 (perfect needle hook)
  //
  template <
    typename DerivedP,
    typename DerivedN,
    typename DerivedS >
  IGL_INLINE void shape_diameter_function(
    const std::function<
      double(
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
  IGL_INLINE void shape_diameter_function(
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
  IGL_INLINE void shape_diameter_function(
    const Eigen::PlainObjectBase<DerivedV> & V,
    const Eigen::PlainObjectBase<DerivedF> & F,
    const Eigen::PlainObjectBase<DerivedP> & P,
    const Eigen::PlainObjectBase<DerivedN> & N,
    const int num_samples,
    Eigen::PlainObjectBase<DerivedS> & S);
  //   per_face  whether to compute per face (S is #F by 1) or per vertex (S is
  //     #V by 1)
  template <
    typename DerivedV,
    typename DerivedF,
    typename DerivedS>
  IGL_INLINE void shape_diameter_function(
    const Eigen::PlainObjectBase<DerivedV> & V,
    const Eigen::PlainObjectBase<DerivedF> & F,
    const bool per_face,
    const int num_samples,
    Eigen::PlainObjectBase<DerivedS> & S);
};
#ifndef IGL_STATIC_LIBRARY
#  include "shape_diameter_function.cpp"
#endif

#endif

