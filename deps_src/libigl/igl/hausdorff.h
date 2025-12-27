// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_HAUSDORFF_H
#define IGL_HAUSDORFF_H
#include "igl_inline.h"

#include <Eigen/Dense>
#include <functional>

namespace igl 
{
  // HAUSDORFF compute the Hausdorff distance between mesh (VA,FA) and mesh
  // (VB,FB). This is the 
  //
  // d(A,B) = max ( max min d(a,b) , max min d(b,a) )
  //                a∈A b∈B          b∈B a∈A
  //
  // Known issue: This is only computing max(min(va,B),min(vb,A)). This is
  // better than max(min(va,Vb),min(vb,Va)). This (at least) is missing
  // "edge-edge" cases like the distance between the two different
  // triangulations of a non-planar quad in 3D. Even simpler, consider the
  // Hausdorff distance between the non-convex, block letter V polygon (with 7
  // vertices) in 2D and its convex hull. The Hausdorff distance is defined by
  // the midpoint in the middle of the segment across the concavity and some
  // non-vertex point _on the edge_ of the V.
  //
  // Inputs:
  //   VA  #VA by 3 list of vertex positions
  //   FA  #FA by 3 list of face indices into VA
  //   VB  #VB by 3 list of vertex positions
  //   FB  #FB by 3 list of face indices into VB
  // Outputs:
  //   d  hausdorff distance
  //   //pair  2 by 3 list of "determiner points" so that pair(1,:) is from A
  //   //  and pair(2,:) is from B
  //
  template <
    typename DerivedVA, 
    typename DerivedFA,
    typename DerivedVB,
    typename DerivedFB,
    typename Scalar>
  IGL_INLINE void hausdorff(
    const Eigen::PlainObjectBase<DerivedVA> & VA, 
    const Eigen::PlainObjectBase<DerivedFA> & FA,
    const Eigen::PlainObjectBase<DerivedVB> & VB, 
    const Eigen::PlainObjectBase<DerivedFB> & FB,
    Scalar & d);
  // Compute lower and upper bounds (l,u) on the Hausdorff distance between a triangle
  // (V) and a pointset (e.g., mesh, triangle soup) given by a distance function
  // handle (dist_to_B).
  //
  // Inputs:
  //   V   3 by 3 list of corner positions so that V.row(i) is the position of the
  //     ith corner
  //   dist_to_B  function taking the x,y,z coordinate of a query position and
  //     outputting the closest-point distance to some point-set B
  // Outputs:
  //   l  lower bound on Hausdorff distance 
  //   u  upper bound on Hausdorff distance
  //
  template <
    typename DerivedV,
    typename Scalar>
  IGL_INLINE void hausdorff(
    const Eigen::MatrixBase<DerivedV>& V,
    const std::function<
      Scalar(const Scalar &,const Scalar &, const Scalar &)> & dist_to_B,
    Scalar & l,
    Scalar & u);
}

#ifndef IGL_STATIC_LIBRARY
#  include "hausdorff.cpp"
#endif

#endif

