// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_PARULA_H
#define IGL_PARULA_H
#include "igl_inline.h"
//#ifndef IGL_NO_EIGEN
#  include <Eigen/Dense>
//#endif
namespace igl
{
  /// Parula colormap like MATLAB's parula
  ///
  /// @param[in] f  factor determining color value as if 0 was min and 1 was max
  /// @param[out] rgb resulting rgb color
  ///   - r  red value
  ///   - g  green value
  ///   - b  blue value
  template <typename T>
  IGL_INLINE void parula(const T f, T * rgb);
  /// \overload
  template <typename T>
  IGL_INLINE void parula(const T f, T & r, T & g, T & b);
  /// \overload
  /// @param[in] Z  #Z list of factors
  /// @param[in] normalize  whether to normalize Z to be tightly between [0,1]
  /// @param[out] C  #C by 3 list of rgb colors
  template <typename DerivedZ, typename DerivedC>
  IGL_INLINE void parula(
    const Eigen::MatrixBase<DerivedZ> & Z,
    const bool normalize,
    Eigen::PlainObjectBase<DerivedC> & C);
  /// \overload
  /// @param[in] min_z  value at blue
  /// @param[in] max_z  value at red
  template <typename DerivedZ, typename DerivedC>
  IGL_INLINE void parula(
    const Eigen::MatrixBase<DerivedZ> & Z,
    const double min_Z,
    const double max_Z,
    Eigen::PlainObjectBase<DerivedC> & C);
};
  
#ifndef IGL_STATIC_LIBRARY
#  include "parula.cpp"
#endif

#endif

