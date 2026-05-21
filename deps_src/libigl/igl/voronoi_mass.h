// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2024 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_VORONOI_MASS_H
#define IGL_VORONOI_MASS_H

#include <igl/igl_inline.h>
#include <Eigen/Core>
namespace igl
{
/// Compute the mass matrix entries for a given tetrahedral mesh (V,T) using the
/// "hybrid" voronoi volume of each vertex.
/// 
///  @param[in] V  #V by 3 list of vertex positions
///  @param[in] T  #T by 4 list of element indices into V
///  @param[out] M  #V list of mass matrix diagonal entries (will be positive as
///    long as tets are not degenerate)
///
template <
  typename DerivedV,
  typename DerivedT,
  typename DerivedM>
IGL_INLINE void voronoi_mass(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedT> & T,
    Eigen::PlainObjectBase<DerivedM> & M);
}

#ifndef IGL_STATIC_LIBRARY
#  include "voronoi_mass.cpp"
#endif

#endif
