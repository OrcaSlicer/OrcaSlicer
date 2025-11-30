// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_FACE_OCCURRENCES
#define IGL_FACE_OCCURRENCES
#include "igl_inline.h"
#include <Eigen/Core>

#include <vector>
namespace igl
{
  /// Count the occurances of each face (row) in a list of face indices
  /// (irrespecitive of order)
  ///
  /// @param[in] F  #F by simplex-size
  /// @param[out] C  #F list of counts
  ///
  /// \pre triangles/tets only (where ignoring order still gives simplex)
  template <typename IntegerF, typename IntegerC>
  IGL_INLINE void face_occurrences(
    const std::vector<std::vector<IntegerF> > & F,
    std::vector<IntegerC> & C);
  /// \overload
  template <typename DerivedF, typename DerivedC>
  IGL_INLINE void face_occurrences(
    const Eigen::MatrixBase<DerivedF> & F,
    Eigen::PlainObjectBase<DerivedC> & C);
}

#ifndef IGL_STATIC_LIBRARY
#  include "face_occurrences.cpp"
#endif

#endif


