// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_OPENGL2_SORT_TRIANGLES_H
#define IGL_OPENGL2_SORT_TRIANGLES_H

#include "../igl_inline.h"
#include <Eigen/Core>

namespace igl
{
  namespace opengl2
  {
    template <
      typename DerivedV,
      typename DerivedF,
      typename DerivedFF,
      typename DerivedI>
    IGL_INLINE void sort_triangles(
      const Eigen::PlainObjectBase<DerivedV> & V,
      const Eigen::PlainObjectBase<DerivedF> & F,
      Eigen::PlainObjectBase<DerivedFF> & FF,
      Eigen::PlainObjectBase<DerivedI> & I);
    template <
      typename DerivedV,
      typename DerivedF,
      typename DerivedFF,
      typename DerivedI>
    IGL_INLINE void sort_triangles_slow(
      const Eigen::PlainObjectBase<DerivedV> & V,
      const Eigen::PlainObjectBase<DerivedF> & F,
      Eigen::PlainObjectBase<DerivedFF> & FF,
      Eigen::PlainObjectBase<DerivedI> & I);
    //template <
    //  typename DerivedV,
    //  typename DerivedF,
    //  typename DerivedMV,
    //  typename DerivedP,
    //  typename DerivedFF,
    //  typename DerivedI>
    //IGL_INLINE void sort_triangles_robust(
    //  const Eigen::PlainObjectBase<DerivedV> & V,
    //  const Eigen::PlainObjectBase<DerivedF> & F,
    //  const Eigen::PlainObjectBase<DerivedMV> & MV,
    //  const Eigen::PlainObjectBase<DerivedP> & P,
    //  Eigen::PlainObjectBase<DerivedFF> & FF,
    //  Eigen::PlainObjectBase<DerivedI> & I);
    //template <
    //  typename DerivedV,
    //  typename DerivedF,
    //  typename DerivedFF,
    //  typename DerivedI>
    //IGL_INLINE void sort_triangles_robust(
    //  const Eigen::PlainObjectBase<DerivedV> & V,
    //  const Eigen::PlainObjectBase<DerivedF> & F,
    //  Eigen::PlainObjectBase<DerivedFF> & FF,
    //  Eigen::PlainObjectBase<DerivedI> & I);
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "sort_triangles.cpp"
#endif

#endif

