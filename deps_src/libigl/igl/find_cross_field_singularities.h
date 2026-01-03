// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Daniele Panozzo <daniele.panozzo@gmail.com>, Olga Diamanti <olga.diam@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#ifndef IGL_FIND_CROSS_FIELD_SINGULARITIES_H
#define IGL_FIND_CROSS_FIELD_SINGULARITIES_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  /// Computes singularities of a cross field, assumed combed
  ///
  /// @param[in] V                #V by 3 eigen Matrix of mesh vertex 3D positions
  /// @param[in] F                #F by 3 eigen Matrix of face (quad) indices
  /// @param[in] mismatch         #F by 3 eigen Matrix containing the integer mismatch of the cross field
  ///                    across all face edges
  /// @param[out] isSingularity    #V by 1 boolean eigen Vector indicating the presence of a singularity on a vertex
  /// @param[out] singularityIndex #V by 1 integer eigen Vector containing the singularity indices
  ///
  template <typename DerivedV, typename DerivedF, typename DerivedM, typename DerivedO>
  IGL_INLINE void find_cross_field_singularities(const Eigen::MatrixBase<DerivedV> &V,
                                                 const Eigen::MatrixBase<DerivedF> &F,
                                                 const Eigen::MatrixBase<DerivedM> &mismatch,
                                                 Eigen::PlainObjectBase<DerivedO> &isSingularity,
                                                 Eigen::PlainObjectBase<DerivedO> &singularityIndex);
  /// \overload
  ///
  /// \brief Wrapper that calculates the mismatch if it is not provided.
  ///
  /// @param[in] PD1              #F by 3 eigen Matrix of the first per face cross field vector
  /// @param[in] PD2              #F by 3 eigen Matrix of the second per face  cross field vector
  /// @param[in] isCombed        boolean indicating whether the cross field is combed
  ///
  /// \note the field in PD1 and PD2 MUST BE combed (see igl::comb_cross_field).
  template <typename DerivedV, typename DerivedF, typename DerivedO>
  IGL_INLINE void find_cross_field_singularities(const Eigen::MatrixBase<DerivedV> &V,
                                                 const Eigen::MatrixBase<DerivedF> &F,
                                                 const Eigen::MatrixBase<DerivedV> &PD1,
                                                 const Eigen::MatrixBase<DerivedV> &PD2,
                                                 Eigen::PlainObjectBase<DerivedO> &isSingularity,
                                                 Eigen::PlainObjectBase<DerivedO> &singularityIndex,
                                                 bool isCombed = false);
}
#ifndef IGL_STATIC_LIBRARY
#include "find_cross_field_singularities.cpp"
#endif

#endif
