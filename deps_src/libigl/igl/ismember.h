// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_ISMEMBER_H
#define IGL_ISMEMBER_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  /// Determine if elements of A exist in elements of B
  ///
  /// @param[in] A  ma by na matrix
  /// @param[in] B  mb by nb matrix
  /// @param[out] IA  ma by na matrix of flags whether corresponding element of
  ///   A exists in B
  /// @param[out] LOCB  ma by na matrix of indices in B locating matching
  ///   element (-1 if not found), indices assume column major ordering
  ///
  template <
    typename DerivedA,
    typename DerivedB,
    typename DerivedIA,
    typename DerivedLOCB>
  IGL_INLINE void ismember(
    const Eigen::MatrixBase<DerivedA> & A,
    const Eigen::MatrixBase<DerivedB> & B,
    Eigen::PlainObjectBase<DerivedIA> & IA,
    Eigen::PlainObjectBase<DerivedLOCB> & LOCB);
}

#ifndef IGL_STATIC_LIBRARY
#  include "ismember.cpp"
#endif
#endif

