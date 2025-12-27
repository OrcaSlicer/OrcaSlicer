// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_SLICE_TETS_H
#define IGL_SLICE_TETS_H
#include "igl_inline.h"

#include <Eigen/Dense>
#include <Eigen/Sparse>

#include <vector>

namespace igl
{
  // SLICE_TETS Slice through a tet mesh (V,T) along a given plane (via its
  // implicit equation).
  //
  // Inputs:
  //   V  #V by 3 list of tet mesh vertices
  //   T  #T by 4 list of tet indices into V 
  ////   plane  list of 4 coefficients in the plane equation: [x y z 1]'*plane = 0
  //   S  #V list of values so that S = 0 is the desired isosurface
  // Outputs:
  //   SV  #SV by 3 list of triangle mesh vertices along slice
  //   SF  #SF by 3 list of triangles indices into SV
  //   J  #SF list of indices into T revealing from which tet each faces comes
  //   BC  #SU by #V list of barycentric coordinates (or more generally: linear
  //     interpolation coordinates) so that SV = BC*V
  // 
  template <
    typename DerivedV, 
    typename DerivedT, 
    typename DerivedS,
    typename DerivedSV,
    typename DerivedSF,
    typename DerivedJ,
    typename BCType>
  IGL_INLINE void slice_tets(
    const Eigen::MatrixBase<DerivedV>& V,
    const Eigen::MatrixBase<DerivedT>& T,
    const Eigen::MatrixBase<DerivedS> & S,
    Eigen::PlainObjectBase<DerivedSV>& SV,
    Eigen::PlainObjectBase<DerivedSF>& SF,
    Eigen::PlainObjectBase<DerivedJ>& J,
    Eigen::SparseMatrix<BCType> & BC);
  template <
    typename DerivedV, 
    typename DerivedT, 
    typename DerivedS,
    typename DerivedSV,
    typename DerivedSF,
    typename DerivedJ>
  IGL_INLINE void slice_tets(
    const Eigen::MatrixBase<DerivedV>& V,
    const Eigen::MatrixBase<DerivedT>& T,
    const Eigen::MatrixBase<DerivedS> & S,
    Eigen::PlainObjectBase<DerivedSV>& SV,
    Eigen::PlainObjectBase<DerivedSF>& SF,
    Eigen::PlainObjectBase<DerivedJ>& J);
  // Outputs:
  //   sE  #SV by 2 list of sorted edge indices into V
  //   lambda  #SV by 1 list of parameters along each edge in sE so that:
  //     SV(i,:) = V(sE(i,1),:)*lambda(i) + V(sE(i,2),:)*(1-lambda(i));
  template <
    typename DerivedV, 
    typename DerivedT, 
    typename DerivedS,
    typename DerivedSV,
    typename DerivedSF,
    typename DerivedJ,
    typename DerivedsE,
    typename Derivedlambda
    >
  IGL_INLINE void slice_tets(
    const Eigen::MatrixBase<DerivedV>& V,
    const Eigen::MatrixBase<DerivedT>& T,
    const Eigen::MatrixBase<DerivedS> & S,
    Eigen::PlainObjectBase<DerivedSV>& SV,
    Eigen::PlainObjectBase<DerivedSF>& SF,
    Eigen::PlainObjectBase<DerivedJ>& J,
    Eigen::PlainObjectBase<DerivedsE>& sE,
    Eigen::PlainObjectBase<Derivedlambda>& lambda);

}

#ifndef IGL_STATIC_LIBRARY
#  include "slice_tets.cpp"
#endif

#endif


