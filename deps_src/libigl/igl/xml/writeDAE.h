// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_XML_WRITEDAE_H
#define IGL_XML_WRITEDAE_H
#include "../igl_inline.h"
#include <string>
#include <Eigen/Core>
namespace igl
{
  namespace xml
  {
    // Write a mesh to a Collada .dae scene file. The resulting scene contains
    // a single "geometry" suitable for solid operaions (boolean union,
    // intersection, etc.) in SketchUp.
    //
    // Inputs:
    //   filename  path to .dae file
    //   V  #V by 3 list of vertex positions
    //   F  #F by 3 list of face indices
    // Returns true iff success
    //
    template <typename DerivedV, typename DerivedF>
    IGL_INLINE bool writeDAE(
      const std::string & filename,
      const Eigen::PlainObjectBase<DerivedV> & V,
      const Eigen::PlainObjectBase<DerivedF> & F);
  }
}

#ifndef IGL_STATIC_LIBRARY
#include "writeDAE.cpp"
#endif
#endif
