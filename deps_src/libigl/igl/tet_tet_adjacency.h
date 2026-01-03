// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2018 Oded Stein <oded.stein@columbia.edu>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#ifndef IGL_TET_TET_ADJACENCY_H
#define IGL_TET_TET_ADJACENCY_H

#include <Eigen/Core>

#include "igl_inline.h"

namespace igl
{
  /// Constructs the tet_tet adjacency matrix for a given tet mesh with tets T
  ///
  /// @param[in] T  #T by 4 list of tets
  /// @param[out] TT   #T by #4 adjacency matrix, the element i,j is the id of
  ///   the tet adjacent to the j face of tet i
  /// @param[out] TTi  #T by #4 adjacency matrix, the element i,j is the id of
  ///   face of the tet TT(i,j) that is adjacent to tet i
  ///
  /// \note the first face of a tet is [0,1,2], the second [0,1,3], the third
  /// [1,2,3], and the fourth [2,0,3].
  template <typename DerivedT, typename DerivedTT, typename DerivedTTi>
  IGL_INLINE void tet_tet_adjacency(
    const Eigen::MatrixBase<DerivedT>& T,
    Eigen::PlainObjectBase<DerivedTT>& TT,
    Eigen::PlainObjectBase<DerivedTTi>& TTi);
  /// \overload
  template <typename DerivedT, typename DerivedTT>
  IGL_INLINE void tet_tet_adjacency(
    const Eigen::MatrixBase<DerivedT>& T,
    Eigen::PlainObjectBase<DerivedTT>& TT);
}

#ifndef IGL_STATIC_LIBRARY
#  include "tet_tet_adjacency.cpp"
#endif


#endif
