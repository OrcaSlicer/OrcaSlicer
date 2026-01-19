// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_PROJECT_H
#define IGL_PROJECT_H
#include "igl_inline.h"
#include <Eigen/Dense>
namespace igl
{
  // Eigen reimplementation of gluProject
  // Inputs:
  //   obj*  3D objects' x, y, and z coordinates respectively
  //   model  model matrix
  //   proj  projection matrix
  //   viewport  viewport vector
  // Returns:
  //   screen space x, y, and z coordinates respectively
  template <typename Scalar>
  IGL_INLINE Eigen::Matrix<Scalar,3,1> project(
    const    Eigen::Matrix<Scalar,3,1>&  obj,
    const    Eigen::Matrix<Scalar,4,4>& model,
    const    Eigen::Matrix<Scalar,4,4>& proj,
    const    Eigen::Matrix<Scalar,4,1>&  viewport);
  // Inputs:
  //   V  #V by 3 list of object points
  //   model  model matrix
  //   proj  projection matrix
  //   viewport  viewport vector
  // Outputs:
  //   P  #V by 3 list of screen space points
  template <typename DerivedV, typename DerivedM, typename DerivedN, typename DerivedO, typename DerivedP>
  IGL_INLINE void project(
    const    Eigen::PlainObjectBase<DerivedV>&  V,
    const    Eigen::MatrixBase<DerivedM>& model,
    const    Eigen::MatrixBase<DerivedN>& proj,
    const    Eigen::MatrixBase<DerivedO>&  viewport,
    Eigen::PlainObjectBase<DerivedP> & P);
}

#ifndef IGL_STATIC_LIBRARY
#  include "project.cpp"
#endif

#endif
