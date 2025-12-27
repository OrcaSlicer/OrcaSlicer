// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_LAUNCH_MEDIT_H
#define IGL_LAUNCH_MEDIT_H
#include "igl_inline.h"

#include <Eigen/Core>

namespace igl 
{
  // Writes the tetmesh in (V,T,F) to a temporary file, opens it with medit
  // (forking with a system call) and returns
  //
  //
  // Templates:
  //   DerivedV  real-value: i.e. from MatrixXd
  //   DerivedT  integer-value: i.e. from MatrixXi
  //   DerivedF  integer-value: i.e. from MatrixXi
  // Inputs:
  //   V  double matrix of vertex positions  #V by 3
  //   T  #T list of tet indices into vertex positions
  //   F  #F list of face indices into vertex positions
  //   wait  whether to wait for medit process to finish before returning
  // Returns returned value of system call (probably not useful if wait=false
  // because of the fork)
  template <typename DerivedV, typename DerivedT, typename DerivedF>
  IGL_INLINE int launch_medit(
    const Eigen::PlainObjectBase<DerivedV> & V, 
    const Eigen::PlainObjectBase<DerivedT> & T,
    const Eigen::PlainObjectBase<DerivedF> & F,
    const bool wait);
}

#ifndef IGL_STATIC_LIBRARY
#  include "launch_medit.cpp"
#endif

#endif

