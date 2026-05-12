// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_UNPROJECT_H
#define IGL_UNPROJECT_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  /// Eigen reimplementation of gluUnproject
  ///
  /// @param[in] win  #P by 3 or 3-vector (#P=1) of screen space x, y, and z coordinates
  /// @param[in] model  4x4 model-view matrix
  /// @param[in] proj  4x4 projection matrix
  /// @param[in] viewport  4-long viewport vector
  /// @param[out] scene  #P by 3 or 3-vector (#P=1) the unprojected x, y, and z coordinates
  ///
  /// \bug The compiler will not complain if V and P are Vector3d, but the
  /// result will be incorrect.
  template <
    typename Derivedwin,
    typename Derivedmodel,
    typename Derivedproj,
    typename Derivedviewport,
    typename Derivedscene>
  IGL_INLINE void unproject(
    const Eigen::MatrixBase<Derivedwin>&  win,
    const Eigen::MatrixBase<Derivedmodel>& model,
    const Eigen::MatrixBase<Derivedproj>& proj,
    const Eigen::MatrixBase<Derivedviewport>&  viewport,
    Eigen::PlainObjectBase<Derivedscene> & scene);
  /// \overload
  template <typename Scalar>
  IGL_INLINE Eigen::Matrix<Scalar,3,1> unproject(
    const Eigen::Matrix<Scalar,3,1>&  win,
    const Eigen::Matrix<Scalar,4,4>& model,
    const Eigen::Matrix<Scalar,4,4>& proj,
    const Eigen::Matrix<Scalar,4,1>&  viewport);
}

#ifndef IGL_STATIC_LIBRARY
#  include "unproject.cpp"
#endif

#endif
