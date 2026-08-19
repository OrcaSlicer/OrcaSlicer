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
#include <unordered_map>
#include <Eigen/Core>
namespace igl
{
  namespace copyleft
  {
    namespace tetgen
    {
      /// Convert a tetgenio to a tetmesh
      ///
      /// @param[in] out output of tetrahedralization
      /// @param[out] V  #V by 3 list of mesh vertex positions
      /// @param[out] T  #T by 4 list of mesh tet indices into V
      /// @param[out] F  #F by 3 list of mesh triangle indices into V
      /// @param[out] TM  #T by 1 list of material indices into R
      /// @param[out] R  #TT list of region ID for each tetrahedron      
      /// @param[out] N  #TT by 4 list of indices neighbors for each tetrahedron ('n')
      /// @param[out] PT  #TV list of incident tetrahedron for a vertex ('m')
      /// @param[out] FT  #TF by 2 list of tetrahedrons sharing a triface ('nn')
      /// @param[out] num_regions Number of regions in output mesh
      ///
      /// \bug Assumes that out.numberoftetrahedronattributes == 1 or 0
      template <
        typename DerivedV, 
        typename DerivedT,
        typename DerivedF,
        typename DerivedTM,
        typename DerivedR,
        typename DerivedN,
        typename DerivedPT,
        typename DerivedFT>
      IGL_INLINE bool tetgenio_to_tetmesh(
        const tetgenio & out,
        Eigen::PlainObjectBase<DerivedV>& V,
        Eigen::PlainObjectBase<DerivedT>& T,
        Eigen::PlainObjectBase<DerivedF>& F,
        Eigen::PlainObjectBase<DerivedTM>& TM,
        Eigen::PlainObjectBase<DerivedR>& R,
        Eigen::PlainObjectBase<DerivedN>& N,
        Eigen::PlainObjectBase<DerivedPT>& PT,
        Eigen::PlainObjectBase<DerivedFT>& FT,
        int & num_regions);
    }
  }
}


#ifndef IGL_STATIC_LIBRARY
#  include "tetgenio_to_tetmesh.cpp"
#endif

#endif
