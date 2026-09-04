// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2020 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_MARCHING_CUBES_H
#define IGL_MARCHING_CUBES_H
#include "igl_inline.h"
#include <unordered_map>

#include <Eigen/Core>
namespace igl
{
  /// Performs marching cubes reconstruction on a grid defined by values, and
  /// points, and generates a mesh defined by vertices and faces
  ///
  /// @param[in] S   nx*ny*nz list of values at each grid corner
  ///                i.e. S(x + y*xres + z*xres*yres) for corner (x,y,z)
  /// @param[in] GV  nx*ny*nz by 3 array of corresponding grid corner vertex locations
  /// @param[in] nx  resolutions of the grid in x dimension
  /// @param[in] ny  resolutions of the grid in y dimension
  /// @param[in] nz  resolutions of the grid in z dimension
  /// @param[in] isovalue  the isovalue of the surface to reconstruct
  /// @param[out] V  #V by 3 list of mesh vertex positions
  /// @param[out] F  #F by 3 list of mesh triangle indices into rows of V
  ///
  template <
    typename DerivedS, 
    typename DerivedGV, 
    typename DerivedV, 
    typename DerivedF>
  IGL_INLINE void marching_cubes(
    const Eigen::MatrixBase<DerivedS> & S,
    const Eigen::MatrixBase<DerivedGV> & GV,
    const unsigned nx,
    const unsigned ny,
    const unsigned nz,
    const typename DerivedS::Scalar isovalue,
    Eigen::PlainObjectBase<DerivedV> &V,
    Eigen::PlainObjectBase<DerivedF> &F);
  /// \overload 
  /// 
  /// \brief Return edge-to-vertex map which can be used to implement
  /// batched root finding by caller (see 909_BatchMarchingCubes)
  ///
  /// @param[out] E2V  map from edge key to index into rows of V
  template <
    typename DerivedS, 
    typename DerivedGV, 
    typename DerivedV, 
    typename DerivedF>
  IGL_INLINE void marching_cubes(
    const Eigen::MatrixBase<DerivedS> & S,
    const Eigen::MatrixBase<DerivedGV> & GV,
    const unsigned nx,
    const unsigned ny,
    const unsigned nz,
    const typename DerivedS::Scalar isovalue,
    Eigen::PlainObjectBase<DerivedV> &V,
    Eigen::PlainObjectBase<DerivedF> &F,
    std::unordered_map<std::int64_t,int> &E2V);
  /// \overload
  ///
  /// \brief Sparse voxel version
  ///
  /// @param[in] S #S list of scalar field values
  /// @param[in] GV  #S by 3 list of referenced grid vertex positions
  /// @param[in] GI  #GI by 8 list of grid corner indices into rows of GV
  template <
    typename DerivedS, 
    typename DerivedGV, 
    typename DerivedGI, 
    typename DerivedV, 
    typename DerivedF>
  IGL_INLINE void marching_cubes(
    const Eigen::MatrixBase<DerivedS> & S,
    const Eigen::MatrixBase<DerivedGV> & GV,
    const Eigen::MatrixBase<DerivedGI> & GI,
    const typename DerivedS::Scalar isovalue,
    Eigen::PlainObjectBase<DerivedV> &V,
    Eigen::PlainObjectBase<DerivedF> &F);
}

#ifndef IGL_STATIC_LIBRARY
#  include "marching_cubes.cpp"
#endif

#endif
