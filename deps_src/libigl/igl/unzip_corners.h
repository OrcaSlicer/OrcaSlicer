// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_UNZIP_CORNERS_H
#define IGL_UNZIP_CORNERS_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <vector>
#include <functional>

namespace igl
{
  /// Given a triangle mesh where corners of each triangle index
  /// different matrices of attributes (e.g. read from an OBJ file), unzip the
  /// corners into unique efficiently: attributes become properly vertex valued
  /// (usually creating greater than #V but less than #F*3 vertices).
  ///
  /// To pass a list of attributes this function takes an std::vector of
  /// std::reference_wrapper of an Eigen::... type. This allows you to use list
  /// initializers **without** incurring a copy, but means you'll need to
  /// provide the derived type of A as an explicit template parameter:
  ///
  ///      unzip_corners<Eigen::MatrixXi>({F,FTC,FN},U,G,J);
  ///
  /// @param[in] A  #A list of #F by 3 attribute indices, typically {F,FTC,FN}
  /// @param[out] U  #U by #A list of indices into each attribute for each unique mesh
  ///               vertex: U(v,a) is the attribute index of vertex v in attribute a.
  /// @param[out] G  #F by 3 list of triangle indices into U
  /// @param[out] J  #F*3 by 1 list of indices so that A[](i,j) = U.row(i+j*#F)
  ///
  /// #### Matlibberish Example
  ///
  ///   [V,F,TC,FTC] = readOBJ('~/Downloads/kiwis/kiwi.obj');
  ///   [U,G] = unzip_corners(cat(3,F,FTC));
  ///   % display mesh
  ///   tsurf(G,V(U(:,1),:));
  ///   % display texture coordinates
  ///   tsurf(G,TC(U(:,2),:));
  ///
  template < typename DerivedA, typename DerivedU, typename DerivedG, typename DerivedJ>
  IGL_INLINE void unzip_corners(
    const std::vector<std::reference_wrapper<DerivedA> > & A,
    Eigen::PlainObjectBase<DerivedU> & U,
    Eigen::PlainObjectBase<DerivedG> & G,
    Eigen::PlainObjectBase<DerivedJ> & J);
}

#ifndef IGL_STATIC_LIBRARY
#  include "unzip_corners.cpp"
#endif
#endif
