// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2019 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef RIGID_ALIGNMENT_H
#define RIGID_ALIGNMENT_H
#include "igl_inline.h"
#include <Eigen/Core>

namespace igl
{
  /// Find the rigid transformation that best aligns the 3D points X to their
  /// corresponding points P with associated normals N.
  ///
  ///      min       ‖(X*R+t-P)'N‖²
  ///      R∈SO(3)
  ///      t∈R³
  ///
  /// @param[in] X  #X by 3 list of query points
  /// @param[in] P  #X by 3 list of corresponding (e.g., closest) points
  /// @param[in] N  #X by 3 list of unit normals for each row in P
  /// @param[out] R  3 by 3 rotation matrix
  /// @param[out] t  1 by 3 translation vector
  ///
  /// \see icp
  template <
    typename DerivedX,
    typename DerivedP,
    typename DerivedN,
    typename DerivedR,
    typename Derivedt
  >
  IGL_INLINE void rigid_alignment(
    const Eigen::MatrixBase<DerivedX> & X,
    const Eigen::MatrixBase<DerivedP> & P,
    const Eigen::MatrixBase<DerivedN> & N,
    Eigen::PlainObjectBase<DerivedR> & R,
    Eigen::PlainObjectBase<Derivedt> & t);
}

#ifndef IGL_STATIC_LIBRARY
#  include "rigid_alignment.cpp"
#endif

#endif 
