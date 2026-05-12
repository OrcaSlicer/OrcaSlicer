// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2024 Alec Jacobson
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_TRIANGLE_REFINE_H
#define IGL_TRIANGLE_REFINE_H
#include "../igl_inline.h"
#include <string>
#include <Eigen/Core>

namespace igl
{
  namespace triangle
  {
    /// Refine an existing triangulation.
    ///
    /// @param[in] V #V by 2 list of 2D vertex positions
    /// @param[in] E #E by 2 list of vertex ids forming segments
    /// @param[in] F #F by 3 list of vertex ids forming triangles
    /// @param[in] flags  string of options pass to triangle (see triangle documentation)
    /// @param[out] V2  #V2 by 2  coordinates of the vertives of the generated triangulation
    /// @param[out] F2  #F2 by 3  list of indices forming the faces of the generated triangulation
    template <
      typename DerivedV,
      typename DerivedE,
      typename DerivedF,
      typename DerivedV2,
      typename DerivedF2>
    IGL_INLINE void refine(
      const Eigen::MatrixBase<DerivedV> & V,
      const Eigen::MatrixBase<DerivedE> & E,
      const Eigen::MatrixBase<DerivedF> & F,
      const std::string flags,
      Eigen::PlainObjectBase<DerivedV2> & V2,
      Eigen::PlainObjectBase<DerivedF2> & F2);
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "refine.cpp"
#endif

#endif

