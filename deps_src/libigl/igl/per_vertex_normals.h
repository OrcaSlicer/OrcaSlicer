// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_PER_VERTEX_NORMALS_H
#define IGL_PER_VERTEX_NORMALS_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  /// Weighting schemes for computing per-vertex normals
  ///
  /// \note It would be nice to support more or all of the methods here:
  /// "A comparison of algorithms for vertex normal computation"
  enum PerVertexNormalsWeightingType
  {
    /// Incident face normals have uniform influence on vertex normal
    PER_VERTEX_NORMALS_WEIGHTING_TYPE_UNIFORM = 0,
    /// Incident face normals are averaged weighted by area
    PER_VERTEX_NORMALS_WEIGHTING_TYPE_AREA = 1,
    /// Incident face normals are averaged weighted by incident angle of vertex
    PER_VERTEX_NORMALS_WEIGHTING_TYPE_ANGLE = 2,
    /// Area weights
    PER_VERTEX_NORMALS_WEIGHTING_TYPE_DEFAULT = 3,
    /// Total number of weighting types
    NUM_PER_VERTEX_NORMALS_WEIGHTING_TYPE = 4
  };
  /// Compute vertex normals via vertex position list, face list
  ///
  /// @param[in] V  #V by 3 eigen Matrix of mesh vertex 3D positions
  /// @param[in] F  #F by 3 eigen Matrix of face (triangle) indices
  /// @param[in] weighting  Weighting type
  /// @param[out] N  #V by 3 eigen Matrix of mesh vertex 3D normals
  template <
    typename DerivedV, 
    typename DerivedF,
    typename DerivedN>
  IGL_INLINE void per_vertex_normals(
    const Eigen::MatrixBase<DerivedV>& V,
    const Eigen::MatrixBase<DerivedF>& F,
    const igl::PerVertexNormalsWeightingType weighting,
    Eigen::PlainObjectBase<DerivedN> & N);
  /// \overload
  template <
    typename DerivedV, 
    typename DerivedF,
    typename DerivedN>
  IGL_INLINE void per_vertex_normals(
    const Eigen::MatrixBase<DerivedV>& V,
    const Eigen::MatrixBase<DerivedF>& F,
    Eigen::PlainObjectBase<DerivedN> & N);
  /// \overload
  ///
  /// @param[in] FN  #F by 3 matrix of face (triangle) normals
  template <typename DerivedV, typename DerivedF, typename DerivedFN, typename DerivedN>
  IGL_INLINE void per_vertex_normals(
    const Eigen::MatrixBase<DerivedV>& V,
    const Eigen::MatrixBase<DerivedF>& F,
    const PerVertexNormalsWeightingType weighting,
    const Eigen::MatrixBase<DerivedFN>& FN,
    Eigen::PlainObjectBase<DerivedN> & N);
  /// \overload
  template <
    typename DerivedV, 
    typename DerivedF,
    typename DerivedFN,
    typename DerivedN>
  IGL_INLINE void per_vertex_normals(
    const Eigen::MatrixBase<DerivedV>& V,
    const Eigen::MatrixBase<DerivedF>& F,
    const Eigen::MatrixBase<DerivedFN>& FN,
    Eigen::PlainObjectBase<DerivedN> & N);
}

#ifndef IGL_STATIC_LIBRARY
#  include "per_vertex_normals.cpp"
#endif

#endif
