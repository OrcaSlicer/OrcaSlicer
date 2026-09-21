// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Qingnan Zhou <qnzhou@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
//
#ifndef IGL_RELABEL_SMALL_IMMERSED_CELLS
#define IGL_RELABEL_SMALL_IMMERSED_CELLS

#include "../../igl_inline.h"
#include <Eigen/Core>
#include <vector>

namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      /// Relabel winding numbers of small immersed cells.
      ///
      /// @param[in] V  #V by 3 list of vertex positions.
      /// @param[in] F  #F by 3 list of triangle indices into V.
      /// @param[in] num_patches  number of patches
      /// @param[in] P  #F list of patch ids.
      /// @param[in] num_cells    number of cells
      /// @param[in] C  #P by 2 list of cell ids on each side of each patch.
      /// @param[in] vol_threshold  Volume threshold, cells smaller than this
      ///                  and is completely immersed will be relabeled.
      /// @param[out] W  #F by 2 cell labels.  W(i,0) is the label on the positive side of
      ///      face i, W(i,1) is the label on the negative side of face i.  W
      ///      will be modified in place by this method.
      template<
        typename DerivedV,
        typename DerivedF,
        typename DerivedP,
        typename DerivedC,
        typename FT,
        typename DerivedW>
      IGL_INLINE void relabel_small_immersed_cells(
        const Eigen::MatrixBase<DerivedV>& V,
        const Eigen::MatrixBase<DerivedF>& F,
        const size_t num_patches,
        const Eigen::MatrixBase<DerivedP>& P,
        const size_t num_cells,
        const Eigen::MatrixBase<DerivedC>& C,
        const FT vol_threashold,
        Eigen::PlainObjectBase<DerivedW>& W);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "relabel_small_immersed_cells.cpp"
#endif
#endif
