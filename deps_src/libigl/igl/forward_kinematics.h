// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_FORWARD_KINEMATICS_H
#define IGL_FORWARD_KINEMATICS_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <Eigen/Geometry>
#include <Eigen/StdVector>
#include <vector>

namespace igl
{
  /// Given a skeleton and a set of relative bone rotations compute absolute
  /// rigid transformations for each bone.
  ///
  /// @param[in] C  #C by dim list of joint positions
  /// @param[in] BE  #BE by 2 list of bone edge indices
  /// @param[in] P  #BE list of parent indices into BE
  /// @param[in] dQ  #BE list of relative rotations
  /// @param[in] dT  #BE list of relative translations
  /// @param[out] vQ  #BE list of absolute rotations
  /// @param[out] vT  #BE list of absolute translations
  IGL_INLINE void forward_kinematics(
    const Eigen::MatrixXd & C,
    const Eigen::MatrixXi & BE,
    const Eigen::VectorXi & P,
    const std::vector<
      Eigen::Quaterniond,Eigen::aligned_allocator<Eigen::Quaterniond> > & dQ,
    const std::vector<Eigen::Vector3d> & dT,
    std::vector<
      Eigen::Quaterniond,Eigen::aligned_allocator<Eigen::Quaterniond> > & vQ,
    std::vector<Eigen::Vector3d> & vT);
  /// \overload
  /// \brief assuming each dT[i] == {0,0,0}
  IGL_INLINE void forward_kinematics(
    const Eigen::MatrixXd & C,
    const Eigen::MatrixXi & BE,
    const Eigen::VectorXi & P,
    const std::vector<
      Eigen::Quaterniond,Eigen::aligned_allocator<Eigen::Quaterniond> > & dQ,
    std::vector<
      Eigen::Quaterniond,Eigen::aligned_allocator<Eigen::Quaterniond> > & vQ,
    std::vector<Eigen::Vector3d> & vT);
  /// \overload
  /// @param[out] T  #BE*(dim+1) by dim stack of transposed transformation matrices
  IGL_INLINE void forward_kinematics(
    const Eigen::MatrixXd & C,
    const Eigen::MatrixXi & BE,
    const Eigen::VectorXi & P,
    const std::vector<
      Eigen::Quaterniond,Eigen::aligned_allocator<Eigen::Quaterniond> > & dQ,
    const std::vector<Eigen::Vector3d> & dT,
    Eigen::MatrixXd & T);
  /// \overload
  IGL_INLINE void forward_kinematics(
    const Eigen::MatrixXd & C,
    const Eigen::MatrixXi & BE,
    const Eigen::VectorXi & P,
    const std::vector<
      Eigen::Quaterniond,Eigen::aligned_allocator<Eigen::Quaterniond> > & dQ,
    Eigen::MatrixXd & T);
};

#ifndef IGL_STATIC_LIBRARY
#  include "forward_kinematics.cpp"
#endif
#endif
