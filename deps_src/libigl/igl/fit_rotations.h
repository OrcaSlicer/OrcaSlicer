// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_FIT_ROTATIONS_H
#define IGL_FIT_ROTATIONS_H
#include "igl_inline.h"
#include <Eigen/Core>

namespace igl
{
  // Known issues: This seems to be implemented in Eigen/Geometry:
  // Eigen::umeyama
  //
  // FIT_ROTATIONS Given an input mesh and new positions find rotations for
  // every covariance matrix in a stack of covariance matrices
  // 
  // Inputs:
  //   S  nr*dim by dim stack of covariance matrices
  //   single_precision  whether to use single precision (faster)
  // Outputs:
  //   R  dim by dim * nr list of rotations
  //
  template <typename DerivedS, typename DerivedD>
  IGL_INLINE void fit_rotations(
    const Eigen::PlainObjectBase<DerivedS> & S,
    const bool single_precision,
          Eigen::PlainObjectBase<DerivedD> & R);
  
  // FIT_ROTATIONS Given an input mesh and new positions find 2D rotations for
  // every vertex that best maps its one ring to the new one ring
  // 
  // Inputs:
  //   S  nr*dim by dim stack of covariance matrices, third column and every
  //   third row will be ignored
  // Outputs:
  //   R  dim by dim * nr list of rotations, third row and third column of each
  //   rotation will just be identity
  //
  template <typename DerivedS, typename DerivedD>
  IGL_INLINE void fit_rotations_planar(
    const Eigen::PlainObjectBase<DerivedS> & S,
          Eigen::PlainObjectBase<DerivedD> & R);
#ifdef __SSE__
  IGL_INLINE void fit_rotations_SSE( const Eigen::MatrixXf & S, Eigen::MatrixXf & R);
  IGL_INLINE void fit_rotations_SSE( const Eigen::MatrixXd & S, Eigen::MatrixXd & R);
#endif
#ifdef __AVX__
  IGL_INLINE void fit_rotations_AVX( const Eigen::MatrixXf & S, Eigen::MatrixXf & R);
#endif
}

#ifndef IGL_STATIC_LIBRARY
#  include "fit_rotations.cpp"
#endif

#endif
