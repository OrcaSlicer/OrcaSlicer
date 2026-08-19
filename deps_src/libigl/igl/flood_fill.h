// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_FLOOD_FILL_H
#define IGL_FLOOD_FILL_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  /// Given a 3D array with sparse non-nan (number?) data fill in the NaNs via
  /// flood fill. This should ensure that, e.g., if data near 0 always has a band
  /// (surface) of numbered and signed data, then components of nans will be
  /// correctly signed.
  ///
  /// @param[in] res  3-long list of dimensions of grid
  /// @param[in,out] S  res(0)*res(1)*res(2)  list of scalar values (with (many)
  ///   nans), flood fill data in place
  template <typename Derivedres, typename DerivedS>
  IGL_INLINE void flood_fill(
    const Eigen::MatrixBase<Derivedres>& res, 
    Eigen::PlainObjectBase<DerivedS> & S);
}
#ifndef IGL_STATIC_LIBRARY
#  include "flood_fill.cpp"
#endif
#endif
