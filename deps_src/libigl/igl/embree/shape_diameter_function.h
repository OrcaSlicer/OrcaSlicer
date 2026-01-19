// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_EMBREE_SHAPE_DIAMETER_FUNCTION_H
#define IGL_EMBREE_SHAPE_DIAMETER_FUNCTION_H
#include "../igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  namespace embree
  {
    // Forward define
    class EmbreeIntersector;
    // Compute shape diamter function per given point
    //
    // Inputs:
    //    ei  EmbreeIntersector containing (V,F)
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
      const EmbreeIntersector & ei,
      const Eigen::PlainObjectBase<DerivedP> & P,
      const Eigen::PlainObjectBase<DerivedN> & N,
      const int num_samples,
      Eigen::PlainObjectBase<DerivedS> & S);
    // Wrapper which builds new EmbreeIntersector for (V,F). That's expensive so
    // avoid this if repeatedly calling.
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
  }
};
#ifndef IGL_STATIC_LIBRARY
#  include "shape_diameter_function.cpp"
#endif

#endif

