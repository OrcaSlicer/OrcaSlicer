// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_CGAL_ASSIGN_H
#define IGL_COPYLEFT_CGAL_ASSIGN_H
#include "../../igl_inline.h"
#include <Eigen/Core>
#include <CGAL/Exact_predicates_exact_constructions_kernel.h>

namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      template <typename DerivedC, typename DerivedD>
      IGL_INLINE void assign(
        const Eigen::MatrixBase<DerivedC> & C,
        Eigen::PlainObjectBase<DerivedD> & D);
      template <typename ReturnScalar, typename DerivedC>
      IGL_INLINE 
      Eigen::Matrix<
        ReturnScalar,
        DerivedC::RowsAtCompileTime, 
        DerivedC::ColsAtCompileTime, 
        1,
        DerivedC::MaxRowsAtCompileTime, 
        DerivedC::MaxColsAtCompileTime> 
      assign(
        const Eigen::MatrixBase<DerivedC> & C);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "assign.cpp"
#endif
#endif
