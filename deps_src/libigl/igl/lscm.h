// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Daniele Panozzo <daniele.panozzo@gmail.com>
//               2015 Alec Jacobson
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_LSCM_H
#define IGL_LSCM_H
#include "igl_inline.h"

#include <Eigen/Dense>
#include <Eigen/Sparse>

namespace igl 
{
  /// Compute a Least-squares conformal map parametrization (equivalently
  /// derived in "Intrinsic Parameterizations of Surface Meshes" [Desbrun et al.
  /// 2002] and "Least Squares Conformal Maps for Automatic Texture Atlas
  /// Generation" [Lévy et al. 2002]), though this implementation follows the
  /// derivation in: "Spectral Conformal Parameterization" [Mullen et al. 2008]
  /// (note, this does **not** implement the Eigen-decomposition based method in
  /// [Mullen et al. 2008], see below). Input should be a manifold
  /// mesh (also no unreferenced vertices) and "boundary" (fixed vertices) `b`
  /// should contain at least two vertices per connected component.
  ///
  /// @param[in] V  #V by 3 list of mesh vertex positions
  /// @param[in] F  #F by 3 list of mesh faces (must be triangles)
  /// @param[in] b  #b boundary indices into V
  /// @param[in] bc #b by 2 list of boundary values
  /// @param[out] UV #V by 2 list of 2D mesh vertex positions in UV space
  /// @param[out] Q  #Vx2 by #Vx2 symmetric positive semi-definite matrix for computing LSCM energy
  /// @return true only on solver success.
  template <
    typename DerivedV, 
    typename DerivedF, 
    typename Derivedb, 
    typename Derivedbc, 
    typename DerivedV_uv, 
    typename QScalar>
  IGL_INLINE bool lscm(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    const Eigen::MatrixBase<Derivedb> & b,
    const Eigen::MatrixBase<Derivedbc> & bc,
    Eigen::PlainObjectBase<DerivedV_uv> & V_uv,
    Eigen::SparseMatrix<QScalar> & Q);
  /// \overload
  template <
    typename DerivedV, 
    typename DerivedF, 
    typename Derivedb, 
    typename Derivedbc, 
    typename DerivedV_uv>
  IGL_INLINE bool lscm(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    const Eigen::MatrixBase<Derivedb> & b,
    const Eigen::MatrixBase<Derivedbc> & bc,
    Eigen::PlainObjectBase<DerivedV_uv> & V_uv);
  /// \overload
  ///
  /// \brief Free boundary version. "Spectral Conformal Parameterization" using eigen
  /// decomposition; Assumes mesh is a single connected component topologically
  /// equivalent to a chunk of the plane.
  template <
    typename DerivedV, 
    typename DerivedF, 
    typename DerivedV_uv>
  IGL_INLINE bool lscm(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    Eigen::PlainObjectBase<DerivedV_uv> & V_uv);
}

#ifndef IGL_STATIC_LIBRARY
#  include "lscm.cpp"
#endif

#endif
