// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2021 Alec Jacobson <alecjacobson@gmail.com>
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_PER_CORNER_NORMALS_H
#define IGL_PER_CORNER_NORMALS_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <vector>

namespace igl
{
  /// Compute per corner normals for a triangle mesh by computing the
  /// area-weighted average of normals at incident faces whose normals deviate
  /// less than the provided threshold.
  ///
  /// @param[in] V  #V by 3 list of vertex positions
  /// @param[in] F  #F by 3 list of mesh triangle indices into V
  /// @param[in] corner_threshold_degrees  threshold in degrees on sharp angles
  /// @param[out] CN  #F*3 by 3 list of mesh vertex 3D normals, where the normal
  ///     for corner F(i,j) is at CN.row(i*3+j)
  template <
    typename DerivedV,
    typename DerivedF,
    typename DerivedCN>
  IGL_INLINE void per_corner_normals(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    const typename DerivedV::Scalar corner_threshold_degrees,
    Eigen::PlainObjectBase<DerivedCN> & CN);
  /// \overload
  /// @param[in] VF  3*#F list  List of faces indice on each vertex, so that VF(NI(i)+j) =
  ///     f, means that face f is the jth face (in no particular order) incident
  ///     on vertex i.
  /// @param[in] NI  #V+1 list  cumulative sum of vertex-triangle degrees with a
  ///     preceeding zero. "How many faces" have been seen before visiting this
  ///     vertex and its incident faces.
  ///
  /// \see vertex_triangle_adjacency
  template <
    typename DerivedV,
    typename DerivedF,
    typename DerivedVF,
    typename DerivedNI,
    typename DerivedCN>
  IGL_INLINE void per_corner_normals(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    const typename DerivedV::Scalar corner_threshold_degrees,
    const Eigen::MatrixBase<DerivedVF> & VF,
    const Eigen::MatrixBase<DerivedNI> & NI,
    Eigen::PlainObjectBase<DerivedCN> & CN);
  /// @param[in] CI  #CI list of face neighbors as indices into rows of F
  /// @param[in] CC  3*#F+1 list of cumulative sizes so that CC(i*3+j+1) - CC(i*3+j) is
  ///     the number of faces considered smoothly incident on corner at F(i,j)
  ///
  /// \see smooth_corner_adjacency
  template <
    typename DerivedV,
    typename DerivedF,
    typename DerivedCI,
    typename DerivedCC,
    typename DerivedCN>
  IGL_INLINE void per_corner_normals(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    const Eigen::MatrixBase<DerivedCI> & CI,
    const Eigen::MatrixBase<DerivedCC> & CC,
    Eigen::PlainObjectBase<DerivedCN> & CN);
  /// Given indexed normals (e.g., read from a .obj file), explode into
  /// per-corner normals (e.g., as expected by igl::opengl::ViewerData)
  ///
  /// @param[in] NV  #NV by 3 list of index normal vectors
  /// @param[in] NF  #F by nc list of indices into rows of NV
  /// @param[out] CN  #F*nc by 3 list of per-corner normals so that 
  ///     CN.row(i*nc+c) = NV.row(NF(i,c))
  template <typename DerivedNV, typename DerivedNF, typename DerivedCN>
  IGL_INLINE void per_corner_normals(
    const Eigen::MatrixBase<DerivedNV> & NV,
    const Eigen::MatrixBase<DerivedNF> & NF,
    Eigen::PlainObjectBase<DerivedCN> & CN);
  /// Per-corner normals for a polygon mesh
  ///
  /// @param[in] V  #V by 3 list of mesh vertex positions
  /// @param[in] I  #I vectorized list of polygon corner indices into rows of some matrix V
  /// @param[in] C  #polygons+1 list of cumulative polygon sizes so that C(i+1)-C(i) = size of
  ///              the ith polygon, and so I(C(i)) through I(C(i+1)-1) are the indices of
  ///              the ith polygon
  /// @param[in] corner_threshold  threshold in degrees on sharp angles
  /// @param[out] N  #I by 3 list of per corner normals
  /// @param[out] VV  #I+#polygons by 3 list of auxiliary triangle mesh vertex positions
  /// @param[out] FF  #I by 3 list of triangle indices into rows of VV
  /// @param[out] J  #I list of indices into original polygons
  /// @param[out] NN  #FF by 3 list of normals for each auxiliary triangle
  template <
    typename DerivedV, 
    typename DerivedI, 
    typename DerivedC, 
    typename DerivedN,
    typename DerivedVV,
    typename DerivedFF,
    typename DerivedJ,
    typename DerivedNN>
  IGL_INLINE void per_corner_normals(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedI> & I,
    const Eigen::MatrixBase<DerivedC> & C,
    const typename DerivedV::Scalar corner_threshold_degrees,
    Eigen::PlainObjectBase<DerivedN>  & N,
    Eigen::PlainObjectBase<DerivedVV> & VV,
    Eigen::PlainObjectBase<DerivedFF> & FF,
    Eigen::PlainObjectBase<DerivedJ>  & J,
    Eigen::PlainObjectBase<DerivedNN> & NN);
}

#ifndef IGL_STATIC_LIBRARY
#  include "per_corner_normals.cpp"
#endif

#endif
