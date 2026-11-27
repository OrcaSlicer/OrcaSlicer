// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_MOD_H
#define IGL_MOD_H
#include "igl_inline.h"
#include "PlainMatrix.h"
#include <Eigen/Core>
namespace igl
{
  /// Compute elementwise mod: B = A % base
  ///
  /// @param[in] A  m by n matrix
  /// @param[in] base  number to mod against
  /// @param[out] B  m by n matrix
  template <typename DerivedA, typename DerivedB>
  IGL_INLINE void mod(
    const Eigen::MatrixBase<DerivedA> & A,
    const int base,
    Eigen::PlainObjectBase<DerivedB> & B);
  /// \overload
  template <typename DerivedA>
  IGL_INLINE igl::PlainMatrix<DerivedA> mod(
    const Eigen::MatrixBase<DerivedA> & A, const int base);
}
#ifndef IGL_STATIC_LIBRARY
#include "mod.cpp"
#endif
#endif
