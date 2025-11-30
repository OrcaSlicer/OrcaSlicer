// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_TETGEN_MESH_TO_TETGENIO_H
#define IGL_COPYLEFT_TETGEN_MESH_TO_TETGENIO_H
#include "../../igl_inline.h"

#ifndef TETLIBRARY
#  define TETLIBRARY 
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
      /// Load a vertex list and face list into a tetgenio object
      ///
      /// @param[in] V  #V by 3 vertex position list
      /// @param[in] F  #F list of polygon face indices into V (0-indexed)
      /// @param[out] in  tetgenio input object
      ///  @param[out] H  #H list of seed point inside each hole
      ///  @param[out] R  #R list of seed point inside each region  
      template <
        typename DerivedV, 
        typename DerivedF, 
        typename DerivedH, 
        typename DerivedVM, 
        typename DerivedFM, 
        typename DerivedR>
      IGL_INLINE void mesh_to_tetgenio(
        const Eigen::MatrixBase<DerivedV>& V,
        const Eigen::MatrixBase<DerivedF>& F,
        const Eigen::MatrixBase<DerivedH>& H,
        const Eigen::MatrixBase<DerivedVM>& VM,
        const Eigen::MatrixBase<DerivedFM>& FM,
        const Eigen::MatrixBase<DerivedR>& R,
        tetgenio & in);
    }
  }
}


#ifndef IGL_STATIC_LIBRARY
#  include "mesh_to_tetgenio.cpp"
#endif

#endif
