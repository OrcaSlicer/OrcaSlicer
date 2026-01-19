// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2018 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_READMSH_H
#define IGL_READMSH_H
#include "igl_inline.h"

#include <Eigen/Dense>
#include <string>

namespace igl
{
  // Read a mesh (e.g., tet mesh) from a gmsh .msh file
  // 
  // Inputs:
  //   filename  path to .msh file
  // Outputs:
  //    V  #V by 3 list of 3D mesh vertex positions
  //    T  #T by ss list of 3D ss-element indices into V (e.g., ss=4 for tets)
  // Returns true on success
  template <
    typename DerivedV,
    typename DerivedT>
  IGL_INLINE bool readMSH(
    const std::string & filename,
    Eigen::PlainObjectBase<DerivedV> & V,
    Eigen::PlainObjectBase<DerivedT> & T);
}


#ifndef IGL_STATIC_LIBRARY
#  include "readMSH.cpp"
#endif
#endif
