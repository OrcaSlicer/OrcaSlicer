// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_CGAL_PIECEWISE_CONSTANT_WINDING_NUMBER_H
#define IGL_COPYLEFT_CGAL_PIECEWISE_CONSTANT_WINDING_NUMBER_H
#include "../../igl_inline.h"
#include <Eigen/Core>
#include <vector>
namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      // PIECEWISE_CONSTANT_WINDING_NUMBER Determine if a given mesh induces a
      // piecewise constant winding number field: Is this mesh valid input to
      // solid set operations.
      // 
      // Inputs:
      //   V  #V by 3 list of mesh vertex positions
      //   F  #F by 3 list of triangle indices into V
      // Returns true if the mesh _combinatorially_ induces a piecewise
      // constant winding number field.
      template <
        typename DerivedV,
        typename DerivedF>
      IGL_INLINE bool piecewise_constant_winding_number(
        const Eigen::PlainObjectBase<DerivedV> & V,
        const Eigen::PlainObjectBase<DerivedF>& F);
    }
  }
}
#ifndef IGL_STATIC_LIBRARY
#  include "piecewise_constant_winding_number.cpp"
#endif
#endif

