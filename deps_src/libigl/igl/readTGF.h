// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_READTGF_H
#define IGL_READTGF_H
#include "igl_inline.h"

#include <vector>
#include <string>
#ifndef IGL_NO_EIGEN
#include <Eigen/Dense>
#endif

namespace igl
{
  // READTGF
  //
  // [V,E,P,BE,CE,PE] = readTGF(filename)
  //
  // Read a graph from a .tgf file
  //
  // Input:
  //  filename  .tgf file name
  // Output:
  //  V  # vertices by 3 list of vertex positions
  //  E  # edges by 2 list of edge indices
  //  P  # point-handles list of point handle indices
  //  BE # bone-edges by 2 list of bone-edge indices
  //  CE # cage-edges by 2 list of cage-edge indices
  //  PE # pseudo-edges by 2 list of pseudo-edge indices
  // 
  // Assumes that graph vertices are 3 dimensional
  IGL_INLINE bool readTGF(
    const std::string tgf_filename,
    std::vector<std::vector<double> > & C,
    std::vector<std::vector<int> > & E,
    std::vector<int> & P,
    std::vector<std::vector<int> > & BE,
    std::vector<std::vector<int> > & CE,
    std::vector<std::vector<int> > & PE);

  #ifndef IGL_NO_EIGEN
  IGL_INLINE bool readTGF(
    const std::string tgf_filename,
    Eigen::MatrixXd & C,
    Eigen::MatrixXi & E,
    Eigen::VectorXi & P,
    Eigen::MatrixXi & BE,
    Eigen::MatrixXi & CE,
    Eigen::MatrixXi & PE);
  IGL_INLINE bool readTGF(
    const std::string tgf_filename,
    Eigen::MatrixXd & C,
    Eigen::MatrixXi & E);
  #endif
}

#ifndef IGL_STATIC_LIBRARY
#  include "readTGF.cpp"
#endif

#endif
