// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2023 Alec Jacobson
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_SPECTRA_LSCM_H
#define IGL_SPECTRA_LSCM_H
#include "../igl_inline.h"

#include <Eigen/Dense>
#include <Eigen/Sparse>

namespace igl 
{
namespace spectra
{
  /// Compute a free-boundary least-squares conformal map parametrization. Equivalently
  /// derived in "Intrinsic Parameterizations of Surface Meshes" [Desbrun et al.
  /// 2002] and "Least Squares Conformal Maps for Automatic Texture Atlas
  /// Generation" [Lévy et al. 2002], though this implementation follows the
  /// derivation in: "Spectral Conformal Parameterization" [Mullen et al. 2008]
  /// Free boundary version. "Spectral Conformal Parameterization" using Eigen
  /// decomposition. Assumes mesh is a single connected component topologically
  /// equivalent to a chunk of the plane.
  ///
  ///
  /// @param[in] V  #V by 3 list of mesh vertex positions
  /// @param[in] F  #F by 3 list of mesh faces (must be triangles)
  /// @param[out] UV #V by 2 list of 2D mesh vertex positions in UV space
  /// @return true only on solver success.
  ///
  template <
    typename DerivedV, 
    typename DerivedF, 
    typename DerivedV_uv>
  IGL_INLINE bool lscm(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    Eigen::PlainObjectBase<DerivedV_uv> & V_uv);
}
}

#ifndef IGL_STATIC_LIBRARY
#  include "lscm.cpp"
#endif

#endif

