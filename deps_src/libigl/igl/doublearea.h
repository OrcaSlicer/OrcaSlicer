// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_DOUBLEAREA_H
#define IGL_DOUBLEAREA_H
#include "igl_inline.h"
#include <Eigen/Dense>
namespace igl
{
  // DOUBLEAREA computes twice the area for each input triangle[quad]
  //
  // Templates:
  //   DerivedV  derived type of eigen matrix for V (e.g. derived from
  //     MatrixXd)
  //   DerivedF  derived type of eigen matrix for F (e.g. derived from
  //     MatrixXi)
  //   DeriveddblA  derived type of eigen matrix for dblA (e.g. derived from
  //     MatrixXd)
  // Inputs:
  //   V  #V by dim list of mesh vertex positions
  //   F  #F by simplex_size list of mesh faces (must be triangles or quads)
  // Outputs:
  //   dblA  #F list of triangle[quad] double areas (SIGNED only for 2D input)
  //
  // Known bug: For dim==3 complexity is O(#V + #F)!! Not just O(#F). This is a big deal
  // if you have 1million unreferenced vertices and 1 face
  template <typename DerivedV, typename DerivedF, typename DeriveddblA>
  IGL_INLINE void doublearea(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    Eigen::PlainObjectBase<DeriveddblA> & dblA);
  // Stream of triangles, computes signed area...
  template <
    typename DerivedA,
    typename DerivedB,
    typename DerivedC,
    typename DerivedD>
  IGL_INLINE void doublearea(
    const Eigen::MatrixBase<DerivedA> & A,
    const Eigen::MatrixBase<DerivedB> & B,
    const Eigen::MatrixBase<DerivedC> & C,
    Eigen::PlainObjectBase<DerivedD> & D);
  // Single triangle in 2D!
  //
  // This should handle streams of corners not just single corners
  template <
    typename DerivedA,
    typename DerivedB,
    typename DerivedC>
  IGL_INLINE typename DerivedA::Scalar doublearea_single(
    const Eigen::MatrixBase<DerivedA> & A,
    const Eigen::MatrixBase<DerivedB> & B,
    const Eigen::MatrixBase<DerivedC> & C);
  // Same as above but use instrinsic edge lengths rather than (V,F) mesh. This
  //
  // Inputs:
  //   l  #F by dim list of edge lengths using
  //     for triangles, columns correspond to edges 23,31,12
  //   nan_replacement  what value should be used for triangles whose given
  //     edge lengths do not obey the triangle inequality. These may be very
  //     wrong (e.g., [100 1 1]) or may be nearly degenerate triangles whose
  //     floating point side length computation leads to breach of the triangle
  //     inequality. One may wish to set this parameter to 0 if side lengths l
  //     are _known_ to come from a valid embedding (e.g., some mesh (V,F)). In
  //     that case, the only circumstance the triangle inequality is broken is
  //     when the triangle is nearly degenerate and floating point error
  //     dominates: hence replacing with zero is reasonable.
  // Outputs:
  //   dblA  #F list of triangle double areas
  template <typename Derivedl, typename DeriveddblA>
  IGL_INLINE void doublearea(
    const Eigen::MatrixBase<Derivedl> & l,
    const typename Derivedl::Scalar nan_replacement,
    Eigen::PlainObjectBase<DeriveddblA> & dblA);
  // default behavior is to assert on NaNs and leave them in place
  template <typename Derivedl, typename DeriveddblA>
  IGL_INLINE void doublearea(
    const Eigen::MatrixBase<Derivedl> & l,
    Eigen::PlainObjectBase<DeriveddblA> & dblA);
  // DOUBLEAREA_QUAD computes twice the area for each input quadrilateral
  //
  // Inputs:
  //   V  #V by dim list of mesh vertex positions
  //   F  #F by simplex_size list of mesh faces (must be quadrilaterals)
  // Outputs:
  //   dblA  #F list of quadrilateral double areas
  //
  template <typename DerivedV, typename DerivedF, typename DeriveddblA>
  IGL_INLINE void doublearea_quad(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  Eigen::PlainObjectBase<DeriveddblA> & dblA);

}

#ifndef IGL_STATIC_LIBRARY
#  include "doublearea.cpp"
#endif

#endif
