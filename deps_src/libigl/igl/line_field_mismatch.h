// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Nico Pietroni <nico.pietroni@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#ifndef IGL_LINE_FIELD_MISSMATCH_H
#define IGL_LINE_FIELD_MISSMATCH_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  /// Calculates the mismatch (integer), at each face edge, of a cross field defined on the mesh faces.
  /// The integer mismatch is a multiple of pi/2 that transforms the cross on one side of the edge to
  /// the cross on the other side. It represents the deviation from a Lie connection across the edge.
  ///
  /// @param[in] V         #V by 3 eigen Matrix of mesh vertex 3D positions
  /// @param[in] F         #F by 3 eigen Matrix of face (quad) indices
  /// @param[in] PD1       #F by 3 eigen Matrix of the first per face cross field vector
  /// @param[in] PD2       #F by 3 eigen Matrix of the second per face cross field vector
  /// @param[in] isCombed  boolean, specifying whether the field is combed (i.e. matching has been precomputed.
  ///             If not, the field is combed first.
  /// @param[out] mismatch  #F by 3 eigen Matrix containing the integer mismatch of the cross field
  ///             across all face edges
  ///
  template <typename DerivedV, typename DerivedF, typename DerivedO>
  IGL_INLINE void line_field_mismatch(
    const Eigen::MatrixBase<DerivedV> &V,
    const Eigen::MatrixBase<DerivedF> &F,
    const Eigen::MatrixBase<DerivedV> &PD1,
    const bool isCombed,
    Eigen::PlainObjectBase<DerivedO> &mismatch);
}
#ifndef IGL_STATIC_LIBRARY
#include "line_field_mismatch.cpp"
#endif

#endif
