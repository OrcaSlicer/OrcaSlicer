// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_WRITE_WRL_H
#define IGL_WRITE_WRL_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <string>
namespace igl
{
  /// Write mesh to a .wrl file
  ///
  /// @param[in] str  path to .wrl file
  /// @param[in] V  #V by 3 list of vertex positions
  /// @param[in] F  #F by 3 list of triangle indices
  /// @return true iff succes
  template <typename DerivedV, typename DerivedF>
  IGL_INLINE bool writeWRL(
    const std::string & str,
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F);
  /// \overload
  ///
  /// @param[in] C  double matrix of rgb values per vertex #V by 3
  template <typename DerivedV, typename DerivedF, typename DerivedC>
  IGL_INLINE bool writeWRL(
    const std::string & str,
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    const Eigen::MatrixBase<DerivedC> & C);
}
#ifndef IGL_STATIC_LIBRARY
#include "writeWRL.cpp"
#endif
#endif
