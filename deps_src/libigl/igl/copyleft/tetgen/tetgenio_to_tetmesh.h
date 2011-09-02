// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_TETGEN_TETGENIO_TO_TETMESH_H
#define IGL_COPYLEFT_TETGEN_TETGENIO_TO_TETMESH_H
#include "../../igl_inline.h"

#ifndef TETLIBRARY
#define TETLIBRARY 
#endif
#include "tetgen.h" // Defined tetgenio, REAL
#include <vector>
#include <Eigen/Core>
namespace igl
{
  namespace copyleft
  {
    namespace tetgen
    {
      // Extract a tetrahedral mesh from a tetgenio object
      // Inputs:
      //   out tetgenio output object
      // Outputs:
      //   V  #V by 3 vertex position list
      //   T  #T by 4 list of tetrahedra indices into V
      //   F  #F by 3 list of marked facets
      // Returns true on success, false on error
      IGL_INLINE bool tetgenio_to_tetmesh(
        const tetgenio & out,
        std::vector<std::vector<REAL > > & V, 
        std::vector<std::vector<int> > & T,
        std::vector<std::vector<int> > & F);
      IGL_INLINE bool tetgenio_to_tetmesh(
        const tetgenio & out,
        std::vector<std::vector<REAL > > & V, 
        std::vector<std::vector<int> > & T);
      
      // Wrapper with Eigen types
      // Templates:
      //   DerivedV  real-value: i.e. from MatrixXd
      //   DerivedT  integer-value: i.e. from MatrixXi
      template <typename DerivedV, typename DerivedT, typename DerivedF>
      IGL_INLINE bool tetgenio_to_tetmesh(
        const tetgenio & out,
        Eigen::PlainObjectBase<DerivedV>& V,
        Eigen::PlainObjectBase<DerivedT>& T,
        Eigen::PlainObjectBase<DerivedF>& F);
      template <typename DerivedV, typename DerivedT>
      IGL_INLINE bool tetgenio_to_tetmesh(
        const tetgenio & out,
        Eigen::PlainObjectBase<DerivedV>& V,
        Eigen::PlainObjectBase<DerivedT>& T);
    }
  }
}


#ifndef IGL_STATIC_LIBRARY
#  include "tetgenio_to_tetmesh.cpp"
#endif

#endif
