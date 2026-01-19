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
  // Construct a ray (source point + direction vector) given a screen space
  // positions (e.g. mouse) and a model-view projection constellation.
  //
  // Inputs:
  //   pos  2d screen-space position (x,y) 
  //   model  4x4 model-view matrix
  //   proj  4x4 projection matrix
  //   viewport  4-long viewport vector
  // Outputs:
  //   s  source of ray (pos unprojected with z=0)
  ///  dir  direction of ray (d - s) where d is pos unprojected with z=1
  // 
  template <
    typename Derivedpos,
    typename Derivedmodel,
    typename Derivedproj,
    typename Derivedviewport,
    typename Deriveds,
    typename Deriveddir>
  IGL_INLINE void unproject_ray(
    const Eigen::PlainObjectBase<Derivedpos> & pos,
    const Eigen::PlainObjectBase<Derivedmodel> & model,
    const Eigen::PlainObjectBase<Derivedproj> & proj,
    const Eigen::PlainObjectBase<Derivedviewport> & viewport,
    Eigen::PlainObjectBase<Deriveds> & s,
    Eigen::PlainObjectBase<Deriveddir> & dir);
}
#ifndef IGL_STATIC_LIBRARY
#  include "unproject_ray.cpp"
#endif
#endif
