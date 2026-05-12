// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.

#ifndef IGL_READOBJ_H
#define IGL_READOBJ_H
#include "igl_inline.h"
// History:
//  return type changed from void to bool  Alec 18 Sept 2011
//  added pure vector of vectors version that has much more support Alec 31 Oct
//    2011

#ifndef IGL_NO_EIGEN
#  include <Eigen/Core>
#endif
#include <string>
#include <vector>
#include <cstdio>

namespace igl 
{
  /// Read a mesh from an ascii obj file, filling in vertex positions, normals
  /// and texture coordinates. Mesh may have faces of any number of degree
  ///
  /// @tparam Scalar  type for positions and vectors (will be read as double and cast
  ///     to Scalar)
  /// @tparam Index  type for indices (will be read as int and cast to Index)
  /// @param[in] str  path to .obj file
  /// @param[out] V  double matrix of vertex positions  #V by 3
  /// @param[out] TC  double matrix of texture coordinats #TC by 2
  /// @param[out] N  double matrix of corner normals #N by 3
  /// @param[out] F  #F list of face indices into vertex positions
  /// @param[out] FTC  #F list of face indices into vertex texture coordinates
  /// @param[out] FN  #F list of face indices into vertex normals
  //  @param[out] FM #tuple list containing (vertex index, normal index, texture coordinates index, material)
  /// @return true on success, false on errors
  template <typename Scalar, typename Index>
  IGL_INLINE bool readOBJ(
    const std::string obj_file_name, 
    std::vector<std::vector<Scalar > > & V,
    std::vector<std::vector<Scalar > > & TC,
    std::vector<std::vector<Scalar > > & N,
    std::vector<std::vector<Index > > & F,
    std::vector<std::vector<Index > > & FTC,
    std::vector<std::vector<Index > > & FN,
    std::vector<std::tuple<std::string, Index, Index >> &FM
    );
  /// \overload
  template <typename Scalar, typename Index>
  IGL_INLINE bool readOBJ(
    const std::string obj_file_name, 
    std::vector<std::vector<Scalar > > & V,
    std::vector<std::vector<Scalar > > & TC,
    std::vector<std::vector<Scalar > > & N,
    std::vector<std::vector<Index > > & F,
    std::vector<std::vector<Index > > & FTC,
    std::vector<std::vector<Index > > & FN);
  /// @param[in,out] obj_file  pointer to already opened .obj file  (will be
  /// closed)
  template <typename Scalar, typename Index>
  IGL_INLINE bool readOBJ(
    FILE * obj_file,
    std::vector<std::vector<Scalar > > & V,
    std::vector<std::vector<Scalar > > & TC,
    std::vector<std::vector<Scalar > > & N,
    std::vector<std::vector<Index > > & F,
    std::vector<std::vector<Index > > & FTC,
    std::vector<std::vector<Index > > & FN,
    std::vector<std::tuple<std::string, Index, Index >> &FM);
  /// \overload
  template <typename Scalar, typename Index>
  IGL_INLINE bool readOBJ(
    const std::string obj_file_name, 
    std::vector<std::vector<Scalar > > & V,
    std::vector<std::vector<Index > > & F);
  /// \overload
  /// \brief Eigen Wrappers. These will return true only if the data is perfectly
  /// "rectangular": All faces are the same degree, all have the same number of
  /// textures/normals etc.
  template <
    typename DerivedV, 
    typename DerivedTC, 
    typename DerivedCN, 
    typename DerivedF,
    typename DerivedFTC,
    typename DerivedFN>
  IGL_INLINE bool readOBJ(
    const std::string str,
    Eigen::PlainObjectBase<DerivedV>& V,
    Eigen::PlainObjectBase<DerivedTC>& TC,
    Eigen::PlainObjectBase<DerivedCN>& CN,
    Eigen::PlainObjectBase<DerivedF>& F,
    Eigen::PlainObjectBase<DerivedFTC>& FTC,
    Eigen::PlainObjectBase<DerivedFN>& FN);
  /// \overload
  template <typename DerivedV, typename DerivedF>
  IGL_INLINE bool readOBJ(
    const std::string str,
    Eigen::PlainObjectBase<DerivedV>& V,
    Eigen::PlainObjectBase<DerivedF>& F);
  /// \overload
  /// \brief Polygon mesh version.
  /// @param[out] I  #I vectorized list of polygon corner indices into rows of some matrix V
  /// @param[out] C  #P+1 list of cumulative polygon sizes so that C(i+1)-C(i) = size of
  ///     the ith polygon, and so I(C(i)) through I(C(i+1)-1) are the indices of
  ///     the ith polygon
  template <typename DerivedV, typename DerivedI, typename DerivedC>
  IGL_INLINE bool readOBJ(
    const std::string str,
    Eigen::PlainObjectBase<DerivedV>& V,
    Eigen::PlainObjectBase<DerivedI>& I,
    Eigen::PlainObjectBase<DerivedC>& C);

}

#ifndef IGL_STATIC_LIBRARY
#  include "readOBJ.cpp"
#endif

#endif
