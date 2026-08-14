// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_RAY_SPHERE_INTERSECT_H
#define IGL_RAY_SPHERE_INTERSECT_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  /// Compute the intersection between a ray from O in direction D and a sphere
  /// centered at C with radius r
  ///
  /// @param[in] o  origin of ray
  /// @param[in] d  direction of ray
  /// @param[in] c  center of sphere
  /// @param[in] r  radius of sphere
  /// @param[out] t0  parameterization of first hit (set only if exists) so that
  ///   hit position = o + t0*d
  /// @param[out] t1  parameterization of second hit (set only if exists)
  /// @return the number of hits
  template <
    typename Derivedo,
    typename Derivedd,
    typename Derivedc, 
    typename r_type, 
    typename t_type>
  IGL_INLINE int ray_sphere_intersect(
    const Eigen::MatrixBase<Derivedo> & o,
    const Eigen::MatrixBase<Derivedd> & d,
    const Eigen::MatrixBase<Derivedc> & c,
    r_type r, 
    t_type & t0,
    t_type & t1);
}
#ifndef IGL_STATIC_LIBRARY
#include "ray_sphere_intersect.cpp"
#endif
#endif

