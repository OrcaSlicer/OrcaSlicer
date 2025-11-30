// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2018 Zhongshi Jiang <jiangzs@nyu.edu>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_TOPOLOGICAL_HOLE_FILL_H
#define IGL_TOPOLOGICAL_HOLE_FILL_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <Eigen/Sparse>
namespace igl
{
    
  /// Topological fill hole on a mesh, with one additional vertex each hole
  /// Index of new abstract vertices will be F.maxCoeff() + (index of hole)
  ///
  /// @param[in] F  #F by simplex-size list of element indices
  /// @param[in] holes vector of hole loops to fill
  /// @param[out] F_filled  input F stacked with filled triangles.
  ///
  /// \bug Why does this add a new vertex for each hole? Why not use a fan?
  template <
  typename DerivedF,
  typename VectorIndex,
  typename DerivedF_filled>
IGL_INLINE void topological_hole_fill(
  const Eigen::MatrixBase<DerivedF> & F,
  const std::vector<VectorIndex> & holes,
  Eigen::PlainObjectBase<DerivedF_filled> &F_filled);

}


#ifndef IGL_STATIC_LIBRARY
#  include "topological_hole_fill.cpp"
#endif

#endif
