// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_BBW_H
#define IGL_BBW_H
#include "igl_inline.h"

#include <Eigen/Dense>
#include <igl/active_set.h>

namespace igl
{
  // Container for BBW computation related data and flags
  class BBWData
  {
    public:
      // Enforce partition of unity during optimization (optimize all weight
      // simultaneously)
      bool partition_unity;
      // Initial guess
      Eigen::MatrixXd W0;
      igl::active_set_params active_set_params;
      // Verbosity level
      // 0: quiet
      // 1: loud
      // 2: louder
      int verbosity;
    public:
      IGL_INLINE BBWData();
      // Print current state of object
      IGL_INLINE void print();
  };

  // Compute Bounded Biharmonic Weights on a given domain (V,Ele) with a given
  // set of boundary conditions
  //
  // Templates
  //   DerivedV  derived type of eigen matrix for V (e.g. MatrixXd)
  //   DerivedF  derived type of eigen matrix for F (e.g. MatrixXi)
  //   Derivedb  derived type of eigen matrix for b (e.g. VectorXi)
  //   Derivedbc  derived type of eigen matrix for bc (e.g. MatrixXd)
  //   DerivedW  derived type of eigen matrix for W (e.g. MatrixXd)
  // Inputs:
  //   V  #V by dim vertex positions
  //   Ele  #Elements by simplex-size list of element indices
  //   b  #b boundary indices into V
  //   bc #b by #W list of boundary values
  //   data  object containing options, initial guess --> solution and results
  // Outputs:
  //   W  #V by #W list of *unnormalized* weights to normalize use
  //    igl::normalize_row_sums(W,W);
  // Returns true on success, false on failure
  template <
    typename DerivedV,
    typename DerivedEle,
    typename Derivedb,
    typename Derivedbc,
    typename DerivedW>
  IGL_INLINE bool bbw(
    const Eigen::PlainObjectBase<DerivedV> & V,
    const Eigen::PlainObjectBase<DerivedEle> & Ele,
    const Eigen::PlainObjectBase<Derivedb> & b,
    const Eigen::PlainObjectBase<Derivedbc> & bc,
    BBWData & data,
    Eigen::PlainObjectBase<DerivedW> & W);
}

#ifndef IGL_STATIC_LIBRARY
#  include "bbw.cpp"
#endif

#endif

