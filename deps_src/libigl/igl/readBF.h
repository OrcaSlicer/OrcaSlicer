// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_READBF_H
#define IGL_READBF_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <string>
/// @file readBF.h
///
/// A .bf file contains a “bone forest”. Normally a skeleton for linear blend
/// skinning is a “bone tree” with a single root. But this format may store
/// multiple trees, hence a forest.
///
/// Each line contains data about a vertex (joint) of the bone forest:
///
///     [weight index] [parent index] [x] [y] [z] [undocument optional data]
///
/// Indices begin with 0. The weight index is -1 if the bone does not have an
/// associated weight. The parent index is -1 for root nodes. The x,y,z
/// coordinates are offset vectors from this joint’s parent’s location (for
/// roots, an offset from the origin).
namespace igl
{
  /// Read a bones forest from a file, returns a list of bone roots
  ///
  /// @param[in] file_name  path to .bf bones tree file
  /// @param[out] WI  #B list of unique weight indices
  /// @param[out] P  #B list of parent indices into B, -1 for roots
  /// @param[out] O  #B by 3 list of tip offset vectors from parent (or position for roots)
  /// @return true on success, false on errors
  template < 
    typename DerivedWI,
    typename DerivedP,
    typename DerivedO>
  IGL_INLINE bool readBF(
    const std::string & filename,
    Eigen::PlainObjectBase<DerivedWI> & WI,
    Eigen::PlainObjectBase<DerivedP> & P,
    Eigen::PlainObjectBase<DerivedO> & O);
  /// Read bone forest into pure bone-skeleton format, expects only bones (no
  /// point handles), and that a root in the .bf <---> no weight attachment.
  ///
  /// @param[in] file_name  path to .bf bones tree file
  /// @param[out] WI  #B list of unique weight indices
  /// @param[out] P  #B list of parent indices into B, -1 for roots
  /// @param[out] O  #B by 3 list of tip offset vectors from parent (or position for roots)
  /// @param[out] C  #C by 3 list of absolute joint locations
  /// @param[out] BE  #BE by 3 list of bone indices into C, in order of weight index
  /// @param[out] P  #BE list of parent bone indices into BE, -1 means root bone
  /// @return true on success, false on errors
  ///   
  /// \see readTGF, bone_parents, forward_kinematics
  template < 
    typename DerivedWI,
    typename DerivedbfP,
    typename DerivedO,
    typename DerivedC,
    typename DerivedBE,
    typename DerivedP>
  IGL_INLINE bool readBF(
    const std::string & filename,
    Eigen::PlainObjectBase<DerivedWI> & WI,
    Eigen::PlainObjectBase<DerivedbfP> & bfP,
    Eigen::PlainObjectBase<DerivedO> & O,
    Eigen::PlainObjectBase<DerivedC> & C,
    Eigen::PlainObjectBase<DerivedBE> & BE,
    Eigen::PlainObjectBase<DerivedP> & P);
}

#ifndef IGL_STATIC_LIBRARY
#  include "readBF.cpp"
#endif
#endif
