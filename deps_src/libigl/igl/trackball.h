// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_TRACKBALL_H
#define IGL_TRACKBALL_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <Eigen/Geometry>

namespace igl
{
  /// Applies a trackball drag to identity
  ///
  /// @param[in] w  width of the trackball context
  /// @param[in] h  height of the trackball context
  /// @param[in] speed_factor  controls how fast the trackball feels, 1 is normal
  /// @param[in] down_mouse_x  x position of mouse down
  /// @param[in] down_mouse_y  y position of mouse down
  /// @param[in] mouse_x  current x position of mouse
  /// @param[in] mouse_y  current y position of mouse
  /// @param[out] quat  the resulting rotation (as quaternion)
  template <typename Q_type>
  IGL_INLINE void trackball(
    const double w,
    const double h,
    const Q_type speed_factor,
    const double down_mouse_x,
    const double down_mouse_y,
    const double mouse_x,
    const double mouse_y,
    Q_type * quat);
  /// Applies a trackball drag to a given rotation
  ///
  /// @param[in] w  width of the trackball context
  /// @param[in] h  height of the trackball context
  /// @param[in] speed_factor  controls how fast the trackball feels, 1 is normal
  /// @param[in] down_quat  rotation at mouse down, i.e. the rotation we're applying the
  ///              trackball motion to (as quaternion)
  /// @param[in] down_mouse_x  x position of mouse down
  /// @param[in] down_mouse_y  y position of mouse down
  /// @param[in] mouse_x  current x position of mouse
  /// @param[in] mouse_y  current y position of mouse
  /// @param[out] quat  the resulting rotation (as quaternion)
  template <typename Q_type>
  IGL_INLINE void trackball(
    const double w,
    const double h,
    const Q_type speed_factor,
    const Q_type * down_quat,
    const double down_mouse_x,
    const double down_mouse_y,
    const double mouse_x,
    const double mouse_y,
    Q_type * quat);
  /// \overload
  template <typename Scalardown_quat, typename Scalarquat>
  IGL_INLINE void trackball(
    const double w,
    const double h,
    const double speed_factor,
    const Eigen::Quaternion<Scalardown_quat> & down_quat,
    const double down_mouse_x,
    const double down_mouse_y,
    const double mouse_x,
    const double mouse_y,
    Eigen::Quaternion<Scalarquat> & quat);
}

#ifndef IGL_STATIC_LIBRARY
#  include "trackball.cpp"
#endif

#endif
