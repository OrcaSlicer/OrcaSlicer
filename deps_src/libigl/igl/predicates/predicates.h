// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2019 Qingnan Zhou <qnzhou@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#pragma once
#ifndef IGL_PREDICATES_PREDICATES_H
#define IGL_PREDICATES_PREDICATES_H

#include "../igl_inline.h"
#include <Eigen/Core>

namespace igl {
  namespace predicates {
    /// Types of orientations and other predicate results.
    ///
    /// \fileinfo
    enum class Orientation {
      POSITIVE=1, INSIDE=1,
      NEGATIVE=-1, OUTSIDE=-1,
      COLLINEAR=0, COPLANAR=0, COCIRCULAR=0, COSPHERICAL=0, DEGENERATE=0
    };

    /// Initialize internal variable used by predciates. Must be called before
    /// using exact predicates. It is safe to call this function from multiple
    /// threads.
    ///
    /// \fileinfo
    IGL_INLINE void exactinit();

    /// Compute the orientation of the triangle formed by pa, pb, pc.
    ///
    /// @param[in] pa  2D point on line
    /// @param[in] pb  2D point on line
    /// @param[in] pc  2D query point.
    /// @return POSITIVE if pa, pb, pc are counterclockwise oriented.
    ///          NEGATIVE if they are clockwise oriented.
    ///          COLLINEAR if they are collinear.
    ///
    /// \fileinfo
    template<typename Vector2D>
    IGL_INLINE Orientation orient2d(
        const Eigen::MatrixBase<Vector2D>& pa,
        const Eigen::MatrixBase<Vector2D>& pb,
        const Eigen::MatrixBase<Vector2D>& pc);
    /// Compute the orientation of the tetrahedron formed by pa, pb, pc, pd.
    ///
    /// @param[in] pa  2D point on plane
    /// @param[in] pb  2D point on plane
    /// @param[in] pc  2D point on plane
    /// @param[in] pd  2D query point 
    ///  @return POSITIVE if pd is "below" the oriented plane formed by pa, pb and pc.
    ///          NEGATIVE if pd is "above" the plane.
    ///          COPLANAR if pd is on the plane.
    ///
    /// \fileinfo
    template<typename Vector3D>
    IGL_INLINE Orientation orient3d(
        const Eigen::MatrixBase<Vector3D>& pa,
        const Eigen::MatrixBase<Vector3D>& pb,
        const Eigen::MatrixBase<Vector3D>& pc,
        const Eigen::MatrixBase<Vector3D>& pd);
    /// Decide whether a point is inside/outside/on a circle.
    ///
    /// @param[in] pa  2D point on circle
    /// @param[in] pb  2D point on circle
    /// @param[in] pc  2D point on circle
    /// @param[in] pd  2D point query
    /// @return INSIDE if pd is inside of the circle defined by pa, pb and pc.
    ///          OUSIDE if pd is outside of the circle.
    ///          COCIRCULAR pd is exactly on the circle.
    ///
    /// \fileinfo
    template<typename Vector2D>
    IGL_INLINE Orientation incircle(
        const Eigen::MatrixBase<Vector2D>& pa,
        const Eigen::MatrixBase<Vector2D>& pb,
        const Eigen::MatrixBase<Vector2D>& pc,
        const Eigen::MatrixBase<Vector2D>& pd);
    /// Decide whether a point is inside/outside/on a sphere.
    ///
    /// @param[in] pa  2D point on sphere
    /// @param[in] pb  2D point on sphere
    /// @param[in] pc  2D point on sphere
    /// @param[in] pd  2D point on sphere
    /// @param[in] pe  2D point query
    /// @return INSIDE if pe is inside of the sphere defined by pa, pb, pc and pd.
    ///          OUSIDE if pe is outside of the sphere.
    ///          COSPHERICAL pd is exactly on the sphere.
    ///
    /// \fileinfo
    template<typename Vector3D>
    IGL_INLINE Orientation insphere(
        const Eigen::MatrixBase<Vector3D>& pa,
        const Eigen::MatrixBase<Vector3D>& pb,
        const Eigen::MatrixBase<Vector3D>& pc,
        const Eigen::MatrixBase<Vector3D>& pd,
        const Eigen::MatrixBase<Vector3D>& pe);
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "predicates.cpp"
#endif

#endif
