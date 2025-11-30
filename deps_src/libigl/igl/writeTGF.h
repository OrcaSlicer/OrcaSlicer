// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_WRITETGF_H
#define IGL_WRITETGF_H
#include "igl_inline.h"

#include <vector>
#include <string>
#include <Eigen/Dense>

namespace igl
{
  /// Write a graph to a .tgf file
  ///
  /// @param[in] filename  .tgf file name
  /// @param[in] V  # vertices by 3 list of vertex positions
  /// @param[in] E  # edges by 2 list of edge indices
  /// 
  /// \pre Assumes that graph vertices are 3 dimensional
  ///
  /// \see readTGF
  IGL_INLINE bool writeTGF(
    const std::string tgf_filename,
    const std::vector<std::vector<double> > & C,
    const std::vector<std::vector<int> > & E);
  /// \overload
  IGL_INLINE bool writeTGF(
    const std::string tgf_filename,
    const Eigen::MatrixXd & C,
    const Eigen::MatrixXi & E);
}

#ifndef IGL_STATIC_LIBRARY
#  include "writeTGF.cpp"
#endif

#endif

