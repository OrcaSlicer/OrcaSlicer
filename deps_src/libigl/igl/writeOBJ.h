// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_WRITEOBJ_H
#define IGL_WRITEOBJ_H
#include "igl_inline.h"
// History:
//  return type changed from void to bool  Alec 20 Sept 2011

#include <Eigen/Core>
#include <string>
#include <vector>

namespace igl 
{
  /// Write a mesh in an ascii obj file
  ///
  /// @param[in] str  path to outputfile
  /// @param[in] V  #V by 3 mesh vertex positions
  /// @param[in] F  #F by 3|4 mesh indices into V
  /// @param[in] CN #CN by 3 normal vectors
  /// @param[in] FN  #F by 3|4 corner normal indices into CN
  /// @param[in] TC  #TC by 2|3 texture coordinates
  /// @param[in] FTC #F by 3|4 corner texture coord indices into TC
  /// @return true on success, false on error
  ///
  /// \bug Horrifyingly, this does not have the same order of parameters as
  /// readOBJ.
  ///
  /// \see readOBJ
  template <
    typename DerivedV, 
    typename DerivedF,
    typename DerivedCN, 
    typename DerivedFN,
    typename DerivedTC, 
    typename DerivedFTC>
  IGL_INLINE bool writeOBJ(
    const std::string str,
    const Eigen::MatrixBase<DerivedV>& V,
    const Eigen::MatrixBase<DerivedF>& F,
    const Eigen::MatrixBase<DerivedCN>& CN,
    const Eigen::MatrixBase<DerivedFN>& FN,
    const Eigen::MatrixBase<DerivedTC>& TC,
    const Eigen::MatrixBase<DerivedFTC>& FTC);
  /// \overload
  template <typename DerivedV, typename DerivedF>
  IGL_INLINE bool writeOBJ(
    const std::string str,
    const Eigen::MatrixBase<DerivedV>& V,
    const Eigen::MatrixBase<DerivedF>& F);
  /// Write a mesh of mixed tris and quads to an ascii obj file
  ///
  /// @param[in] str  path to outputfile
  /// @param[in] V  #V by 3 mesh vertex positions
  /// @param[in] F  #F std::vector of std::vector<Index> of size 3 or 4 mesh indices into V
  /// @return true on success, false on error
  template <typename DerivedV, typename T>
  IGL_INLINE bool writeOBJ(
    const std::string &str,
    const Eigen::MatrixBase<DerivedV>& V,
    const std::vector<std::vector<T> >& F);

}

#ifndef IGL_STATIC_LIBRARY
#  include "writeOBJ.cpp"
#endif

#endif
