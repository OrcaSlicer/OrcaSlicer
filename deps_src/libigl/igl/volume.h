// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_VOLUME_H
#define IGL_VOLUME_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  // VOLUME Compute volume for all tets of a given tet mesh
  // (V,T)
  //
  // vol = volume(V,T)
  //
  // Inputs:
  //   V  #V by dim list of vertex positions
  //   T  #V by 4 list of tet indices
  // Outputs:
  //   vol  #T list of dihedral angles (in radians)
  //
  template <
    typename DerivedV, 
    typename DerivedT, 
    typename Derivedvol>
  IGL_INLINE void volume(
    const Eigen::MatrixBase<DerivedV>& V,
    const Eigen::MatrixBase<DerivedT>& T,
    Eigen::PlainObjectBase<Derivedvol>& vol);
  template <
    typename DerivedA,
    typename DerivedB,
    typename DerivedC,
    typename DerivedD,
    typename Derivedvol>
  IGL_INLINE void volume(
    const Eigen::MatrixBase<DerivedA> & A,
    const Eigen::MatrixBase<DerivedB> & B,
    const Eigen::MatrixBase<DerivedC> & C,
    const Eigen::MatrixBase<DerivedD> & D,
    Eigen::PlainObjectBase<Derivedvol> & vol);
  // Single tet
  template <
    typename VecA,
    typename VecB,
    typename VecC,
    typename VecD>
  IGL_INLINE typename VecA::Scalar volume_single(
    const VecA & a,
    const VecB & b,
    const VecC & c,
    const VecD & d);
  // Intrinsic version:
  //
  // Inputs:
  //   L  #V by 6 list of edge lengths (see edge_lengths)
  template <
    typename DerivedL, 
    typename Derivedvol>
  IGL_INLINE void volume(
    const Eigen::MatrixBase<DerivedL>& L,
    Eigen::PlainObjectBase<Derivedvol>& vol);
}

#ifndef IGL_STATIC_LIBRARY
#  include "volume.cpp"
#endif

#endif


