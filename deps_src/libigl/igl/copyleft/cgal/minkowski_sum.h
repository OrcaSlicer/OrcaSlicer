// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_CGAL_MINKOWSKI_SUM_H
#define IGL_COPYLEFT_CGAL_MINKOWSKI_SUM_H

#include "../../igl_inline.h"
#include <Eigen/Core>

namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      /// Compute the Minkowski sum of a closed triangle mesh (V,F) and a
      /// set of simplices in 3D.
      ///
      /// @param[in] VA  #VA by 3 list of mesh vertices in 3D
      /// @param[in] FA  #FA by 3 list of triangle indices into VA
      /// @param[in] VB  #VB by 3 list of mesh vertices in 3D
      /// @param[in] FB  #FB by ss list of simplex indices into VB, ss<=3
      /// @param[in] resolve_overlaps  whether or not to resolve self-union. If false
      ///     then result may contain self-intersections if input mesh is
      ///     non-convex.
      /// @param[out] W  #W by 3 list of mesh vertices in 3D
      /// @param[out] G  #G by 3 list of triangle indices into W
      /// @param[out] J  #G by 2 list of indices into 
      ///   
      template <
        typename DerivedVA,
        typename DerivedFA,
        typename DerivedVB,
        typename DerivedFB,
        typename DerivedW,
        typename DerivedG,
        typename DerivedJ>
      IGL_INLINE void minkowski_sum(
        const Eigen::MatrixBase<DerivedVA> & VA,
        const Eigen::MatrixBase<DerivedFA> & FA,
        const Eigen::MatrixBase<DerivedVB> & VB,
        const Eigen::MatrixBase<DerivedFB> & FB,
        const bool resolve_overlaps,
        Eigen::PlainObjectBase<DerivedW> & W,
        Eigen::PlainObjectBase<DerivedG> & G,
        Eigen::PlainObjectBase<DerivedJ> & J);
      /// \overload
      /// \brief Compute the Minkowski sum of a closed triangle mesh (V,F) and a
      /// segment [s,d] in 3D.
      ///
      /// @param[in] s  segment source endpoint in 3D
      /// @param[in] d  segment source endpoint in 3D
      template <
        typename DerivedVA,
        typename DerivedFA,
        typename sType, int sCols, int sOptions,
        typename dType, int dCols, int dOptions,
        typename DerivedW,
        typename DerivedG,
        typename DerivedJ>
      IGL_INLINE void minkowski_sum(
        const Eigen::MatrixBase<DerivedVA> & VA,
        const Eigen::MatrixBase<DerivedFA> & FA,
        const Eigen::Matrix<sType,1,sCols,sOptions> & s,
        const Eigen::Matrix<dType,1,dCols,dOptions> & d,
        const bool resolve_overlaps,
        Eigen::PlainObjectBase<DerivedW> & W,
        Eigen::PlainObjectBase<DerivedG> & G,
        Eigen::PlainObjectBase<DerivedJ> & J);
      /// \overload
      template <
        typename DerivedVA,
        typename DerivedFA,
        typename sType, int sCols, int sOptions,
        typename dType, int dCols, int dOptions,
        typename DerivedW,
        typename DerivedG,
        typename DerivedJ>
      IGL_INLINE void minkowski_sum(
        const Eigen::MatrixBase<DerivedVA> & VA,
        const Eigen::MatrixBase<DerivedFA> & FA,
        const Eigen::Matrix<sType,1,sCols,sOptions> & s,
        const Eigen::Matrix<dType,1,dCols,dOptions> & d,
        Eigen::PlainObjectBase<DerivedW> & W,
        Eigen::PlainObjectBase<DerivedG> & G,
        Eigen::PlainObjectBase<DerivedJ> & J);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "minkowski_sum.cpp"
#endif

#endif
