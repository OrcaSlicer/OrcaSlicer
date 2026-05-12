// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_UNPROJECT_RAY_H
#define IGL_UNPROJECT_RAY_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  /// Construct a ray (source point + direction vector) given a screen space
  /// positions (e.g. mouse) and a model-view projection constellation.
  ///
  /// @param[in] pos  2d screen-space position (x,y) 
  /// @param[in] model  4x4 model-view matrix
  /// @param[in] proj  4x4 projection matrix
  /// @param[in] viewport  4-long viewport vector
  /// @param[out] s  source of ray (pos unprojected with z=0)
  /// @param[out] dir  direction of ray (d - s) where d is pos unprojected with z=1
  /// 
  template <
    typename Derivedpos,
    typename Derivedmodel,
    typename Derivedproj,
    typename Derivedviewport,
    typename Deriveds,
    typename Deriveddir>
  IGL_INLINE void unproject_ray(
    const Eigen::MatrixBase<Derivedpos> & pos,
    const Eigen::MatrixBase<Derivedmodel> & model,
    const Eigen::MatrixBase<Derivedproj> & proj,
    const Eigen::MatrixBase<Derivedviewport> & viewport,
    Eigen::PlainObjectBase<Deriveds> & s,
    Eigen::PlainObjectBase<Deriveddir> & dir);
}
#ifndef IGL_STATIC_LIBRARY
#  include "unproject_ray.cpp"
#endif
#endif
